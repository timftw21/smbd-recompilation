#include <winsock2.h>
#include <ws2tcpip.h>
#include "xbox.h"
#include "frame_clock.h"
#include "diagnostics.h"
#include "thread_abi.h"
#include "disc.h"
#include "disc_cache.h"
#include "crypto.h"
#include "native_apis.h"
#include "native_guest_roots.h"
#include "input.h"
#include "audio.h"
#include "gpu.h"
#include <Windows.h>
#include <iphlpapi.h>
#include <bcrypt.h>
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <sstream>

namespace smbd {
namespace {
constexpr std::uint32_t success=0,invalid_handle=0xC0000008,invalid_parameter=0xC000000D,
    no_memory=0xC0000017,conflicting_addresses=0xC0000018,timeout=0x102,
    name_not_found=0xC0000034,access_denied=0xC0000022,write_protected=0xC00000A2,
    buffer_too_small=0xC0000023,no_media=0xC0000013,end_of_file=0xC0000011;
constexpr std::uint32_t page=4096,kernel_base=0x80010000,return_sentinel=0xFFFFFFF0;
constexpr std::uint32_t memory_nozero=0x800000; // Xbox MEM_NOZERO, used by its heap allocator.
std::uint32_t align_up(std::uint32_t value,std::uint32_t alignment) {
    if(!alignment || (alignment&(alignment-1)) || value>UINT32_MAX-(alignment-1))
        throw std::runtime_error("Invalid guest allocation alignment or size");
    return (value+alignment-1)&~(alignment-1);
}
struct ThreadExit { std::uint32_t status; };
std::string folded(std::string_view text) {
    std::string result(text);
    for(auto& ch:result) {if(ch=='/') ch='\\';else if(ch>='a' && ch<='z') ch-=32;}
    return result;
}
bool wildcard(std::string_view pattern,std::string_view name) {
    if(pattern=="*.*") pattern="*";
    const auto a=folded(pattern),b=folded(name);
    std::size_t i=0,j=0,star=std::string::npos,matched=0;
    while(j<b.size()) {
        if(i<a.size() && (a[i]=='?' || a[i]==b[j])) {++i;++j;}
        else if(i<a.size() && a[i]=='*') {star=i++;matched=j;}
        else if(star!=std::string::npos) {i=star+1;j=++matched;}
        else return false;
    }
    while(i<a.size() && a[i]=='*') ++i;
    return i==a.size();
}
std::uint32_t file_error(DWORD error) {
    switch(error) {
    case ERROR_FILE_NOT_FOUND:return name_not_found;case ERROR_PATH_NOT_FOUND:return 0xC000003A;
    case ERROR_ACCESS_DENIED:return access_denied;case ERROR_SHARING_VIOLATION:return 0xC0000043;
    case ERROR_ALREADY_EXISTS:case ERROR_FILE_EXISTS:return 0xC0000035;
    case ERROR_DISK_FULL:case ERROR_HANDLE_DISK_FULL:return 0xC000007F;
    case ERROR_INVALID_PARAMETER:case ERROR_INVALID_NAME:return invalid_parameter;
    case ERROR_DIR_NOT_EMPTY:return 0xC0000101;case ERROR_HANDLE_EOF:return end_of_file;
    case ERROR_WRITE_PROTECT:return write_protected;case ERROR_NOT_ENOUGH_MEMORY:return no_memory;
    case ERROR_OPERATION_ABORTED:return 0xC0000120;
    default:throw std::runtime_error(std::format("Unmapped native file error {}",error));
    }
}
std::uint32_t dos_error(std::uint32_t status) {
    using Convert=ULONG(WINAPI*)(LONG);
    static const auto convert=reinterpret_cast<Convert>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"RtlNtStatusToDosError"));
    if(!convert) throw std::runtime_error("Native NTSTATUS conversion is unavailable");
    return convert(static_cast<LONG>(status));
}
}
struct Xbox::State {
    enum class ThreadState { ready,running,waiting,ended };
    enum class WaitKind { none,critical,objects,delay };
    struct IoApc {std::uint32_t routine,context,status;};
    struct Thread {
        State* host=nullptr;
        Cpu cpu;
        void* fiber=nullptr;
        std::uint32_t object=0,tib=0,stack=0,stack_size=0,tls_size=0;
        std::uint32_t routine=0,context=0,system=0,id=0,exit_status=0;
        ThreadState state=ThreadState::ready;
        bool suspended=false,entry=false,disable_boost=false;
        unsigned suspend_count=0;
        std::uint32_t wait_object=0;
        WaitKind wait_kind=WaitKind::none;
        std::array<std::uint32_t,64> wait_objects{};
        std::uint32_t wait_count=0;
        bool wait_all=false;
        bool alertable=false;
        std::deque<IoApc> apcs;
        std::chrono::steady_clock::time_point due=std::chrono::steady_clock::time_point::max();
    };
    struct AlertableWait {
        Thread& thread;bool prior;
        AlertableWait(Thread& value,bool enabled):thread(value),prior(value.alertable){thread.alertable=enabled;}
        ~AlertableWait(){thread.alertable=prior;}
    };
    enum class AllocationKind { pool,virtual_memory,stack,cache };
    struct Allocation {
        std::uint32_t size,protect;
        std::vector<std::uint32_t> pages;
        AllocationKind kind=AllocationKind::pool;
    };
    struct OpenFile {
        DiscEntry entry{};
        std::string path,link,pattern;
        std::uint64_t position=0;
        std::uint32_t access=0,share=0,options=0;
        bool device=false,listing_loaded=false;
        HANDLE native=INVALID_HANDLE_VALUE;
        std::filesystem::path host_path;
        ~OpenFile() {if(native!=INVALID_HANDLE_VALUE) CloseHandle(native);}
        std::vector<DiscEntry> listing;
        std::size_t listing_position=0;
    };
    struct Object {
        enum class Kind { thread,event,semaphore,file,link };
        Kind kind; std::uint32_t address; Thread* thread=nullptr;
        std::shared_ptr<OpenFile> file;
    };
    struct PendingIo {
        OVERLAPPED overlap{};
        std::shared_ptr<OpenFile> file;
        HANDLE native=INVALID_HANDLE_VALUE;
        std::uint32_t file_object=0,event=0,status=0,ordinal=0,callsite=0,requested=0,buffer=0;
        Thread* issuer=nullptr;
        std::uint32_t apc_routine=0,apc_context=0;
        std::uint64_t offset=0;
        bool active=false;
        ~PendingIo() {if(overlap.hEvent) CloseHandle(overlap.hEvent);}
    };
    Xbe& image;
    Memory& memory;
    std::span<std::byte> ram;
    std::unique_ptr<Disc> disc;
    std::shared_ptr<OpenFile> disc_io;
    std::array<PendingIo,32> pending_io{};
    unsigned pending_io_count=0;
    XboxHost host;
    Input* input=nullptr;
    std::unique_ptr<Input> owned_input;
    struct InputHandle {std::uint32_t id=0,port=0;};
    std::array<InputHandle,32> input_handles{};
    std::uint32_t next_input_handle=0xF2000000;
    std::unique_ptr<Audio> audio;
    std::uint32_t audio_device=0,audio_references=0;
    float output_gain=1;
    struct AudioBuffer {
        std::uint32_t raw,descriptor,voice,identifier,references=1,data=0,size=0,owned_data=0;
        AudioFormat format;
    };
    std::map<std::uint32_t,AudioBuffer> audio_buffers;
    Renderer* graphics=nullptr;
    std::unique_ptr<Renderer> owned_graphics;
    std::unique_ptr<Gpu> gpu;
    std::uint32_t gpu_cursor=0;
    bool graphics_initialized=false;
    bool live=false;
    bool ethernet_initialized=false;
    std::uint32_t ethernet_state=0;
    DiscCache read_cache;
    std::uint32_t cache_allocation=0,cache_pages=0;
    std::set<std::uint32_t> persistent_pages;
    std::filesystem::path user_root;
    std::map<std::string,std::string> links{{"\\??\\D:","\\Device\\CdRom0"}};
    std::array<std::byte,65536> kernel_image{};
    KernelServices services;
    GuestClock clock;
    ExecutionDiagnostic diagnostic;
    std::array<std::uint64_t,512> service_calls{};
    std::uint64_t apcs_delivered=0;
    std::deque<BootIo> io_trace;
    std::map<std::uint32_t,Allocation> allocations;
    std::array<std::uint32_t,5> last_failed_allocation{};
    std::map<std::uint32_t,Object> handles;
    struct Timer { std::chrono::steady_clock::time_point due; std::uint32_t period,dpc; };
    std::map<std::uint32_t,Timer> timers;
    std::deque<std::uint32_t> dpcs;
    std::map<std::uint32_t,unsigned> object_references;
    std::map<std::uint32_t,Object> objects;
    std::map<std::uint32_t,std::uint32_t> object_pages;
    std::vector<std::unique_ptr<Thread>> threads;
    Thread* current=nullptr;
    Thread* frame_thread=nullptr;
    void* scheduler=nullptr;
    bool converted=false,stopping=false,entry_returned=false;
    std::string boundary;
    std::uint32_t next_handle=0x100,next_thread=4,heap_begin=0,gdt=0,kernel_cursor=0x1000,tick_address=0,shutdown_head=0;
    std::uint32_t thread_type=0,event_type=0,semaphore_type=0,file_type=0;
    std::uint32_t flicker_filter=0;
    bool luma_filter=false;
    std::vector<std::uint32_t> shutdown_registrations;
    std::size_t next_runnable=0;
    LARGE_INTEGER performance_frequency{};
    std::chrono::steady_clock::time_point started=std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point quantum_due;
    std::chrono::steady_clock::time_point video_started;
    std::uint64_t video_ticks=0,vblank_callbacks=0;
    FrameClock source_clock;
    FrameClock::Time frame_started{};
    double frame_debt=0,logic_fraction=0,stage_time=0;
    std::uint32_t stage_expected=UINT32_MAX;
    bool in_frame_loop=false;
    struct GoalCrossing { bool pending=false;std::uint32_t goal=0,group=0;std::uint8_t type=0; };
    std::array<std::array<GoalCrossing,2>,8> goal_crossings{};
    bool sampling_goals=false;
    FrameClock::Time timing_started{},timing_finished{};
    std::uint64_t simulation_steps=0,timing_epochs=0;
    std::uint32_t timing_mode=UINT32_MAX,timing_scene=UINT32_MAX;

    State(Xbe& image,Memory& memory,std::span<std::byte> ram,const std::filesystem::path& disc_path,XboxHost host):image(image),memory(memory),ram(ram),host(std::move(host)) {
        if(ram.size()!=64*1024*1024 || !image.supported()) throw std::runtime_error("Native Xbox requires the verified image and 64 MiB RAM");
        frame_step={};
        if(!disc_path.empty()) {
            disc=std::make_unique<Disc>(disc_path);
            const auto executable=disc->find("default.xbe");
            if(disc->sha256(executable)!=image.sha256) throw std::runtime_error("Mounted disc executable does not match the verified target");
            disc_io=std::make_shared<OpenFile>();
            disc_io->native=CreateFileW(disc_path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
                FILE_FLAG_OVERLAPPED|FILE_FLAG_RANDOM_ACCESS,nullptr);
            if(disc_io->native==INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot open mounted disc for native asynchronous reads");
        }
        user_root=std::filesystem::absolute(this->host.storage.empty()?std::filesystem::path("data/user"):this->host.storage).lexically_normal();
        if(std::filesystem::weakly_canonical(user_root)!=user_root)
            throw std::runtime_error("Native storage path crosses a reparse point");
        std::filesystem::create_directories(user_root/"TDATA");
        std::filesystem::create_directories(user_root/"UDATA");
        heap_begin=align_up(image.base+image.image_size,page);
        memory.map(kernel_base,kernel_image);
        memory.map(0x80000000U+heap_begin,ram.subspan(heap_begin));
        memory.map(0xF0000000U+heap_begin,ram.subspan(heap_begin));
        if(this->host.ultrawide) {
            // Shared original camera aspect and horizontal canvas width. Their
            // only indexed readers configure cameras/culling and the UI canvas.
            if(memory.load<std::uint32_t>(0x402428)!=0x3FE38E39U || memory.load<std::uint32_t>(0x40242C)!=0x44555555U)
                throw std::runtime_error("Original widescreen constants do not match the supported game");
            memory.store<std::uint32_t>(0x402428,std::bit_cast<std::uint32_t>(21.0f/9));
            memory.store<std::uint32_t>(0x40242C,std::bit_cast<std::uint32_t>(480.0f*21/9));
        }
        QueryPerformanceFrequency(&performance_frequency);
        clock={this,[](void* context) {
            const auto& host=*static_cast<State*>(context);
            const auto ns=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now()-host.started).count());
            return (ns/1000000000)*733333333+(ns%1000000000)*733333333/1000000000;
        }};
        services.context=this;
        services.publish_memory=[](void* context) {
            auto& state=*static_cast<State*>(context);
            state.memory.publish_writes();
            if(state.host.publish_memory) state.host.publish_memory();
        };
        bind<252>(); bind<253>(); bind<255>(); bind<258>(); bind<187>(); bind<184>(); bind<199>(); bind<217>();
        bind<165>(); bind<166>(); bind<171>(); bind<173>(); bind<179>(); bind<180>(); bind<182>();
        bind<15>(); bind<17>(); bind<23>(); bind<291>(); bind<277>(); bind<294>(); bind<306>();
        bind<301>(); bind<103>(); bind<104>(); bind<129>(); bind<160>(); bind<161>();
        bind<126>(); bind<127>(); bind<128>(); bind<143>(); bind<99>(); bind<238>(); bind<49>(); bind<47>();
        bind<107>(); bind<108>(); bind<112>(); bind<113>(); bind<119>(); bind<137>(); bind<149>(); bind<150>(); bind<97>();
        bind<145>(); bind<159>(); bind<158>(); bind<189>(); bind<193>(); bind<225>(); bind<186>(); bind<222>();
        bind<232>();bind<233>(); bind<234>(); bind<246>(); bind<250>(); bind<24>(); bind<289>(); bind<279>(); bind<269>();
        bind<304>(); bind<305>();
        bind<67>(); bind<69>(); bind<190>(); bind<196>(); bind<198>(); bind<202>(); bind<203>();
        bind<207>(); bind<210>(); bind<211>(); bind<215>(); bind<218>(); bind<219>(); bind<226>(); bind<236>();
        bind<327>();bind<328>();
        bind<2>();
        bind<167>();bind<172>();bind<178>();bind<181>();bind<37>();
        bind<124>();bind<144>();bind<224>();bind<231>();bind<151>();
        bind<260>();bind<308>();bind<335>();bind<336>();bind<337>();bind<340>();
        for(const auto& section:image.sections) {
            memory.store<std::uint32_t>(section.header_address+24,section.preloaded()?1:0);
            for(const auto pointer:{section.head_reference,section.tail_reference}) if(pointer) memory.store<std::uint16_t>(pointer,0);
        }
        for(const auto& section:image.sections) if(section.preloaded()) for(const auto pointer:{section.head_reference,section.tail_reference})
            if(pointer) memory.store<std::uint16_t>(pointer,memory.load<std::uint16_t>(pointer)+1);
        gdt=allocate(64,page,true,PAGE_READWRITE)|0x80000000U;
        memory.store<std::uint64_t>(gdt+8,0x00CF9B000000FFFF);
        memory.store<std::uint64_t>(gdt+16,0x00CF93000000FFFF);
        // This describes the native compatibility module. It contains no original
        // kernel INIT section or executable instruction stream to patch.
        IMAGE_DOS_HEADER dos{}; dos.e_magic=IMAGE_DOS_SIGNATURE; dos.e_lfanew=0x100;
        IMAGE_NT_HEADERS32 pe{};
        pe.Signature=IMAGE_NT_SIGNATURE; pe.FileHeader.Machine=IMAGE_FILE_MACHINE_I386;
        pe.FileHeader.NumberOfSections=1; pe.FileHeader.SizeOfOptionalHeader=sizeof(IMAGE_OPTIONAL_HEADER32);
        pe.OptionalHeader.Magic=IMAGE_NT_OPTIONAL_HDR32_MAGIC; pe.OptionalHeader.ImageBase=kernel_base;
        pe.OptionalHeader.SectionAlignment=page; pe.OptionalHeader.FileAlignment=512;
        pe.OptionalHeader.SizeOfImage=static_cast<DWORD>(kernel_image.size()); pe.OptionalHeader.SizeOfHeaders=page;
        IMAGE_SECTION_HEADER section{};
        std::memcpy(section.Name,".data",5); section.VirtualAddress=page;
        section.Misc.VirtualSize=static_cast<DWORD>(kernel_image.size()-page);
        section.Characteristics=IMAGE_SCN_CNT_INITIALIZED_DATA|IMAGE_SCN_MEM_READ|IMAGE_SCN_MEM_WRITE;
        std::memcpy(kernel_image.data(),&dos,sizeof(dos));
        std::memcpy(kernel_image.data()+0x100,&pe,sizeof(pe));
        std::memcpy(kernel_image.data()+0x100+sizeof(pe),&section,sizeof(section));
        for(const auto& import : image.imports) memory.store<std::uint32_t>(import.address,0xFFF00000U+import.ordinal*16);
        const auto version=export_data(324,8);
        memory.store<std::uint16_t>(version,1); memory.store<std::uint16_t>(version+4,5838);
        export_data(322,8); // Retail ABI flags; the host has no devkit peripherals.
        tick_address=export_data(156,4);
        export_data(164,4); // Cold native launch: no dashboard launch-data page.
        memory.store<std::uint32_t>(export_data(356,4),6); // Standard AV mode: native-resolution SD profile.
        shutdown_head=export_data(0,8);
        memory.store<std::uint32_t>(shutdown_head,shutdown_head); memory.store<std::uint32_t>(shutdown_head+4,shutdown_head);
        thread_type=export_data(259,28); event_type=export_data(16,28); semaphore_type=export_data(21,28);
        file_type=export_data(71,28);
        const auto name=export_data(326,64);
        constexpr std::string_view executable="\\Device\\CdRom0\\default.xbe";
        memory.store<std::uint16_t>(name,static_cast<std::uint16_t>(executable.size()));
        memory.store<std::uint16_t>(name+2,static_cast<std::uint16_t>(executable.size()+1));
        memory.store<std::uint32_t>(name+4,name+8);
        std::memcpy(memory.access(name+8,executable.size()+1),executable.data(),executable.size()+1);
        initialize_save_identity();
    }
    ~State() {
        for(auto& request:pending_io) if(request.active) {
            CancelIoEx(request.native,&request.overlap);
            DWORD transferred=0;GetOverlappedResult(request.native,&request.overlap,&transferred,TRUE);
        }
        for(auto& thread : threads) if(thread->fiber) DeleteFiber(thread->fiber);
        if(converted) ConvertFiberToThread();
    }
    template<unsigned Ordinal> static void dispatch_kernel(void* context,Cpu& cpu,Memory&) {static_cast<State*>(context)->service<Ordinal>(cpu);}
    template<unsigned Ordinal> void bind() {services.entries[Ordinal]=dispatch_kernel<Ordinal>;}
    std::uint32_t export_data(unsigned ordinal,std::uint32_t size) {
        kernel_cursor=align_up(kernel_cursor,16);
        if(size>kernel_image.size()-kernel_cursor) throw std::runtime_error("Native kernel data exceeds its module");
        const auto address=kernel_base+kernel_cursor;
        kernel_cursor+=size;
        for(const auto& import : image.imports) if(import.ordinal==ordinal) memory.store<std::uint32_t>(import.address,address);
        return address;
    }
    void initialize_save_identity() {
        const auto path=user_root/"storage.key";
        const auto attributes=GetFileAttributesW(path.c_str());
        if(attributes!=INVALID_FILE_ATTRIBUTES && (attributes&FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("Native save identity is a reparse point");
        std::array<std::byte,32> identity{};
        if(!std::filesystem::exists(path)) {
            if(BCryptGenRandom(nullptr,reinterpret_cast<PUCHAR>(identity.data()),static_cast<ULONG>(identity.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)
                throw std::runtime_error("Cannot create native save identity");
            write_text(path,std::string_view(reinterpret_cast<const char*>(identity.data()),identity.size()),false);
        }
        File file(path);
        if(file.size()!=identity.size()) throw std::runtime_error("Invalid storage.key; preserve it with your saves");
        file.read(0,identity);
        std::memcpy(memory.access(export_data(323,16),16),identity.data(),16);
        const auto certificate=image.data(u32(image.data(image.base+0x118,4),0),0x1D0);
        const auto derive=[&](std::uint32_t destination,unsigned offset) {
            const auto digest=xbox_hmac(Bytes(identity).subspan(16),Bytes(certificate).subspan(offset,16));
            std::memcpy(memory.access(destination,16),digest.data(),16);
        };
        derive(export_data(325,16),0xC0);derive(export_data(353,16),0xB0);
        const auto alternate=export_data(354,256);
        for(unsigned i=0;i<16;++i) derive(alternate+i*16,0xD0+i*16);
        diagnostic_record("{\"type\":\"storage_ready\",\"root\":"+json(path_text(user_root))+'}');
    }
    std::uint32_t allocate(std::uint32_t size,std::uint32_t alignment,bool committed,std::uint32_t protect,
                           std::uint32_t requested=0,std::uint32_t lowest=0,std::uint32_t highest=UINT32_MAX,bool clear=true,bool top_down=false) {
        const auto fail=[&] {last_failed_allocation={size,alignment,requested,lowest,highest};return 0U;};
        if(!size) return fail();
        size=align_up(size,page); alignment=std::max(alignment,page);
        const auto lower=align_up(std::max(heap_begin,lowest),alignment);
        const auto upper=std::min<std::uint64_t>(ram.size(),static_cast<std::uint64_t>(highest)+1);
        auto cursor=lower;
        if(top_down && !requested) {
            if(upper<size || upper-size<lower) return fail();
            cursor=static_cast<std::uint32_t>(upper-size)&~(alignment-1);
            for(auto it=allocations.rbegin();it!=allocations.rend();++it) {
                const auto& [base,block]=*it;
                if(cursor>=static_cast<std::uint64_t>(base)+block.size) break;
                if(static_cast<std::uint64_t>(cursor)+size<=base) continue;
                if(base<size || base-size<lower) return fail();
                cursor=(base-size)&~(alignment-1);
            }
            if(cursor<lower) return fail();
        }
        if(requested) { if(requested<heap_begin || (requested&(page-1))) return fail(); cursor=requested; }
        if(!top_down || requested) for(const auto& [base,block] : allocations) {
            if(static_cast<std::uint64_t>(cursor)+size<=base) break;
            if(cursor>=static_cast<std::uint64_t>(base)+block.size) continue;
            if(requested) return fail();
            cursor=align_up(base+block.size,alignment);
        }
        if(static_cast<std::uint64_t>(cursor)+size>ram.size() || static_cast<std::uint64_t>(cursor)+size-1>highest) return fail();
        allocations.emplace(cursor,Allocation{size,protect,std::vector<std::uint32_t>(size/page,committed?protect:0U)});
        if(committed && clear) std::memset(memory.access(cursor,size),0,size);
        return cursor;
    }
    auto allocation(std::uint32_t address) {
        address&=0x0FFFFFFF;
        auto found=allocations.upper_bound(address);
        if(found==allocations.begin()) return allocations.end();
        --found;
        return address-found->first<found->second.size?found:allocations.end();
    }
    void release_allocation(std::uint32_t base) {
        const auto found=allocations.find(base);
        if(found==allocations.end()) return;
        if(gpu) gpu->invalidate_memory(found->first,found->second.size);
        allocations.erase(found);
    }
    std::uint32_t cache_size(std::uint32_t pages) {
        if(pages>ram.size()/page) return invalid_parameter;
        const auto previous=cache_allocation,previous_pages=cache_pages;
        read_cache.storage({});
        if(previous) release_allocation(previous);
        cache_allocation=pages?allocate(pages*page,page,true,PAGE_READWRITE):0;
        if(pages && !cache_allocation) {
            cache_allocation=previous_pages?allocate(previous_pages*page,page,true,PAGE_READWRITE,previous):0;
            if(previous_pages && !cache_allocation) throw std::runtime_error("Could not restore native disc-cache reservation");
            cache_pages=previous_pages;
        } else cache_pages=pages;
        if(cache_allocation) {
            allocations.at(cache_allocation).kind=AllocationKind::cache;
            read_cache.storage(ram.subspan(cache_allocation,cache_pages*page));
        }
        return cache_pages==pages?success:no_memory;
    }
    std::uint32_t argument(const Cpu& cpu,unsigned index) { return memory.load<std::uint32_t>(cpu.registers[esp]+4+index*4); }
    void finish(Cpu& cpu,unsigned cleanup,std::optional<std::uint64_t> result=std::nullopt,bool wide=false) {
        cpu.eip=memory.load<std::uint32_t>(cpu.registers[esp]);
        cpu.registers[esp]+=4+cleanup;
        if(result) cpu.registers[eax]=static_cast<std::uint32_t>(*result);
        if(wide && result) cpu.registers[edx]=static_cast<std::uint32_t>(*result>>32);
    }
    Thread& create(std::uint32_t routine,std::uint32_t context,std::uint32_t system,std::uint32_t stack_size,
                   std::uint32_t tls_size,bool suspended,bool entry=false) {
        if(threads.size()>=64) throw NativeBoundary(current?current->cpu.eip:image.entry,"native thread limit exceeded");
        auto thread=std::make_unique<Thread>();
        thread->host=this; thread->routine=routine; thread->context=context; thread->system=system;
        thread->id=next_thread; next_thread+=4;
        thread->suspended=suspended; thread->suspend_count=suspended?1:0; thread->entry=entry;
        thread->stack_size=align_up(stack_size?stack_size:image.stack_commit,page);
        thread->stack=allocate(thread->stack_size,page,true,PAGE_READWRITE);
        const auto object_page=allocate(page,page,true,PAGE_READWRITE);
        if(!thread->stack || !object_page || tls_size+32>thread->stack_size) throw std::runtime_error("Cannot allocate a native guest thread");
        allocations.at(thread->stack).kind=AllocationKind::stack;
        static_assert(NativeThreadAbi::optional_callbacks+4<=NativeThreadAbi::tib);
        thread->object=object_page|0x80000000U; thread->tib=thread->object+NativeThreadAbi::tib; thread->tls_size=tls_size;
        memory.store<std::uint8_t>(thread->object,6);
        // StackBase includes the TLS reservation. The supplied original system
        // routine fills that reservation and its self-reference (0xE682C).
        const auto stack_base=thread->stack+thread->stack_size;
        const auto tls=stack_base-tls_size;
        memory.store<std::uint32_t>(thread->object+0x1C,stack_base);
        memory.store<std::uint32_t>(thread->object+0x20,thread->stack);
        memory.store<std::uint32_t>(thread->object+0x28,tls_size?tls:0);
        memory.store<std::uint8_t>(thread->object+0x32,8); memory.store<std::uint8_t>(thread->object+0x70,8);
        memory.store<std::uint32_t>(thread->tib,UINT32_MAX);
        memory.store<std::uint32_t>(thread->tib+4,stack_base);
        memory.store<std::uint32_t>(thread->tib+8,thread->stack);
        memory.store<std::uint32_t>(thread->tib+0x18,thread->tib);
        memory.store<std::uint32_t>(thread->tib+NativeThreadAbi::current,thread->object);
        memory.store<std::uint32_t>(thread->tib+NativeThreadAbi::tls_current,thread->object);
        auto& cpu=thread->cpu;
        cpu.kernel=&services; cpu.clock=&clock; cpu.diagnostic=live?nullptr:&diagnostic;
        cpu.ultrawide=host.ultrawide;
        cpu.preempt=[](Cpu& cpu) {
            auto& host=*static_cast<State*>(cpu.kernel->context);
            if(host.host.performance) host.host.performance->guest_sample(cpu.eip,host.current->id);
            if(!(cpu.flags&0x200) || host.memory.load<std::uint8_t>(cpu.fs_base+0x24)>1 ||
               std::chrono::steady_clock::now()<host.quantum_due) return;
            HostFloatingScope floating(cpu.floating);
            host.yield();
        };
        cpu.fs_base=thread->tib; cpu.fs_selector=0x30;
        cpu.cs_selector=8; cpu.ds_selector=cpu.ss_selector=16; cpu.gdt_base=gdt; cpu.gdt_limit=63;
        cpu.registers[esp]=(tls-16)&~15U;
        if(system) {
            memory.store<std::uint32_t>(cpu.registers[esp]+4,routine);
            memory.store<std::uint32_t>(cpu.registers[esp]+8,context);
        } else if(!entry) memory.store<std::uint32_t>(cpu.registers[esp]+4,context);
        memory.store<std::uint32_t>(cpu.registers[esp],return_sentinel);
        thread->fiber=CreateFiberEx(0,8*1024*1024,FIBER_FLAG_FLOAT_SWITCH,fiber_entry,thread.get());
        if(!thread->fiber) throw std::runtime_error("Cannot create a native thread fiber");
        auto& result=*thread;
        threads.push_back(std::move(thread));
        return result;
    }
    static void WINAPI fiber_entry(void* parameter) {
        auto& thread=*static_cast<Thread*>(parameter);
        auto& host=*thread.host;
        thread.state=ThreadState::running;
        try {
            if(host.stopping) throw ThreadExit{0xC0000120};
            invoke_native(thread.cpu,host.memory,thread.system?thread.system:thread.routine);
            if(thread.cpu.eip!=return_sentinel) throw NativeBoundary(thread.cpu.eip,"thread returned outside its original native entry");
            if(thread.entry) host.entry_returned=true;
            thread.exit_status=thread.cpu.registers[eax];
        } catch(const ThreadExit& exit) { thread.exit_status=exit.status; }
        catch(const GuestTrap& error) {
            if(!host.stopping) {
                host.boundary=error.what();
                // Xbox debug-print interrupts pass a counted ANSI string in ECX.
                // Keep stopping at the trap, but retain the game's error message.
                if(error.vector==0x2D && thread.cpu.registers[eax]==1) try {
                    const auto descriptor=thread.cpu.registers[ecx];
                    const auto length=host.memory.load<std::uint16_t>(descriptor);
                    const auto pointer=host.memory.load<std::uint32_t>(descriptor+4);
                    if(length && length<=4096)
                        host.boundary+="; "+std::string(static_cast<const char*>(host.memory.access(pointer,length)),length);
                } catch(...) {} // Do not replace the original failure with a reporting error.
            }
        }
        catch(const std::exception& error) { if(!host.stopping) host.boundary=error.what(); }
        thread.state=ThreadState::ended;
        host.memory.store<std::uint32_t>(thread.object+4,1);
        SwitchToFiber(host.scheduler);
        std::terminate();
    }
    void yield(ThreadState state=ThreadState::ready,std::uint32_t object=0,
               std::chrono::steady_clock::time_point due=std::chrono::steady_clock::time_point::max()) {
        if(memory.load<std::uint8_t>(current->tib+0x24)>1) throw NativeBoundary(current->cpu.eip,"a DPC attempted a blocking wait");
        current->state=state; current->wait_object=object; current->due=due;
        if(object) current->wait_kind=WaitKind::critical;
        {PerfScope scope(host.performance,PerfPhase::scheduler);SwitchToFiber(scheduler);}
        if(stopping) throw ThreadExit{0xC0000120};
        current->state=ThreadState::running;
        current->wait_kind=WaitKind::none;
    }
    void dispatcher(std::uint32_t address,unsigned type,unsigned size,std::uint32_t signal) {
        memory.access(address,size);
        memory.fill_elements<std::uint32_t>(address,0,size/4,false);
        memory.store<std::uint8_t>(address,static_cast<std::uint8_t>(type));
        memory.store<std::uint8_t>(address+2,static_cast<std::uint8_t>(size/4));
        memory.store<std::uint32_t>(address+4,signal);
        memory.store<std::uint32_t>(address+8,address+8); memory.store<std::uint32_t>(address+12,address+8);
    }
    std::chrono::steady_clock::time_point due_time(std::int64_t interval) {
        const auto now=std::chrono::steady_clock::now();
        std::uint64_t ticks;
        if(interval>0) {
            FILETIME time; GetSystemTimeAsFileTime(&time);
            const auto current_time=(static_cast<std::uint64_t>(time.dwHighDateTime)<<32)|time.dwLowDateTime;
            ticks=static_cast<std::uint64_t>(interval)>current_time?interval-current_time:0;
        } else ticks=0U-static_cast<std::uint64_t>(interval);
        const auto available=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::time_point::max()-now).count();
        return ticks>static_cast<std::uint64_t>(available)/100?std::chrono::steady_clock::time_point::max():
            now+std::chrono::nanoseconds(static_cast<std::int64_t>(ticks*100));
    }
    bool signalled(std::uint32_t address) {
        const auto type=memory.load<std::uint8_t>(address);
        if(type!=0 && type!=1 && type!=5 && type!=6 && type!=8 && type!=9)
            throw NativeBoundary(current?current->cpu.eip:image.entry,"unsupported guest dispatcher object type");
        return static_cast<std::int32_t>(memory.load<std::uint32_t>(address+4))>0;
    }
    void consume(std::uint32_t address) {
        const auto type=memory.load<std::uint8_t>(address);
        if(type==1 || type==9) memory.store<std::uint32_t>(address+4,0);
        else if(type==5) memory.store<std::uint32_t>(address+4,memory.load<std::uint32_t>(address+4)-1);
    }
    bool apc_ready(const Thread& thread) {
        return thread.alertable && !thread.apcs.empty() && !memory.load<std::uint8_t>(thread.tib+0x24);
    }
    void queue_apc(Thread& thread,std::uint32_t routine,std::uint32_t context,std::uint32_t status) {
        if(!routine || thread.state==ThreadState::ended) return;
        if(thread.apcs.size()>=1024) throw NativeBoundary(current?current->cpu.eip:image.entry,"native user APC queue exceeds 1024 callbacks");
        thread.apcs.push_back({routine,context,status});
    }
    void user_callback(Cpu& cpu,std::uint32_t routine,std::span<const std::uint32_t> arguments,bool caller_cleanup=false) {
        const auto saved=cpu;
        const auto bytes=0x100+static_cast<std::uint32_t>(arguments.size()*4);
        if(cpu.registers[esp]<current->stack+bytes) throw NativeBoundary(routine,"user callback exhausted its guest stack");
        const auto stack=cpu.registers[esp]-bytes;
        memory.store<std::uint32_t>(stack,return_sentinel);
        for(unsigned i=0;i<arguments.size();++i) memory.store<std::uint32_t>(stack+4+i*4,arguments[i]);
        cpu.registers[esp]=stack;cpu.floating.active=false;cpu.floating.host_context=nullptr;
        try {
            // Use the actual thread CPU so nested waits and native preemption
            // retain the callback's stack while it is running.
            invoke_native(cpu,memory,routine);
            if(cpu.eip!=return_sentinel || cpu.registers[esp]!=stack+4+(caller_cleanup?0:arguments.size()*4))
                throw NativeBoundary(routine,"user callback has an unexpected return convention");
        } catch(...) {cpu=saved;throw;}
        cpu=saved;
    }
    bool deliver_apcs() {
        auto& thread=*current;
        if(!apc_ready(thread)) return false;
        AlertableWait inactive(thread,false);
        unsigned delivered=0;
        while(!thread.apcs.empty()) {
            if(++delivered>1024) throw NativeBoundary(thread.cpu.eip,"user APC delivery exceeded its bounded work limit");
            const auto apc=thread.apcs.front();thread.apcs.pop_front();
            const std::array arguments{apc.context,apc.status,0U};
            user_callback(thread.cpu,apc.routine,arguments);++apcs_delivered;
        }
        return true;
    }
    bool waiting_ready(const Thread& thread) {
        if(apc_ready(thread)) return true;
        if(thread.wait_kind==WaitKind::critical) return !memory.load<std::uint32_t>(thread.wait_object+24);
        if(thread.wait_kind!=WaitKind::objects) return false;
        bool all=true;
        for(unsigned i=0;i<thread.wait_count;++i) {
            const bool state=signalled(thread.wait_objects[i]);
            if(state && !thread.wait_all) return true;
            all &=state;
        }
        return all;
    }
    std::uint32_t allocate_object() {
        // File, event, semaphore and link objects fit in 128-byte slots. Giving
        // each cached file its own 4 KiB page exhausted the game's 64 MiB RAM.
        auto found=std::ranges::find_if(object_pages,[](const auto& entry) {return entry.second!=UINT32_MAX;});
        if(found==object_pages.end()) {
            const auto base=allocate(page,page,true,PAGE_READWRITE);
            if(!base) return 0;
            found=object_pages.emplace(base,0).first;
        }
        const auto slot=std::countr_zero(~found->second);
        found->second|=1U<<slot;
        const auto address=found->first+slot*128;
        std::memset(memory.access(address,128),0,128);
        return address;
    }
    void release_object(std::uint32_t address) {
        if(!objects.contains(address) || object_references[address]) return;
        if(std::ranges::any_of(handles,[&](const auto& pair) {return pair.second.address==address;})) return;
        const auto physical=address&0x0FFFFFFF;
        const auto pooled=object_pages.find(physical&~(page-1));
        if(pooled==object_pages.end()) release_allocation(physical);
        else {
            pooled->second&=~(1U<<((physical&(page-1))/128));
            if(!pooled->second) {release_allocation(pooled->first);object_pages.erase(pooled);}
        }
        objects.erase(address); object_references.erase(address);
    }
    std::uint32_t wait(std::span<const std::uint32_t> addresses,bool all,std::uint32_t timeout_pointer,bool alertable) {
        if(addresses.empty() || addresses.size()>64) return invalid_parameter;
        if(all) for(unsigned i=0;i<addresses.size();++i) for(unsigned j=0;j<i;++j) if(addresses[i]==addresses[j]) return invalid_parameter;
        AlertableWait mode(*current,alertable);
        const auto due=timeout_pointer?due_time(std::bit_cast<std::int64_t>(memory.load<std::uint64_t>(timeout_pointer))):
            std::chrono::steady_clock::time_point::max();
        struct References {
            State& host; std::span<const std::uint32_t> addresses;
            ~References() { for(auto address:addresses) if(host.objects.contains(address)) {
                --host.object_references[address]; host.release_object(address);
            }}
        } references{*this,addresses};
        for(auto address:addresses) if(objects.contains(address)) ++object_references[address];
        for(;;) {
            if(alertable) {dispatch_io();if(deliver_apcs()) return 0xC0;}
            bool complete=true; std::optional<unsigned> selected;
            for(unsigned i=0;i<addresses.size();++i) {
                const bool state=signalled(addresses[i]); complete &=state;
                if(state && !selected) selected=i;
            }
            if((all && complete) || (!all && selected)) {
                if(all) for(auto address:addresses) consume(address); else consume(addresses[*selected]);
                return all?0:*selected;
            }
            if(std::chrono::steady_clock::now()>=due) return timeout;
            std::copy(addresses.begin(),addresses.end(),current->wait_objects.begin());
            current->wait_count=static_cast<std::uint32_t>(addresses.size()); current->wait_all=all;
            current->wait_kind=WaitKind::objects; yield(ThreadState::waiting,0,due);
        }
    }
    bool queue_dpc(std::uint32_t address,std::uint32_t arg1,std::uint32_t arg2) {
        memory.access(address,28);
        if(memory.load<std::uint8_t>(address+2)) return false;
        if(dpcs.size()>=1024) throw NativeBoundary(current?current->cpu.eip:image.entry,"DPC queue limit exceeded");
        memory.store<std::uint32_t>(address+20,arg1); memory.store<std::uint32_t>(address+24,arg2);
        memory.store<std::uint8_t>(address+2,1); dpcs.push_back(address); return true;
    }
    void dispatch_timers() {
        const auto now=std::chrono::steady_clock::now();
        for(auto item=timers.begin();item!=timers.end();) {
            auto& timer=item->second; const auto address=item->first;
            if(timer.due>now) {++item;continue;}
            memory.store<std::uint32_t>(address+4,1);
            if(timer.dpc) queue_dpc(timer.dpc,0,0);
            if(timer.period) {timer.due=now+std::chrono::milliseconds(timer.period);++item;}
            else {memory.store<std::uint8_t>(address+3,0);item=timers.erase(item);}
        }
    }
    void dispatch_dpcs(Thread& thread) {
        current=&thread;
        while(!dpcs.empty()) {
            const auto address=dpcs.front(); dpcs.pop_front(); memory.store<std::uint8_t>(address+2,0);
            auto callback=thread.cpu; callback.floating.active=false; callback.floating.host_context=nullptr;
            callback.registers[esp]-=64;
            const auto stack=callback.registers[esp];
            memory.store<std::uint32_t>(stack,return_sentinel); memory.store<std::uint32_t>(stack+4,address);
            for(unsigned i=0;i<3;++i) memory.store<std::uint32_t>(stack+8+i*4,memory.load<std::uint32_t>(address+16+i*4));
            const auto prior=memory.load<std::uint8_t>(thread.tib+0x24); memory.store<std::uint8_t>(thread.tib+0x24,2);
            try {
                invoke_native(callback,memory,memory.load<std::uint32_t>(address+12));
                if(callback.eip!=return_sentinel) throw NativeBoundary(callback.eip,"DPC returned outside its native continuation");
            } catch(...) {memory.store<std::uint8_t>(thread.tib+0x24,prior);throw;}
            memory.store<std::uint8_t>(thread.tib+0x24,prior);
        }
    }
    void dispatch_video(std::chrono::steady_clock::time_point now) {
        if(!host.vblank_callback_slot) return;
        const auto ns=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now-video_started).count());
        const auto ticks=(ns/1000000000)*60+(ns%1000000000)*60/1000000000;
        if(ticks==video_ticks) return;
        video_ticks=ticks;
        if(graphics_initialized) memory.store<std::uint32_t>(0x0032B6B0+0x1DC0,1);
        const auto routine=memory.load<std::uint32_t>(host.vblank_callback_slot);
        if(!routine) return;
        for(auto& thread:threads) {
            if(thread->state==ThreadState::ended || thread->suspended || !(thread->cpu.flags&0x200) ||
               memory.load<std::uint8_t>(thread->tib+0x24)>1) continue;
            current=thread.get();
            auto callback=thread->cpu;callback.preempt=nullptr;
            callback.floating.active=false;callback.floating.host_context=nullptr;
            callback.registers[esp]-=64;
            const auto stack=callback.registers[esp],data=stack+32;
            memory.store<std::uint32_t>(stack,return_sentinel);memory.store<std::uint32_t>(stack+4,data);
            memory.store<std::uint32_t>(data,static_cast<std::uint32_t>(ticks));
            memory.store<std::uint32_t>(data+4,static_cast<std::uint32_t>(gpu?gpu->stats().flips:host.frames?host.frames():0));
            memory.store<std::uint32_t>(data+8,0);
            const auto prior=memory.load<std::uint8_t>(thread->tib+0x24);memory.store<std::uint8_t>(thread->tib+0x24,2);
            try {
                invoke_native(callback,memory,routine);
                if(callback.eip!=return_sentinel || callback.registers[esp]!=stack+4)
                    throw NativeBoundary(callback.eip,"vertical-blank callback has an unexpected ABI");
            } catch(...) {memory.store<std::uint8_t>(thread->tib+0x24,prior);throw;}
            memory.store<std::uint8_t>(thread->tib+0x24,prior);
            ++vblank_callbacks;return;
        }
    }
    void clock_data() {
        const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started).count();
        memory.store<std::uint32_t>(tick_address,static_cast<std::uint32_t>(ms));
    }
    unsigned render_rate() const {
        const auto rate=host.refresh_rate?host.refresh_rate():60U;
        return rate?std::max(60U,rate):0;
    }
    void begin_frame() {
        const auto now=FrameClock::Clock::now();
        const auto mode=memory.load<std::uint32_t>(0x6C9364);
        const auto scene=memory.load<std::uint16_t>(0x6C9288);
        const bool reset=!in_frame_loop || mode!=timing_mode || scene!=timing_scene;
        in_frame_loop=true;frame_thread=current;
        if(reset) {frame_debt=logic_fraction=0;source_clock={};++timing_epochs;stage_expected=UINT32_MAX;goal_crossings={};}
        const auto rate=render_rate();
        double delta=1;
        if(!reset && rate!=60) {
            frame_debt+=std::chrono::duration<double>(now-frame_started).count()*60;
            delta=std::min(1.0,frame_debt);frame_debt-=delta;
        }
        frame_started=now;timing_mode=mode;timing_scene=scene;
        logic_fraction+=delta;
        const bool tick=logic_fraction>=1-1e-9;
        if(tick) logic_fraction=std::max(0.0,logic_fraction-1);
        frame_step={static_cast<float>(delta),static_cast<float>(logic_fraction),tick};
    }
    bool take_goal(Cpu& cpu,std::uint32_t query) {
        if(sampling_goals) return false;
        const auto ball=cpu.registers[ecx];
        if(ball<0x738DF0 || (ball-0x738DF0)%0x1B0 || (ball-0x738DF0)/0x1B0>=goal_crossings.size()) return false;
        const bool primary=query==0x12F30;
        auto& crossing=goal_crossings[(ball-0x738DF0)/0x1B0][primary?0:1];
        if(!crossing.pending) return false;
        const auto state=memory.load<std::uint8_t>(ball+3);
        if(state!=4 && state!=32) {crossing={};return false;}
        memory.store<std::uint32_t>(memory.load<std::uint32_t>(cpu.registers[esp]+4),crossing.goal);
        if(primary) {
            memory.store<std::uint32_t>(memory.load<std::uint32_t>(cpu.registers[esp]+8),crossing.group);
            memory.store<std::uint8_t>(memory.load<std::uint32_t>(cpu.registers[esp]+12),crossing.type);
        }
        crossing.pending=false;finish(cpu,0,1);return true;
    }
    void sample_goals(Cpu& cpu) {
        // Goal triggers test a swept segment. Check each actual physics step,
        // then deliver the crossing once to the original 60 Hz INFO controller.
        const auto saved=cpu.registers;const auto flags=cpu.flags,pc=cpu.eip;
        if(saved[esp]<current->stack+0x100) throw NativeBoundary(0x13180,"goal query exhausted its guest stack");
        const auto stack=saved[esp]-0x100;
        struct Sampling {bool& flag;~Sampling(){flag=false;}} sampling{sampling_goals};sampling_goals=true;
        for(unsigned i=0;i<goal_crossings.size();++i) {
            const auto ball=0x738DF0+i*0x1B0;
            const auto state=memory.load<std::uint8_t>(ball+3);
            auto& crossings=goal_crossings[i];
            const auto active=memory.load<std::uint8_t>(ball);
            if(!active || active==4 || (state!=4 && state!=32)) {crossings={};continue;}
            for(unsigned kind=0;kind<2;++kind) {
                auto& crossing=crossings[kind];
                if(crossing.pending || (!kind && (memory.load<std::uint32_t>(ball+0x98)&0x100))) continue;
                // 13D66..13D82: ECX=ball, three caller-cleaned output pointers
                // for goal ID, animation group and one-byte goal type. The
                // separate 13CEB query uses ECX and only the goal-ID pointer.
                const auto query=native_goal_queries[kind];
                memory.store<std::uint32_t>(stack,return_sentinel);
                memory.store<std::uint32_t>(stack+4,stack+20);
                memory.store<std::uint32_t>(stack+8,stack+24);
                memory.store<std::uint32_t>(stack+12,stack+28);
                cpu.registers[esp]=stack;cpu.registers[ecx]=ball;
                invoke_native(cpu,memory,query);
                if(cpu.eip!=return_sentinel || cpu.registers[esp]!=stack+4)
                    throw NativeBoundary(query,"original goal query has an unexpected ABI");
                if(cpu.registers[eax]) crossing={true,memory.load<std::uint32_t>(stack+20),kind?0:memory.load<std::uint32_t>(stack+24),kind?std::uint8_t{}:memory.load<std::uint8_t>(stack+28)};
            }
        }
        cpu.registers=saved;cpu.flags=flags;cpu.eip=pc;
    }
    void sample_stage_time(Cpu& cpu) {
        const auto count=memory.load<std::uint32_t>(0x72FF80);
        const bool advance=!(memory.load<std::uint8_t>(0x72FF9C)&5);
        if(count!=stage_expected) stage_time=static_cast<std::int32_t>(count);
        else if(advance) stage_time+=frame_step.delta;
        stage_expected=count+(advance && frame_step.logic?1U:0U);
        cpu.floating.xmm[0][0]=std::bit_cast<std::uint32_t>(static_cast<float>(stage_time));
    }
    void wait_frame(Cpu& cpu) {
        // The original common frame wrapper serves gameplay, movies, menus,
        // loading and party games. Its video-counter spin is lowered to this
        // cooperative producer wait; video callbacks retain their own clock.
        const auto mode=memory.load<std::uint32_t>(0x6C9364);
        const auto scene=memory.load<std::uint16_t>(0x6C9288);
        frame_thread=current;
        if(!in_frame_loop && (mode!=timing_mode || scene!=timing_scene)) {source_clock={};++timing_epochs;}
        if(!in_frame_loop) {timing_mode=mode;timing_scene=scene;}
        const bool main_frame=in_frame_loop && memory.load<std::uint32_t>(cpu.registers[esp])==0x66E7D;
        const auto now=FrameClock::Clock::now();source_clock.due(main_frame?render_rate():60,now);source_clock.advance(now);
        // E5990 immediately follows this wait. Active movie playback makes
        // that original function wait on the independent 60 Hz video clock.
        // Keep its SDK side effects and let it own pacing for those frames.
        const bool movie_wait=memory.load<std::uint32_t>(0x756080) && memory.load<std::uint8_t>(0x756088);
        if(!movie_wait) while(FrameClock::Clock::now()<source_clock.next()) yield(ThreadState::waiting,0,source_clock.next());
        timing_finished=FrameClock::Clock::now();if(!simulation_steps) timing_started=timing_finished;
        if(main_frame && render_rate()!=60) source_clock.discard_overrun(timing_finished);
        if(!main_frame || frame_step.logic) ++simulation_steps;
        if(in_frame_loop && !main_frame) {frame_started=timing_finished;frame_step={};}
    }
    std::string counted_string(std::uint32_t address) {
        if(!address) return {};
        const auto length=memory.load<std::uint16_t>(address),maximum=memory.load<std::uint16_t>(address+2);
        if(length>maximum) throw NativeBoundary(current->cpu.eip,"invalid counted guest string");
        if(!length) return {};
        const auto data=memory.load<std::uint32_t>(address+4);
        std::string result(reinterpret_cast<const char*>(memory.access(data,length)),length);
        if(result.find('\0')!=std::string::npos) throw NativeBoundary(current->cpu.eip,"embedded zero in guest object name");
        return result;
    }
    std::optional<std::string> object_name(std::uint32_t attributes,bool resolve=true) {
        if(!attributes) return std::nullopt;
        auto result=counted_string(memory.load<std::uint32_t>(attributes+4));
        const auto root=memory.load<std::uint32_t>(attributes);
        // Xbox ObDosDevicesDirectory() is the namespace handle -3, also
        // written by the original CreateFileA wrapper at E0A95.
        if(!result.starts_with('\\') && root==0xFFFFFFFDU) result="\\??\\"+result;
        else if(!result.starts_with('\\') && root) {
            const auto found=handles.find(root);
            if(found==handles.end() || found->second.kind!=Object::Kind::file || !found->second.file->entry.directory()) return std::nullopt;
            result=found->second.file->path+'\\'+result;
        }
        if(!result.starts_with('\\') && result.size()>=2 && result[1]==':') result="\\??\\"+result;
        for(auto& ch:result) if(ch=='/') ch='\\';
        const auto separators=[&] {
            result.erase(std::unique(result.begin(),result.end(),[](char a,char b){return a=='\\' && b=='\\';}),result.end());
        };
        separators();
        if(resolve) for(unsigned iteration=0;iteration<16;++iteration) {
            bool changed=false;
            const auto key=folded(result);
            for(const auto& [name,target]:links) if(key==name || (key.starts_with(name) && key.size()>name.size() && key[name.size()]=='\\')) {
                result=target+result.substr(name.size());separators();changed=true;break;
            }
            if(!changed) return result;
        }
        else return result;
        throw NativeBoundary(current->cpu.eip,"cyclic guest symbolic links");
    }
    std::optional<std::string> disc_path(std::string_view name) {
        constexpr std::string_view root="\\DEVICE\\CDROM0";
        const auto key=folded(name);
        if(key==root) return "";
        if(!key.starts_with(root) || key.size()<=root.size() || key[root.size()]!='\\') return std::nullopt;
        auto relative=std::string(name.substr(root.size()+1));
        if(relative.ends_with('\\')) relative.pop_back();
        std::size_t start=0;
        while(start<relative.size()) {
            const auto end=relative.find('\\',start);
            const auto part=std::string_view(relative).substr(start,end==std::string::npos?relative.size()-start:end-start);
            if(part.empty() || part=="." || part==".." || part.find(':')!=std::string_view::npos) return std::nullopt;
            if(end==std::string::npos) break;
            start=end+1;
        }
        return relative;
    }
    std::optional<std::filesystem::path> writable_path(std::string_view name) {
        constexpr std::string_view root="\\DEVICE\\HARDDISK0\\PARTITION";
        const auto key=folded(name);
        if(!key.starts_with(root) || key.size()<root.size()+1) return std::nullopt;
        const auto partition=key[root.size()];
        if(partition!='1' && (partition<'3' || partition>'5')) return std::nullopt;
        const auto prefix_size=root.size()+1;
        if(key.size()>prefix_size && key[prefix_size]!='\\') return std::nullopt;
        auto path=user_root;
        auto validate=[&](const std::filesystem::path& candidate) {
            const auto attributes=GetFileAttributesW(candidate.c_str());
            return attributes==INVALID_FILE_ATTRIBUTES || !(attributes&FILE_ATTRIBUTE_REPARSE_POINT);
        };
        if(!validate(path)) throw NativeBoundary(current->cpu.eip,"native storage root is a reparse point");
        if(partition!='1') {
            path/="CACHE";if(!validate(path)) throw NativeBoundary(current->cpu.eip,"native cache root is a reparse point");
            path/="PARTITION"+std::string(1,partition);if(!validate(path)) throw NativeBoundary(current->cpu.eip,"native cache partition is a reparse point");
        }
        std::size_t start=prefix_size+1;
        while(start<name.size()) {
            const auto end=name.find('\\',start);
            const auto part=name.substr(start,end==std::string_view::npos?name.size()-start:end-start);
            if(part.empty() || part=="." || part==".." || part.ends_with('.') || part.ends_with(' ') ||
               part.find_first_of(":*?\"<>|")!=std::string_view::npos) return std::nullopt;
            if(std::ranges::any_of(part,[](unsigned char ch) {return ch<32 || ch>126;}))
                throw NativeBoundary(current->cpu.eip,"non-ASCII writable filename requires an explicit guest encoding");
            const auto base=folded(part.substr(0,part.find('.')));
            if(base=="CON" || base=="PRN" || base=="AUX" || base=="NUL" ||
                (base.size()==4 && (base.starts_with("COM") || base.starts_with("LPT")) && base[3]>='1' && base[3]<='9')) return std::nullopt;
            path/=std::string(part);
            if(!validate(path)) throw NativeBoundary(current->cpu.eip,"writable guest path crosses a reparse point");
            if(end==std::string_view::npos) break;
            start=end+1;
        }
        return path;
    }
    void update_input() {
        if(!input) {
            input=host.input;
            if(!input) {owned_input=std::make_unique<Input>();input=owned_input.get();}
        }
        input->update();
        constexpr std::uint32_t gamepad_type=0x0034148C;
        const auto mask=input->connected(),previous=memory.load<std::uint32_t>(gamepad_type);
        memory.store<std::uint32_t>(gamepad_type+4,memory.load<std::uint32_t>(gamepad_type+4)|(previous^mask));
        memory.store<std::uint32_t>(gamepad_type,mask);
    }
    void input_api(Cpu& cpu,std::uint32_t address) {
        constexpr std::uint32_t gamepad_type=0x0034148C,remote_type=0x003414F0;
        update_input();
        const auto type=[&](std::uint32_t value) {
            if(value!=gamepad_type && value!=remote_type) throw NativeBoundary(address,"unrecognized input device type");
            memory.access(value,12);return value;
        };
        const auto last_error=[&](std::uint32_t error) {
            const auto stack_base=memory.load<std::uint32_t>(cpu.fs_base+4);
            const auto index=memory.load<std::uint32_t>(image.tls->index_address);
            const auto tls=memory.load<std::uint32_t>(stack_base+index*4);
            memory.store<std::uint32_t>(tls+8,error);
        };
        if(address==0x003417A3) {
            const auto count=argument(cpu,0),array=argument(cpu,1);
            if(count>32) throw NativeBoundary(address,"input preallocation exceeds the native device bound");
            if(count) memory.access(array,count*8);
            for(unsigned i=0;i<count;++i) type(memory.load<std::uint32_t>(array+i*8));
            memory.store<std::uint32_t>(0x009544A0,0);
            finish(cpu,8);return;
        }
        if(address==0x00342371 || address==0x00342393) {
            const auto device=type(argument(cpu,0)),current_mask=memory.load<std::uint32_t>(device);
            if(address==0x00342371) {
                memory.store<std::uint32_t>(device+4,0);memory.store<std::uint32_t>(device+8,current_mask);
                finish(cpu,4,current_mask);
            } else {
                const auto additions=argument(cpu,1),removals=argument(cpu,2);
                memory.access(additions,4);memory.access(removals,4);
                const auto changed=memory.load<std::uint32_t>(device+4),previous=memory.load<std::uint32_t>(device+8);
                const auto reconnected=changed&previous&current_mask;
                const auto added=(~previous&current_mask)|reconnected,removed=(~current_mask&previous)|reconnected;
                memory.store<std::uint32_t>(additions,added);memory.store<std::uint32_t>(removals,removed);
                memory.store<std::uint32_t>(device+4,0);memory.store<std::uint32_t>(device+8,current_mask);
                finish(cpu,12,(added|removed)!=0);
            }
            return;
        }
        if(address==0x00342242) {
            const auto device=type(argument(cpu,0)),port=argument(cpu,1),slot=argument(cpu,2),parameters=argument(cpu,3);
            if(parameters) {
                memory.access(parameters,4);
                if((memory.load<std::uint8_t>(parameters)&0xFC) || memory.load<std::uint8_t>(parameters+3)) {
                    last_error(87);finish(cpu,16,0);return;
                }
                if(!(memory.load<std::uint8_t>(parameters)&1)) throw NativeBoundary(address,"manual input polling is not yet bound");
            }
            if(device!=gamepad_type || port>=4 || slot) {last_error(87);finish(cpu,16,0);return;}
            if(!(input->connected()&(1U<<port))) {last_error(1167);finish(cpu,16,0);return;}
            const auto available=std::ranges::find(input_handles,0U,&InputHandle::id);
            if(available==input_handles.end() || next_input_handle>=0xF2FFFFFF) {last_error(8);finish(cpu,16,0);return;}
            *available={++next_input_handle,port};finish(cpu,16,available->id);return;
        }
        const auto handle=argument(cpu,0);
        const auto found=std::ranges::find(input_handles,handle,&InputHandle::id);
        const bool valid=handle && found!=input_handles.end();
        if(address==0x00342298) {
            if(!valid) throw NativeBoundary(address,"invalid native input handle");
            *found={};finish(cpu,4);return;
        }
        const auto output=argument(cpu,1);
        if(address==0x003422A4) {
            memory.access(output,22);
            if(!valid || !(input->connected()&(1U<<found->port))) {finish(cpu,8,1167);return;}
            const auto& state=input->state(found->port);
            memory.store<std::uint32_t>(output,state.packet);memory.store<std::uint16_t>(output+4,state.buttons);
            for(unsigned i=0;i<8;++i) memory.store<std::uint8_t>(output+6+i,state.analog[i]);
            for(unsigned i=0;i<4;++i) memory.store<std::uint16_t>(output+14+i*2,static_cast<std::uint16_t>(state.axes[i]));
            finish(cpu,8,0);return;
        }
        if(address==0x00342317) {
            memory.access(output,70);
            const auto event_handle=memory.load<std::uint32_t>(output+4);
            if(event_handle && (!handles.contains(event_handle) || handles.at(event_handle).kind!=Object::Kind::event)) {
                memory.store<std::uint32_t>(output,6);finish(cpu,8,6);return;
            }
            const auto status=valid?input->rumble(found->port,memory.load<std::uint16_t>(output+66),memory.load<std::uint16_t>(output+68)):1167U;
            memory.store<std::uint8_t>(output+64,0);memory.store<std::uint8_t>(output+65,6);
            memory.store<std::uint32_t>(output,status);
            if(event_handle) memory.store<std::uint32_t>(handles.at(event_handle).address+4,1);
            finish(cpu,8,status);return;
        }
        throw NativeBoundary(address,"native input API is unbound");
    }
    void graphics_helper(Cpu& cpu,std::uint32_t entry,std::uint32_t return_pc,std::span<const std::uint32_t> arguments={}) {
        if(!std::ranges::contains(native_graphics_helpers,entry))
            throw NativeBoundary(entry,"original graphics helper is not a declared native entry");
        const auto registers=cpu.registers;
        const auto flags=cpu.flags,pc=cpu.eip;
        const auto stack=registers[esp]-0x400-static_cast<std::uint32_t>(arguments.size()*4);
        memory.store<std::uint32_t>(stack,return_pc);
        for(unsigned i=0;i<arguments.size();++i) memory.store<std::uint32_t>(stack+4+i*4,arguments[i]);
        cpu.registers[esp]=stack;cpu.registers[ecx]=0x0032B6B0;
        invoke_native(cpu,memory,entry);
        if(cpu.eip!=return_pc || cpu.registers[esp]!=stack+4+arguments.size()*4)
            throw NativeBoundary(entry,"original graphics setup helper has an unexpected ABI");
        const auto result=cpu.registers[eax];
        cpu.registers=registers;cpu.registers[eax]=result;cpu.flags=flags;cpu.eip=pc;
    }
    void initialize_graphics(Cpu& cpu) {
        constexpr std::uint32_t context=0x0032B6B0;
        if(graphics || cpu.registers[ecx]!=context || memory.load<std::uint32_t>(0x0032B6A8)!=context)
            throw NativeBoundary(cpu.eip,"unsupported graphics device initialization context");
        const auto parameters=argument(cpu,0);
        memory.access(parameters,68);
        const auto width=memory.load<std::uint32_t>(parameters),height=memory.load<std::uint32_t>(parameters+4);
        const auto format=memory.load<std::uint32_t>(parameters+8),samples=memory.load<std::uint32_t>(parameters+16);
        const auto depth=memory.load<std::uint32_t>(parameters+36);
        if(!width || !height || width>4096 || height>4096 || (format!=0x12 && format!=0x1E) || (samples && samples!=0x11) ||
            (memory.load<std::uint32_t>(parameters+32) && depth!=0x2A && depth!=0x2E))
            throw NativeBoundary(cpu.eip,std::format("unsupported presentation {}x{} color {} samples {} depth {}",
                width,height,hex32(format),hex32(samples),hex32(depth)));
        graphics=host.renderer;
        if(graphics) graphics->resize(width,height);
        else {owned_graphics=std::make_unique<Renderer>(width,height);graphics=owned_graphics.get();}
        const auto raw=allocate(96,page,true,PAGE_READWRITE);
        if(!raw) {finish(cpu,4,0x8007000E);return;}
        const auto notification=raw|0x80000000U;
        // Preserve the original allocation layout and run the original pure SDK
        // setup helpers. Native D3D11 submission owns device work/completion.
        memory.store<std::uint32_t>(context+0x2488,notification);
        memory.store<std::uint32_t>(context+0x30,notification);
        memory.store<std::uint32_t>(context+0x2490,notification+32);
        memory.store<std::uint32_t>(context+0x248C,notification+64);
        graphics_helper(cpu,0x00320410,0x00323134);
        if(static_cast<std::int32_t>(cpu.registers[eax])<0) {finish(cpu,4,cpu.registers[eax]);return;}
        gpu_cursor=memory.load<std::uint32_t>(context+0x24);
        // Verified handle/class pairs from the original 3230E0 initializer.
        constexpr std::array<GraphicsObject,11> graphics_objects{{{0xD,0x97},{0xE,0x39},{0x10,0x9F},{0x11,0x62},{0x12,0x44},
            {0x13,0x57},{0x14,0x43},{0x15,0x12},{0x16,0x72},{0x18,0x19},{0x19,0x30}}};
        gpu=std::make_unique<Gpu>(*graphics,memory,notification,graphics_objects);
        gpu->flip=host.flip;
        gpu->completion=[this](std::uint32_t routine,std::uint32_t context) {
            if(!current) throw NativeBoundary(routine,"graphics completion has no active guest thread");
            const auto irql_address=current->tib+0x24;
            const auto prior=memory.load<std::uint8_t>(irql_address);
            memory.store<std::uint8_t>(irql_address,2);
            try {
                PerfScope guest(host.performance,PerfPhase::guest);
                const std::array arguments{context};user_callback(current->cpu,routine,arguments,true);
            } catch(...) {memory.store<std::uint8_t>(irql_address,prior);throw;}
            memory.store<std::uint8_t>(irql_address,prior);
        };
        memory.store<std::uint32_t>(0x0032B208,memory.load<std::uint32_t>(0x0032B208)|0xFF7F7F);
        memory.store<std::uint32_t>(context+0x94C,0x40);
        graphics_helper(cpu,0x00322830,0x0032347B);
        const std::array presentation{parameters};
        graphics_helper(cpu,0x00322B40,0x0032348A,presentation);
        if(static_cast<std::int32_t>(cpu.registers[eax])<0) {finish(cpu,4,cpu.registers[eax]);return;}
        memory.store<std::uint32_t>(context+0x794,0x0032DD28);
        const std::array shader4{4U},shader2{2U};
        graphics_helper(cpu,0x0031D5C0,0x003234EA,shader4);
        graphics_helper(cpu,0x0031D5C0,0x003234F1,shader2);
        memory.fill_elements<std::uint32_t>(0x0032DC10,0,0x46,false);
        memory.store<std::uint32_t>(0x0032DC14,0x10);
        const std::array targets{memory.load<std::uint32_t>(context+0x1A14),memory.load<std::uint32_t>(context+0x1A20)};
        graphics_helper(cpu,0x0031DC00,0x0032351C,targets);
        graphics_helper(cpu,0x00322940,0x00323521);
        const std::array clear{0U,0U,3U,0U,0x3F800000U,0U};
        graphics_helper(cpu,0x0031AF10,0x00323535,clear);
        const std::array flicker{5U},luma{0U};
        graphics_helper(cpu,0x0031D990,0x00323590,flicker);
        graphics_helper(cpu,0x0031D9E0,0x00323597,luma);
        dispatcher(context+0x1DBC,1,16,0);
        host.vblank_callback_slot=context+0x1DB8;
        graphics_initialized=true;video_started=std::chrono::steady_clock::now();
        finish(cpu,4,success);
    }
    void submit_graphics() {
        constexpr std::uint32_t context=0x0032B6B0;
        if(!gpu) throw std::runtime_error("Graphics submission precedes native device creation");
        const auto flags=memory.load<std::uint32_t>(context+8);
        const auto end=memory.load<std::uint32_t>(context+((flags&4)?0x770:0));
        const auto begin=memory.load<std::uint32_t>(context+0x24),limit=memory.load<std::uint32_t>(context+0x28);
        if(gpu_cursor<begin || end<gpu_cursor || end>limit) throw std::runtime_error("Graphics submission left its SDK push-buffer ring");
        if(end!=gpu_cursor) {
            gpu->submit(gpu_cursor,end,current && gpu->capture_status().pending?memory.load<std::uint32_t>(current->cpu.registers[esp]):0);gpu_cursor=end;
        }
    }
    void graphics_api(Cpu& cpu,std::string_view api) {
        constexpr std::uint32_t context=0x0032B6B0;
        if(api=="D3D_Initialize") {initialize_graphics(cpu);return;}
        if(!gpu) throw NativeBoundary(cpu.eip,"graphics API precedes native device creation");
        if(api=="D3D_LockTexture" || api=="D3D_LockSurface") {
            const bool texture=api=="D3D_LockTexture";
            const auto resource=argument(cpu,0),level=texture?argument(cpu,1):0U;
            const auto output=argument(cpu,texture?2:1),rectangle=argument(cpu,texture?3:2),flags=argument(cpu,texture?4:3);
            const std::array arguments{resource,0U,level,output,rectangle,flags};
            // Keep the original SDK's format, mip, rectangle and address rules.
            graphics_helper(cpu,0x00321BA0,0x0031F580,arguments);
            if(!(flags&0x80)) { // Xbox D3DLOCK_READONLY; 0x10 is NOFLUSH.
                const auto pointer=memory.load<std::uint32_t>(output+4)&0x0FFFFFFFU;
                const auto block=allocation(pointer);
                if(block==allocations.end()) throw NativeBoundary(cpu.eip,"texture write lock has no native allocation");
                gpu->invalidate_memory(block->first,block->second.size);
            }
            finish(cpu,texture?20:16);return;
        }
        if(api=="D3D_IsBusy") {finish(cpu,0,gpu->busy()?1:0);return;}
        if(api=="D3D_BlockOnTime") {submit_graphics();gpu->wait();finish(cpu,8);return;}
        if(api=="D3D_KickOff") {
            if(cpu.registers[ecx]!=context) throw NativeBoundary(cpu.eip,"unsupported SDK submission context");
            submit_graphics();finish(cpu,0);return;
        }
        if(api=="D3D_ReserveSpace") {
            const auto flags=memory.load<std::uint32_t>(context+8),cursor=memory.load<std::uint32_t>(context);
            if(flags&4) {
                const auto buffer=memory.load<std::uint32_t>(context+0x764),base=memory.load<std::uint32_t>(buffer+4);
                memory.store<std::uint32_t>(context+0x768,memory.load<std::uint32_t>(context+0x768)+cursor-base);
                memory.store<std::uint32_t>(context,base);finish(cpu,8,base);return;
            }
            const auto minimum=argument(cpu,0),maximum=argument(cpu,1);
            const auto begin=memory.load<std::uint32_t>(context+0x24),end=memory.load<std::uint32_t>(context+0x28);
            if(minimum>maximum || maximum>end-begin-0x204 || cursor<begin || cursor>end)
                throw NativeBoundary(cpu.eip,"invalid SDK push-buffer reservation");
            auto next=cursor;
            if(maximum+0x204>end-cursor) {
                submit_graphics();next=begin;gpu_cursor=begin;memory.store<std::uint32_t>(context,next);
                memory.store<std::uint32_t>(context+0x40,memory.load<std::uint32_t>(context+0x40)+1);
                memory.store<std::uint32_t>(context+0x44,cursor-begin);
            }
            memory.store<std::uint32_t>(context+4,std::min(end,next+maximum)-0x204);
            finish(cpu,8,next);return;
        }
        if(api=="D3D_SetTileNoWait") {
            const auto index=argument(cpu,0),tile=argument(cpu,1);
            if(index>=8) throw NativeBoundary(cpu.eip,"invalid SDK graphics tile index");
            const auto cache=context+0x1AC0+index*24;
            if(!tile || !memory.load<std::uint32_t>(tile+4)) memory.fill_elements<std::uint32_t>(cache,0,6,false);
            else {
                const auto pointer=memory.load<std::uint32_t>(tile+4),size=memory.load<std::uint32_t>(tile+8),pitch=memory.load<std::uint32_t>(tile+12);
                if(!size || !pitch || (pitch&63U) || allocation(pointer)==allocations.end())
                    throw NativeBoundary(cpu.eip,"tile requires allocated storage and aligned pitch");
                memory.access(pointer,size);
                if(graphics->stats().draws) throw NativeBoundary(cpu.eip,"changing a submitted tile requires resource migration");
                std::memmove(memory.access(cache,24),memory.access(tile,24),24);
                if(!(memory.load<std::uint32_t>(tile)&0x80000000U)) {
                    memory.store<std::uint32_t>(cache+16,0);memory.store<std::uint32_t>(cache+20,0);
                }
            }
            finish(cpu,8);return;
        }
        if(api=="D3D_BlockUntilVerticalBlank") {
            const auto due=video_started+std::chrono::nanoseconds(((video_ticks+1)*1000000000+59)/60);
            if(std::chrono::steady_clock::now()<due) yield(ThreadState::waiting,0,due);
            memory.store<std::uint32_t>(context+0x1DC0,0);finish(cpu,0);return;
        }
        throw NativeBoundary(cpu.eip,"native graphics API is not bound");
    }
    AudioFormat audio_format(std::uint32_t address) {
        if(!address) throw NativeBoundary(current->cpu.eip,"sound format pointer is null");
        AudioFormat result;
        result.tag=memory.load<std::uint16_t>(address);result.channels=memory.load<std::uint16_t>(address+2);
        result.rate=memory.load<std::uint32_t>(address+4);result.block_align=memory.load<std::uint16_t>(address+12);
        result.bits=memory.load<std::uint16_t>(address+14);
        if(result.tag==0x69) result.samples_per_block=memory.load<std::uint16_t>(address+18);
        return result;
    }
    void audio_mixbins(std::uint32_t identifier,std::uint32_t address,bool volumes_only=false) {
        if(!address) {
            if(volumes_only) throw std::runtime_error("Sound mix-bin volume pointer is null");
            constexpr std::array<MixBin,2> defaults{{{0,0},{1,0}}};audio->mixbins(identifier,defaults);return;
        }
        const auto count=memory.load<std::uint32_t>(address),pairs=memory.load<std::uint32_t>(address+4);
        if(count>(volumes_only?32U:8U)) throw std::runtime_error("Sound mix-bin list exceeds its capacity");
        std::array<MixBin,32> bins{};
        for(unsigned i=0;i<count;++i) bins[i]={memory.load<std::uint32_t>(pairs+i*8),std::bit_cast<std::int32_t>(memory.load<std::uint32_t>(pairs+i*8+4))};
        audio->mixbins(identifier,std::span(bins).first(count),volumes_only);
    }
    bool audio_data(AudioBuffer& buffer,std::uint32_t pointer,std::uint32_t size) {
        std::uint32_t owned=0;
        if(!pointer && size) {
            owned=allocate(size,page,true,PAGE_READWRITE);
            if(!owned) return false;
            pointer=owned;
        }
        const auto bytes=size?std::span(static_cast<const std::byte*>(memory.access(pointer,size)),size):std::span<const std::byte>{};
        audio->bind(buffer.identifier,bytes,pointer);
        if(buffer.owned_data) release_allocation(buffer.owned_data);
        buffer.data=pointer;buffer.size=size;buffer.owned_data=owned;
        // SMB Deluxe's settings layout differs from the other title's SDK:
        // sample pointer/size are +BC/+C0, then play and loop regions at +C4.
        memory.store<std::uint32_t>(buffer.descriptor+0xBC,pointer);memory.store<std::uint32_t>(buffer.descriptor+0xC0,size);
        memory.store<std::uint32_t>(buffer.descriptor+0xC4,0);memory.store<std::uint32_t>(buffer.descriptor+0xC8,size);
        memory.store<std::uint32_t>(buffer.descriptor+0xCC,0);memory.store<std::uint32_t>(buffer.descriptor+0xD0,size);
        return true;
    }
    void audio_api(Cpu& cpu,std::string_view api) {
        const auto address=cpu.eip;
        if(api=="DirectSoundCreate") {
            if(argument(cpu,0) || argument(cpu,2)) {finish(cpu,12,0x80070057);return;}
            if(!audio) {
                audio=std::make_unique<Audio>(true,host.mute_audio);
                audio->output_gain(host.mute_audio?0:output_gain);
                audio->recording(host.performance && host.performance->active());
            }
            if(!audio_device) {
                const auto raw=allocate(44,page,true,PAGE_READWRITE);
                if(!raw){finish(cpu,12,0x8007000E);return;}
                memory.store<std::uint32_t>(raw,0x00463640);memory.store<std::uint32_t>(raw+4,2);
                memory.store<std::uint32_t>(raw+16,raw+16);memory.store<std::uint32_t>(raw+20,raw+16);
                memory.store<std::uint32_t>(0x00341288,raw);audio_device=raw+8;
            }
            ++audio_references;memory.store<std::uint32_t>(argument(cpu,1),audio_device);finish(cpu,12,success);return;
        }
        if(!audio) throw NativeBoundary(address,"native sound device has not been created");
        if(const auto error=audio->failure();!error.empty()) throw NativeBoundary(address,error);
        if(api=="CDirectSound_DoWork") {
            for(const auto& [object,buffer]:audio_buffers)
                memory.store<std::uint16_t>(buffer.voice+18,static_cast<std::uint16_t>((audio->status(buffer.identifier)&1)?3:1));
            finish(cpu,4);return;
        }
        const auto object=argument(cpu,0);
        if(api=="IDirectSound_CreateSoundBuffer") {
            if(object!=audio_device || argument(cpu,3)) {finish(cpu,16,0x80070057);return;}
            const auto descriptor=argument(cpu,1),output=argument(cpu,2);
            if(memory.load<std::uint32_t>(descriptor)!=24) {finish(cpu,16,0x80070057);return;}
            const auto flags=memory.load<std::uint32_t>(descriptor+4),size=memory.load<std::uint32_t>(descriptor+8);
            if(flags&0x182000) throw NativeBoundary(address,"sound effect and submix buffers require their native routing implementation");
            const auto format=audio_format(memory.load<std::uint32_t>(descriptor+12));
            const auto identifier=audio->create(format);
            const auto raw=allocate(page,page,true,PAGE_READWRITE);
            if(!raw){audio->release(identifier);finish(cpu,16,0x8007000E);return;}
            const auto public_object=raw+28,body=raw+64,voice=raw+320;
            memory.store<std::uint32_t>(raw,0x00463670);memory.store<std::uint32_t>(raw+4,1);
            memory.store<std::uint32_t>(raw+8,audio_device-8);
            memory.store<std::uint32_t>(public_object,body);memory.store<std::uint32_t>(public_object+4,voice);
            memory.store<std::uint32_t>(body,0x00463680);memory.store<std::uint32_t>(body+4,1);
            memory.store<std::uint32_t>(body+8,flags);memory.store<std::uint16_t>(voice+18,1);
            AudioBuffer buffer{raw,body,voice,identifier};buffer.format=format;
            audio->headroom(identifier,(flags&0x10)?0:600);
            if(const auto bins=memory.load<std::uint32_t>(descriptor+16)) audio_mixbins(identifier,bins);
            if(size && !audio_data(buffer,0,size)) {
                audio->release(identifier);release_allocation(raw);finish(cpu,16,0x8007000E);return;
            }
            audio_buffers.emplace(public_object,buffer);
            memory.store<std::uint32_t>(output,public_object);finish(cpu,16,success);return;
        }
        const auto found=audio_buffers.find(object);
        if(found==audio_buffers.end()) throw NativeBoundary(address,"invalid native sound buffer");
        auto& buffer=found->second;
        const auto id=buffer.identifier;
        if(api=="IDirectSoundBuffer_Release") {
            const auto references=--buffer.references;
            if(!references){audio->release(id);if(buffer.owned_data)release_allocation(buffer.owned_data);release_allocation(buffer.raw);audio_buffers.erase(found);}
            finish(cpu,4,references);return;
        }
        if(api=="IDirectSoundBuffer_SetBufferData") {finish(cpu,12,audio_data(buffer,argument(cpu,1),argument(cpu,2))?success:0x8007000E);return;}
        if(api=="IDirectSoundBuffer_Play") {
            const auto flags=argument(cpu,3);
            if(argument(cpu,1) || argument(cpu,2) || (flags&~1U)) throw NativeBoundary(address,"unsupported sound play mode");
            audio->play(id,(flags&1)!=0);memory.store<std::uint16_t>(buffer.voice+18,3);finish(cpu,16,success);return;
        }
        if(api=="IDirectSoundBuffer_Stop") {audio->stop(id);memory.store<std::uint16_t>(buffer.voice+18,1);finish(cpu,4,success);return;}
        if(api=="IDirectSoundBuffer_GetStatus") {memory.store<std::uint32_t>(argument(cpu,1),audio->status(id));finish(cpu,8,success);return;}
        if(api=="IDirectSoundBuffer_GetCurrentPosition") {
            const auto position=audio->positions(id);
            if(argument(cpu,1)) memory.store<std::uint32_t>(argument(cpu,1),position.play);
            if(argument(cpu,2)) memory.store<std::uint32_t>(argument(cpu,2),position.write);
            finish(cpu,12,success);return;
        }
        if(api=="IDirectSoundBuffer_SetCurrentPosition") {audio->position(id,argument(cpu,1));finish(cpu,8,success);return;}
        if(api=="IDirectSoundBuffer_SetVolume") {audio->volume(id,std::bit_cast<std::int32_t>(argument(cpu,1)));finish(cpu,8,success);return;}
        if(api=="IDirectSoundBuffer_SetHeadroom") {audio->headroom(id,argument(cpu,1));finish(cpu,8,success);return;}
        if(api=="IDirectSoundBuffer_SetFrequency") {audio->frequency(id,argument(cpu,1));finish(cpu,8,success);return;}
        if(api=="IDirectSoundBuffer_SetMixBins" || api=="IDirectSoundBuffer_SetMixBinVolumes") {
            audio_mixbins(id,argument(cpu,1),api=="IDirectSoundBuffer_SetMixBinVolumes");finish(cpu,8,success);return;
        }
        if(api=="IDirectSoundBuffer_Lock") {
            auto offset=argument(cpu,1),size=argument(cpu,2);const auto flags=argument(cpu,7);
            if(flags&~3U) {finish(cpu,32,0x80070057);return;}
            if(flags&1) offset=audio->positions(id).write;
            if(flags&2) size=buffer.size;
            if(offset>buffer.size || size>buffer.size) {finish(cpu,32,0x80070057);return;}
            const auto first=std::min(size,buffer.size-offset);
            memory.store<std::uint32_t>(argument(cpu,3),buffer.data+offset);memory.store<std::uint32_t>(argument(cpu,4),first);
            if(argument(cpu,5) && argument(cpu,6)) {
                memory.store<std::uint32_t>(argument(cpu,5),first<size?buffer.data:0);memory.store<std::uint32_t>(argument(cpu,6),size-first);
            }
            finish(cpu,32,success);return;
        }
        throw NativeBoundary(address,"native sound operation is unbound");
    }
    void platform(Cpu& cpu,std::uint32_t address) {
        PerfScope scope(host.performance,PerfPhase::platform,address);
        const auto api=std::ranges::find(native_apis,address,&NativeApi::address);
        if(api!=native_apis.end() && api->name=="XMountUtilityDrive") {
            const auto title=std::format("{:08X}",image.title_id);
            const auto target="\\Device\\Harddisk0\\Partition3\\"+title;
            const auto path=writable_path(target);
            if(!path) throw NativeBoundary(address,"native utility drive has no valid storage path");
            const auto parent=user_root/"CACHE"/"PARTITION3";
            std::filesystem::create_directories(parent);
            if(argument(cpu,0) && std::filesystem::exists(*path)) {
                if(std::filesystem::weakly_canonical(path->parent_path())!=parent || path->filename()!=title)
                    throw NativeBoundary(address,"utility cache cleanup leaves its title directory");
                unsigned count=0;
                for(const auto& entry:std::filesystem::recursive_directory_iterator(*path))
                    if(++count>4096 || (GetFileAttributesW(entry.path().c_str())&FILE_ATTRIBUTE_REPARSE_POINT))
                        throw NativeBoundary(address,"utility cache cleanup is not bounded to ordinary title files");
                std::filesystem::remove_all(*path);
            }
            std::filesystem::create_directories(*path);
            const auto [link,inserted]=links.emplace("\\??\\Z:",target);
            if(!inserted && folded(link->second)!=folded(target)) throw NativeBoundary(address,"utility drive is mounted to another title");
            finish(cpu,4,1);return;
        }
        if(api!=std::end(native_apis)) {
            if(api->name.starts_with("DirectSound") || api->name.starts_with("IDirectSound") || api->name=="CDirectSound_DoWork") {audio_api(cpu,api->name);return;}
            if(api->name.starts_with("D3D_")) {graphics_api(cpu,api->name);return;}
            if(api->name.starts_with("XInput") || api->name=="XGetDevices" || api->name=="XGetDeviceChanges" || api->name=="XInitDevices") {
                input_api(cpu,address);return;
            }
        }
        if(!host.platform) throw NativeBoundary(address,"native platform function is not bound");
        host.platform(cpu,memory,address);
    }
    std::uint32_t open_writable(Cpu& cpu,const std::string& name,unsigned options_index,unsigned share_index,std::uint32_t disposition,std::uint32_t& information) {
        const auto path=writable_path(name);
        if(!path) return name_not_found;
        const auto access=argument(cpu,1),options=argument(cpu,options_index),share=argument(cpu,share_index);
        if(disposition>5 || share>7) return invalid_parameter;
        auto attributes=GetFileAttributesW(path->c_str());
        const bool existed=attributes!=INVALID_FILE_ATTRIBUTES;
        if((options&1) && !existed && (disposition==2 || disposition==3)) {
            if(!CreateDirectoryW(path->c_str(),nullptr)) return file_error(GetLastError());
            attributes=GetFileAttributesW(path->c_str());
        } else if((options&1) && existed && disposition==2) return 0xC0000035;
        const bool directory=attributes!=INVALID_FILE_ATTRIBUTES && (attributes&FILE_ATTRIBUTE_DIRECTORY)!=0;
        if((options&1) && !directory) return existed?0xC0000103:0xC000003A;
        if((options&0x40) && directory) return 0xC00000BA;
        constexpr DWORD creation[]={CREATE_ALWAYS,OPEN_EXISTING,CREATE_NEW,OPEN_ALWAYS,TRUNCATE_EXISTING,CREATE_ALWAYS};
        const auto mode=directory?OPEN_EXISTING:creation[disposition];
        const auto flags=FILE_FLAG_BACKUP_SEMANTICS|((options&0x1000)?FILE_FLAG_DELETE_ON_CLOSE:0U)|
            ((options&0x30)?0U:FILE_FLAG_OVERLAPPED);
        const auto native=CreateFileW(path->c_str(),access,share,nullptr,mode,flags,nullptr);
        if(native==INVALID_HANDLE_VALUE) return file_error(GetLastError());
        auto file=std::make_shared<OpenFile>();file->native=native;file->host_path=*path;
        file->path=name;file->access=access;file->share=share;file->options=options;
        file->entry.attributes=static_cast<std::uint8_t>(directory?0x10:0x20);
        const auto base=allocate_object();
        if(!base) return no_memory;
        const auto address=base|0x80000000U,handle=next_handle;next_handle+=4;
        memory.store<std::uint16_t>(address,5);
        memory.store<std::uint8_t>(address+2,static_cast<std::uint8_t>(((access&(0x80000000U|1U))?2:0)|((access&(0x40000000U|6U))?4:0)|((share&7)<<4)));
        dispatcher(address+0x28,1,16,1);dispatcher(address+0x38,0,16,1);
        const Object object{Object::Kind::file,address,nullptr,file};handles.emplace(handle,object);objects.emplace(address,object);
        memory.store<std::uint32_t>(argument(cpu,0),handle);
        information=!existed?2:disposition==0?0:disposition==4 || disposition==5?3:1;
        return success;
    }
    std::uint32_t convert_string(Cpu& cpu,bool to_unicode) {
        const auto destination=argument(cpu,0),source=argument(cpu,1);
        const auto length=memory.load<std::uint16_t>(source),maximum=memory.load<std::uint16_t>(source+2);
        const auto input_address=memory.load<std::uint32_t>(source+4);
        if(length>maximum || (to_unicode?false:(length&1)!=0)) return invalid_parameter;
        const auto bytes=length?memory.access(input_address,length):nullptr;
        int characters=0;
#ifdef _WIN32
        if(length) characters=to_unicode?MultiByteToWideChar(1252,0,static_cast<const char*>(bytes),length,nullptr,0):
            WideCharToMultiByte(1252,0,static_cast<const wchar_t*>(bytes),length/2,nullptr,0,nullptr,nullptr);
        if(length && !characters) return invalid_parameter;
#else
        characters=to_unicode?length:length/2;
#endif
        const auto unit=to_unicode?2U:1U,required=(static_cast<unsigned>(characters)+1)*unit;
        if(required>UINT16_MAX) return 0xC0000106;
        auto output=memory.load<std::uint32_t>(destination+4);
        if(argument(cpu,2)) {
            output=allocate(required,page,true,PAGE_READWRITE);
            if(!output) return no_memory;
            output|=0x80000000U;
            memory.store<std::uint16_t>(destination+2,static_cast<std::uint16_t>(required));
            memory.store<std::uint32_t>(destination+4,output);
        } else if(memory.load<std::uint16_t>(destination+2)<required) return 0x80000005;
        auto target=memory.access(output,required);
        if(characters) {
#ifdef _WIN32
            const auto written=to_unicode?MultiByteToWideChar(1252,0,static_cast<const char*>(bytes),length,static_cast<wchar_t*>(target),characters):
                WideCharToMultiByte(1252,0,static_cast<const wchar_t*>(bytes),length/2,static_cast<char*>(target),characters,nullptr,nullptr);
            if(written!=characters) return invalid_parameter;
#else
            constexpr std::array<char16_t,32> cp1252{0x20AC,0x81,0x201A,0x192,0x201E,0x2026,0x2020,0x2021,0x2C6,0x2030,0x160,0x2039,0x152,0x8D,0x17D,0x8F,
                0x90,0x2018,0x2019,0x201C,0x201D,0x2022,0x2013,0x2014,0x2DC,0x2122,0x161,0x203A,0x153,0x9D,0x17E,0x178};
            for(unsigned i=0;i<unsigned(characters);++i) {
                if(to_unicode) {
                    const auto value=static_cast<const unsigned char*>(bytes)[i];
                    const auto converted=char16_t(value>=0x80 && value<0xA0?cp1252[value-0x80]:value);
                    std::memcpy(static_cast<std::byte*>(target)+i*2,&converted,2);
                } else {
                    char16_t value;std::memcpy(&value,static_cast<const std::byte*>(bytes)+i*2,2);
                    const auto found=std::ranges::find(cp1252,value);
                    // Report unsupported best-fit mappings instead of accepting
                    // a different filename or overwriting another save.
                    if(value>255 && found==cp1252.end()) return invalid_parameter;
                    static_cast<unsigned char*>(target)[i]=found!=cp1252.end()?static_cast<unsigned char>(0x80+std::distance(cp1252.begin(),found)):static_cast<unsigned char>(value);
                }
            }
#endif
        }
        std::memset(static_cast<std::byte*>(target)+required-unit,0,unit);
        memory.store<std::uint16_t>(destination,static_cast<std::uint16_t>(required-unit));
        return success;
    }
    void io_status(std::uint32_t address,std::uint32_t status,std::uint32_t information=0) {
        memory.store<std::uint32_t>(address,status); memory.store<std::uint32_t>(address+4,information);
    }
    void trace_io(Cpu& cpu,unsigned ordinal,std::uint32_t status,std::uint32_t information,const std::string& path,
                  std::uint32_t options=0,std::uint32_t requested=0,std::uint64_t offset=0,std::uint32_t buffer=0) {
        if(host.trace_storage && folded(path).starts_with("\\DEVICE\\HARDDISK"))
            diagnostic_record(std::format("{{\"type\":\"storage_io\",\"ordinal\":{},\"path\":{},\"status\":{},\"bytes\":{},\"requested\":{},\"options_or_class\":{},\"offset\":{},\"callsite\":{}}}",
                ordinal,json(path),json(hex32(status)),information,requested,options,offset,json(hex32(memory.load<std::uint32_t>(cpu.registers[esp])))));
        if(live && !host.trace_io) return;
        if(io_trace.size()==64) io_trace.pop_front();
        io_trace.push_back({ordinal,status,information,memory.load<std::uint32_t>(cpu.registers[esp]),path,options,requested,offset,buffer});
    }
    bool valid_io(Cpu& cpu) {
        const auto event=argument(cpu,1);
        if(!event) return true;
        const auto found=handles.find(event);
        return found!=handles.end() && found->second.kind==Object::Kind::event;
    }
    void io_complete(Cpu& cpu,std::uint32_t status,std::uint32_t information=0) {
        const auto event=argument(cpu,1);
        io_status(argument(cpu,4),status,information);
        if(event) memory.store<std::uint32_t>(handles.at(event).address+4,1);
        if((status>>30)!=3) queue_apc(*current,argument(cpu,2),argument(cpu,3),argument(cpu,4));
    }
    std::uint32_t queue_io(Cpu& cpu,const Object& object,bool write,std::uint64_t offset) {
        auto& file=*object.file;
        const auto output=argument(cpu,5),length=argument(cpu,6),status=argument(cpu,4),event=argument(cpu,1);
        const auto fail=[&](std::uint32_t result) {
            io_complete(cpu,result);trace_io(cpu,write?236:219,result,0,file.path,file.options,length,offset,output);return result;
        };
        if(offset>INT64_MAX) return fail(invalid_parameter);
        if(file.entry.directory() && !file.device) return fail(0xC00000BA);
        if(write && file.native==INVALID_HANDLE_VALUE) return fail(write_protected);
        if(!write && !(file.access&(0x80000000U|1U))) return fail(access_denied);
        const auto sector=file.native!=INVALID_HANDLE_VALUE?512U:2048U;
        // Original callers use ordinary heap buffers.
        // Enforce sector transfers without imposing Windows DMA alignment on
        // the guest's pointer; our mounted-image handle is buffered.
        if((file.options&8) && ((offset%sector) || (length%sector))) return fail(invalid_parameter);
        memory.access(status,8);const auto buffer=memory.access(output,length);
        const auto size=file_size(file);
        if(!length) {io_complete(cpu,success);return success;}
        if(!write && offset>=size) return fail(end_of_file);
        const auto count=write?length:static_cast<std::uint32_t>(std::min<std::uint64_t>(length,size-offset));
        const auto disc_offset=offset+(file.device?0:std::uint64_t(file.entry.sector)*2048);
        if(!write && file.native==INVALID_HANDLE_VALUE &&
            read_cache.read(disc_offset,std::span(static_cast<std::byte*>(buffer),count))) {
            io_complete(cpu,success,count);
            memory.store<std::uint32_t>(object.address+0x38+4,1);
            trace_io(cpu,219,success,count,file.path,file.options,length,offset,output);
            return success;
        }
        auto request=std::ranges::find(pending_io,false,&PendingIo::active);
        if(request==pending_io.end()) throw NativeBoundary(cpu.eip,"native asynchronous file queue exceeds 32 requests");
        if(!request->overlap.hEvent) {
            request->overlap.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
            if(!request->overlap.hEvent) return fail(no_memory);
        }
        const auto native_event=request->overlap.hEvent;
        request->overlap={};request->overlap.hEvent=native_event;ResetEvent(native_event);
        request->file=object.file;request->file_object=object.address;
        request->event=event?handles.at(event).address:0;request->status=status;
        request->issuer=current;request->apc_routine=argument(cpu,2);request->apc_context=argument(cpu,3);
        request->ordinal=write?236:219;request->callsite=memory.load<std::uint32_t>(cpu.registers[esp]);
        request->offset=offset;request->requested=length;request->buffer=output;
        request->native=file.native!=INVALID_HANDLE_VALUE?file.native:disc_io->native;
        const auto native_offset=offset+(file.native!=INVALID_HANDLE_VALUE || file.device?0:std::uint64_t(file.entry.sector)*2048);
        request->overlap.Offset=static_cast<DWORD>(native_offset);request->overlap.OffsetHigh=static_cast<DWORD>(native_offset>>32);
        io_status(status,0x103);memory.store<std::uint32_t>(object.address+0x38+4,0);
        if(request->event) memory.store<std::uint32_t>(request->event+4,0);
        const auto issued=write?WriteFile(request->native,buffer,count,nullptr,&request->overlap):
            ReadFile(request->native,buffer,count,nullptr,&request->overlap);
        if(!issued && GetLastError()!=ERROR_IO_PENDING) {
            const auto result=file_error(GetLastError());request->file.reset();return fail(result);
        }
        request->active=true;++pending_io_count;
        trace_io(cpu,request->ordinal,0x103,0,file.path,file.options,length,offset,output);
        return 0x103;
    }
    void dispatch_io() {
        if(!pending_io_count) return;
        for(auto& request:pending_io) if(request.active) {
            DWORD transferred=0;
            const auto completed=GetOverlappedResult(request.native,&request.overlap,&transferred,FALSE);
            const auto error=completed?ERROR_SUCCESS:GetLastError();
            if(error==ERROR_IO_INCOMPLETE) continue;
            const auto result=completed?success:file_error(error);
            if(completed && request.ordinal==219 && request.file->native==INVALID_HANDLE_VALUE) {
                const auto offset=request.offset+(request.file->device?0:std::uint64_t(request.file->entry.sector)*2048);
                read_cache.insert(offset,Bytes(static_cast<const std::byte*>(memory.access(request.buffer,transferred)),transferred));
            }
            io_status(request.status,result,transferred);
            memory.store<std::uint32_t>(request.file_object+0x38+4,1);
            if(request.event) memory.store<std::uint32_t>(request.event+4,1);
            queue_apc(*request.issuer,request.apc_routine,request.apc_context,request.status);
            if(!live || host.trace_io) {
                if(io_trace.size()==64) io_trace.pop_front();
                io_trace.push_back({request.ordinal,result,transferred,request.callsite,request.file->path,
                    request.file->options,request.requested,request.offset,request.buffer});
            }
            request.file.reset();request.active=false;--pending_io_count;
        }
    }
    std::uint32_t open_file(Cpu& cpu,unsigned options_index,unsigned share_index,std::uint32_t disposition,std::uint32_t& information) {
        const auto name=object_name(argument(cpu,2));
        if(!name) return invalid_parameter;
        const auto relative=disc_path(*name);
        if(!relative) return open_writable(cpu,*name,options_index,share_index,disposition,information);
        if(!disc) return no_media;
        const auto entry=disc->lookup(*relative);
        if(!entry) return name_not_found;
        const auto access=argument(cpu,1),options=argument(cpu,options_index),share=argument(cpu,share_index);
        if(disposition>5 || share>7) return invalid_parameter;
        if(disposition==2) return 0xC0000035; // FILE_CREATE on an existing path.
        if(disposition!=1 && disposition!=3) return write_protected;
        if(access&(0x40000000U|0x00010000U|0x00000116U)) return access_denied;
        if((options&1) && !entry->directory()) return 0xC0000103;
        if((options&0x40) && entry->directory()) return 0xC00000BA;
        if(options&0x1000) return write_protected; // Delete on close.
        for(const auto& [handle,existing]:handles) if(existing.kind==Object::Kind::file && folded(existing.file->path)==folded(*name)) {
            if((access&(0x80000000U|1U)) && !(existing.file->share&1)) return 0xC0000043;
            if((existing.file->access&(0x80000000U|1U)) && !(share&1)) return 0xC0000043;
        }
        const auto base=allocate_object();
        if(!base) return no_memory;
        const auto address=base|0x80000000U,handle=next_handle; next_handle+=4;
        auto file=std::make_shared<OpenFile>();file->entry=*entry;file->path=*name;
        file->access=access;file->share=share;file->options=options;file->device=relative->empty();
        memory.store<std::uint16_t>(address,5);
        memory.store<std::uint8_t>(address+2,static_cast<std::uint8_t>(((access&(0x80000000U|1U))?2:0)|((share&7)<<4)));
        // Xbox FILE_OBJECT uses four-byte packing: byte offset at +0x14,
        // lock event at +0x28 and completion event at +0x38.
        dispatcher(address+0x28,1,16,1);dispatcher(address+0x38,0,16,1);
        const Object object{Object::Kind::file,address,nullptr,file};
        handles.emplace(handle,object);objects.emplace(address,object);
        memory.store<std::uint32_t>(argument(cpu,0),handle);
        information=1;
        return success;
    }
    std::uint64_t file_size(const OpenFile& file) const {
        if(file.native!=INVALID_HANDLE_VALUE && !file.entry.directory()) {
            LARGE_INTEGER size;if(!GetFileSizeEx(file.native,&size)) throw std::runtime_error("Cannot query native file size");
            return static_cast<std::uint64_t>(size.QuadPart);
        }
        return file.device?disc->size():file.entry.directory()?0:file.entry.size;
    }
    std::uint32_t file_information(OpenFile& file,std::uint32_t output,std::uint32_t length,unsigned type,std::uint32_t& written) {
        unsigned required;
        switch(type) {
        case 4:required=36;break;case 5:required=24;break;case 6:required=8;break;case 7:case 8:case 16:case 17:required=4;break;
        case 9:required=4+static_cast<unsigned>(file.path.size());break;case 14:case 19:case 20:required=8;break;case 34:required=52;break;
        default:throw NativeBoundary(current->cpu.eip,std::format("unsupported file information class {}",type));
        }
        if(length<required) return buffer_too_small;
        auto data=static_cast<std::byte*>(memory.access(output,required));std::memset(data,0,required);
        const auto size=file_size(file);
        const auto unit=file.native!=INVALID_HANDLE_VALUE?16384ULL:2048ULL,allocated=(size+unit-1)&~(unit-1);
        auto attributes=static_cast<std::uint32_t>(file.entry.attributes)|(file.native==INVALID_HANDLE_VALUE?1U:0U);
        if(file.native!=INVALID_HANDLE_VALUE && (type==4 || type==34)) {
            BY_HANDLE_FILE_INFORMATION native{};
            if(!GetFileInformationByHandle(file.native,&native)) return file_error(GetLastError());
            attributes=native.dwFileAttributes;
            const FILETIME times[]={native.ftCreationTime,native.ftLastAccessTime,native.ftLastWriteTime,native.ftLastWriteTime};
            for(unsigned i=0;i<4;++i) memory.store<std::uint64_t>(output+i*8,(static_cast<std::uint64_t>(times[i].dwHighDateTime)<<32)|times[i].dwLowDateTime);
        }
        switch(type) {
        case 4:memory.store<std::uint32_t>(output+32,attributes);break;
        case 5:memory.store<std::uint64_t>(output,allocated);memory.store<std::uint64_t>(output+8,size);
            memory.store<std::uint32_t>(output+16,1);memory.store<std::uint8_t>(output+21,file.entry.directory());break;
        case 6:
            if(file.native!=INVALID_HANDLE_VALUE) {
                BY_HANDLE_FILE_INFORMATION native{};
                if(!GetFileInformationByHandle(file.native,&native)) return file_error(GetLastError());
                memory.store<std::uint64_t>(output,(static_cast<std::uint64_t>(native.nFileIndexHigh)<<32)|native.nFileIndexLow);
            } else memory.store<std::uint64_t>(output,file.entry.sector);
            break;
        case 8:memory.store<std::uint32_t>(output,file.access);break;
        case 9:memory.store<std::uint32_t>(output,static_cast<std::uint32_t>(file.path.size()));
            std::memcpy(data+4,file.path.data(),file.path.size());break;
        case 14:memory.store<std::uint64_t>(output,file.position);break;
        case 16:memory.store<std::uint32_t>(output,file.options);break;
        case 17:memory.store<std::uint32_t>(output,file.native!=INVALID_HANDLE_VALUE?511:2047);break;
        case 19:memory.store<std::uint64_t>(output,allocated);break;
        case 20:memory.store<std::uint64_t>(output,size);break;
        case 34:memory.store<std::uint64_t>(output+32,allocated);memory.store<std::uint64_t>(output+40,size);
            memory.store<std::uint32_t>(output+48,attributes);break;
        }
        written=required;return success;
    }
    std::uint32_t disc_control(Cpu& cpu,OpenFile& file,std::uint32_t& written) {
        if(!file.device) return invalid_parameter;
        const auto code=argument(cpu,5),request=argument(cpu,6),input_length=argument(cpu,7),output=argument(cpu,8),output_length=argument(cpu,9);
        if(code==0x4D014) {
            if(input_length<44 || memory.load<std::uint16_t>(request)!=44) return invalid_parameter;
            memory.access(request,44);
            const auto opcode=memory.load<std::uint8_t>(request+28);
            const auto mode=memory.load<std::uint8_t>(request+30)&0x3FU;
            // READ DVD STRUCTURE requests physical optical/security data absent
            // from a partition-only XISO. Return the real unsupported status;
            // the original caller decides whether it can continue.
            if(opcode==0xAD) return 0xC00000BB;
            if(opcode!=0x5A || mode!=0x3E) throw NativeBoundary(cpu.eip,std::format("unsupported virtual disc SCSI command 0x{:02X}, page 0x{:02X}",opcode,mode));
            if(memory.load<std::uint8_t>(request+8)!=1) return invalid_parameter;
            const auto capacity=memory.load<std::uint32_t>(request+12),buffer=memory.load<std::uint32_t>(request+20);
            const auto requested=(memory.load<std::uint8_t>(request+35)<<8)|memory.load<std::uint8_t>(request+36);
            const auto count=std::min({capacity,static_cast<unsigned>(requested),28U});
            // A mounted partition-only XISO is an already readable virtual Xbox
            // data partition. This reports that provider's state, not a physical
            // drive's authentication transcript or fabricated security sector.
            std::array<std::byte,28> mode_page{};
            mode_page[1]=std::byte{26};mode_page[8]=std::byte{0x3E};mode_page[9]=std::byte{18};
            mode_page[10]=mode_page[11]=mode_page[12]=std::byte{1};mode_page[13]=std::byte{0xD0};
            if(count) std::memcpy(memory.access(buffer,count),mode_page.data(),count);
            memory.store<std::uint8_t>(request+2,0);memory.store<std::uint32_t>(request+12,count);
            if(output_length) {if(output_length<44) return buffer_too_small;std::memmove(memory.access(output,44),memory.access(request,44),44);}
            written=44;return success;
        }
        throw NativeBoundary(cpu.eip,std::format("unsupported virtual disc IOCTL 0x{:08X}",code));
    }
    template<unsigned Ordinal> void service(Cpu& cpu) {
        PerfScope scope(host.performance,PerfPhase::kernel,Ordinal);
        ++service_calls[Ordinal]; clock_data();
        if constexpr(Ordinal==327 || Ordinal==328) {
            const auto pointer=argument(cpu,0);
            const auto section=std::ranges::find(image.sections,pointer,&Section::header_address);
            if(section==image.sections.end()) {finish(cpu,4,invalid_parameter);return;}
            const auto count=memory.load<std::uint32_t>(pointer+24);
            if constexpr(Ordinal==327) {
                if(count==UINT32_MAX) {finish(cpu,4,invalid_parameter);return;}
                if(!count) {
                    image.load_section(*section,std::span(static_cast<std::byte*>(memory.access(section->address,section->size)),section->size));
                    for(const auto reference:{section->head_reference,section->tail_reference}) if(reference)
                        memory.store<std::uint16_t>(reference,memory.load<std::uint16_t>(reference)+1);
                }
                memory.store<std::uint32_t>(pointer+24,count+1);
            } else {
                if(!count) {finish(cpu,4,invalid_parameter);return;}
                memory.store<std::uint32_t>(pointer+24,count-1);
                if(count==1) {
                    std::memset(memory.access(section->address,section->size),0,section->size);
                    for(const auto reference:{section->head_reference,section->tail_reference}) if(reference)
                        memory.store<std::uint16_t>(reference,memory.load<std::uint16_t>(reference)-1);
                }
            }
            finish(cpu,4,success);
        } else if constexpr(Ordinal==190 || Ordinal==202) {
            std::uint32_t information=0;
            const auto result=open_file(cpu,Ordinal==190?8:5,Ordinal==190?6:4,Ordinal==190?argument(cpu,7):1U,information);
            trace_io(cpu,Ordinal,result,information,object_name(argument(cpu,2)).value_or("<invalid object attributes>"));
            io_status(argument(cpu,3),result,information);finish(cpu,Ordinal==190?36:24,result);
        } else if constexpr(Ordinal==219 || Ordinal==236 || Ordinal==196 || Ordinal==207) {
            constexpr unsigned cleanup=Ordinal==219 || Ordinal==236?32:40;
            if(!valid_io(cpu)) {io_status(argument(cpu,4),invalid_handle);finish(cpu,cleanup,invalid_handle);return;}
            const auto found=handles.find(argument(cpu,0));
            if(found==handles.end() || found->second.kind!=Object::Kind::file) {io_complete(cpu,invalid_handle);finish(cpu,cleanup,invalid_handle);return;}
            auto& file=*found->second.file;
            std::uint32_t result=success,written=0,trace_buffer=0,trace_requested=0;
            std::uint64_t trace_offset=0;
            if constexpr(Ordinal==219 || Ordinal==236) {
                const auto output=argument(cpu,5),length=argument(cpu,6),offset_pointer=argument(cpu,7);
                auto offset=offset_pointer?memory.load<std::uint64_t>(offset_pointer):UINT64_MAX-1;
                if(offset==UINT64_MAX-1) offset=file.position;
                trace_buffer=output;trace_requested=length;trace_offset=offset;
                if(!(file.options&0x30)) {
                    if constexpr(Ordinal==236) if(offset==UINT64_MAX) offset=file_size(file);
                    finish(cpu,cleanup,queue_io(cpu,found->second,Ordinal==236,offset));return;
                }
                if(file.native!=INVALID_HANDLE_VALUE) {
                    if constexpr(Ordinal==236) if(offset==UINT64_MAX) offset=file_size(file);
                    if(offset>INT64_MAX) result=invalid_parameter;
                    else if(file.entry.directory()) result=0xC00000BA;
                    else {
                        const LARGE_INTEGER position{.QuadPart=static_cast<LONGLONG>(offset)};
                        if(!SetFilePointerEx(file.native,position,nullptr,FILE_BEGIN)) result=file_error(GetLastError());
                        else {
                            const auto buffer=memory.access(output,length);
                            BOOL completed;DWORD native_written=0;
                            if constexpr(Ordinal==219) completed=ReadFile(file.native,buffer,length,&native_written,nullptr);
                            else completed=WriteFile(file.native,buffer,length,&native_written,nullptr);
                            written=native_written;
                            if(!completed) result=file_error(GetLastError());
                            else if constexpr(Ordinal==219) {if(!written && length) result=end_of_file;}
                            if(completed) {file.position=offset+written;memory.store<std::uint64_t>(found->second.address+0x14,file.position);}
                        }
                    }
                } else if constexpr(Ordinal==236) result=write_protected;
                else if(offset>INT64_MAX) result=invalid_parameter;
                else if(!(file.access&(0x80000000U|1U))) result=access_denied;
                else if(file.entry.directory() && !file.device) result=0xC00000BA;
                else if(length) {
                    const auto size=file_size(file);
                    if(offset>=size) result=end_of_file;
                    else {
                        written=static_cast<std::uint32_t>(std::min<std::uint64_t>(length,size-offset));
                        const std::span destination(static_cast<std::byte*>(memory.access(output,written)),written);
                        if(file.device) disc->read(offset,destination);else disc->read(file.entry,offset,destination);
                        file.position=offset+written;memory.store<std::uint64_t>(found->second.address+0x14,file.position);
                    }
                }
            } else if constexpr(Ordinal==196) result=disc_control(cpu,file,written);
            else {
                const auto output=argument(cpu,5),length=argument(cpu,6),type=argument(cpu,7),pattern=argument(cpu,8);
                if(!file.entry.directory()) result=0xC0000103;
                else if(type!=1 && type!=12) throw NativeBoundary(cpu.eip,std::format("unsupported directory information class {}",type));
                else {
                    if(argument(cpu,9) || !file.listing_loaded) {
                        file.pattern=pattern?counted_string(pattern):"*";
                        if(file.native!=INVALID_HANDLE_VALUE) {
                            file.listing.clear();
                            for(const auto& entry:std::filesystem::directory_iterator(file.host_path)) {
                                if(file.listing.size()>=4096 || entry.is_symlink()) throw NativeBoundary(cpu.eip,"native directory exceeds its bound or contains a link");
                                const auto size=entry.is_directory()?0:entry.file_size();
                                if(size>UINT32_MAX) throw NativeBoundary(cpu.eip,"native directory contains a file larger than Xbox supports");
                                const auto attributes=GetFileAttributesW(entry.path().c_str());
                                if(attributes==INVALID_FILE_ATTRIBUTES) throw NativeBoundary(cpu.eip,"cannot query native directory entry attributes");
                                file.listing.push_back({utf8(entry.path().filename().native()),0,static_cast<std::uint32_t>(size),static_cast<std::uint8_t>(attributes)});
                            }
                            std::ranges::sort(file.listing,[](const auto& a,const auto& b) {return folded(a.name)<folded(b.name);});
                        } else {
                            const auto relative=disc_path(file.path);
                            if(!relative) throw NativeBoundary(cpu.eip,"directory left its mounted disc");
                            file.listing=disc->list(*relative);
                        }
                        file.listing_position=0;file.listing_loaded=true;
                    }
                    bool any=false;
                    while(file.listing_position<file.listing.size()) {
                        const auto& entry=file.listing[file.listing_position];
                        if(!wildcard(file.pattern,entry.name)) {++file.listing_position;continue;}
                        const auto header=type==1?64U:12U;
                        const auto name_length=static_cast<unsigned>(entry.name.size()),required=align_up(header+name_length,8);
                        if(required>length-written) {if(!any) result=0x80000005;break;}
                        const auto destination=output+written;
                        std::memset(memory.access(destination,required),0,required);
                        memory.store<std::uint32_t>(destination+4,static_cast<std::uint32_t>(file.listing_position));
                        if(type==1) {
                            memory.store<std::uint64_t>(destination+40,entry.directory()?0:entry.size);
                            const auto unit=file.native!=INVALID_HANDLE_VALUE?16384ULL:2048ULL;
                            memory.store<std::uint64_t>(destination+48,entry.directory()?0:(static_cast<std::uint64_t>(entry.size)+unit-1)&~(unit-1));
                            memory.store<std::uint32_t>(destination+56,static_cast<std::uint32_t>(entry.attributes)|(file.native==INVALID_HANDLE_VALUE?1U:0U));
                            memory.store<std::uint32_t>(destination+60,name_length);
                        } else memory.store<std::uint32_t>(destination+8,name_length);
                        std::memcpy(memory.access(destination+header,name_length),entry.name.data(),name_length);
                        written=required;any=true;++file.listing_position;
                        // Xbox's ten-argument API always returns a single entry.
                        // SDK callers advance by querying again, including while
                        // removing every file in an existing save directory.
                        break;
                    }
                    if(!any && result==success) result=0x80000006;
                }
            }
            memory.store<std::uint32_t>(found->second.address+0x10,result);
            trace_io(cpu,Ordinal,result,written,file.path,file.options,trace_requested,trace_offset,trace_buffer);
            io_complete(cpu,result,written);finish(cpu,cleanup,result);
        } else if constexpr(Ordinal==211 || Ordinal==218 || Ordinal==226 || Ordinal==198) {
            const auto found=handles.find(argument(cpu,0));
            constexpr unsigned cleanup=Ordinal==198?8:20;
            if(found==handles.end() || found->second.kind!=Object::Kind::file) {io_status(argument(cpu,1),invalid_handle);finish(cpu,cleanup,invalid_handle);return;}
            auto& file=*found->second.file;
            std::uint32_t result=success,written=0;
            if constexpr(Ordinal==211) result=file_information(file,argument(cpu,2),argument(cpu,3),argument(cpu,4),written);
            else if constexpr(Ordinal==226) {
                const auto type=argument(cpu,4);
                if(type==14) {
                    if(argument(cpu,3)<8) result=buffer_too_small;
                    else {
                        const auto offset=memory.load<std::uint64_t>(argument(cpu,2));
                        if(offset>INT64_MAX) result=invalid_parameter;
                        else {file.position=offset;memory.store<std::uint64_t>(found->second.address+0x14,offset);}
                    }
                } else if(file.native==INVALID_HANDLE_VALUE) result=write_protected;
                else if(type==19 || type==20) {
                    if(argument(cpu,3)<8) result=buffer_too_small;
                    else {
                        const auto size=memory.load<std::uint64_t>(argument(cpu,2));
                        if(size>INT64_MAX) result=invalid_parameter;
                        else if(type==19) {
                            FILE_ALLOCATION_INFO information{};information.AllocationSize.QuadPart=static_cast<LONGLONG>(size);
                            if(!SetFileInformationByHandle(file.native,FileAllocationInfo,&information,sizeof(information))) result=file_error(GetLastError());
                        } else {
                            FILE_END_OF_FILE_INFO information{};information.EndOfFile.QuadPart=static_cast<LONGLONG>(size);
                            if(!SetFileInformationByHandle(file.native,FileEndOfFileInfo,&information,sizeof(information))) result=file_error(GetLastError());
                        }
                    }
                } else if(type==13) {
                    if(argument(cpu,3)<1) result=buffer_too_small;
                    else {
                        FILE_DISPOSITION_INFO information{memory.load<std::uint8_t>(argument(cpu,2))!=0};
                        if(!SetFileInformationByHandle(file.native,FileDispositionInfo,&information,sizeof(information))) result=file_error(GetLastError());
                    }
                } else if(type==4) {
                    if(argument(cpu,3)<36) result=buffer_too_small;
                    else {
                        FILE_BASIC_INFO information{};const auto request=argument(cpu,2);
                        information.CreationTime.QuadPart=std::bit_cast<std::int64_t>(memory.load<std::uint64_t>(request));
                        information.LastAccessTime.QuadPart=std::bit_cast<std::int64_t>(memory.load<std::uint64_t>(request+8));
                        information.LastWriteTime.QuadPart=std::bit_cast<std::int64_t>(memory.load<std::uint64_t>(request+16));
                        information.ChangeTime.QuadPart=std::bit_cast<std::int64_t>(memory.load<std::uint64_t>(request+24));
                        information.FileAttributes=memory.load<std::uint32_t>(request+32);
                        if(!SetFileInformationByHandle(file.native,FileBasicInfo,&information,sizeof(information))) result=file_error(GetLastError());
                    }
                }
                else throw NativeBoundary(cpu.eip,std::format("unsupported set-file information class {}",type));
            } else if constexpr(Ordinal==218) {
                const auto output=argument(cpu,2),length=argument(cpu,3),type=argument(cpu,4);
                if(type==3) {
                    if(length<24) result=buffer_too_small;
                    else {
                        if(file.native!=INVALID_HANDLE_VALUE) {
                            ULARGE_INTEGER free{},total{};
                            if(!GetDiskFreeSpaceExW(user_root.c_str(),&free,&total,nullptr)) result=file_error(GetLastError());
                            else {
                                const auto capacity=std::min<std::uint64_t>(0x131F00000,total.QuadPart);
                                memory.store<std::uint64_t>(output,capacity/16384);memory.store<std::uint64_t>(output+8,std::min(capacity,free.QuadPart)/16384);
                                memory.store<std::uint32_t>(output+16,32);memory.store<std::uint32_t>(output+20,512);written=24;
                            }
                        } else {
                            memory.store<std::uint64_t>(output,disc->size()/2048);memory.store<std::uint64_t>(output+8,0);
                            memory.store<std::uint32_t>(output+16,1);memory.store<std::uint32_t>(output+20,2048);written=24;
                        }
                    }
                } else if(type==4) {
                    if(length<8) result=buffer_too_small;
                    else {memory.store<std::uint32_t>(output,file.native!=INVALID_HANDLE_VALUE?7:2);memory.store<std::uint32_t>(output+4,file.native!=INVALID_HANDLE_VALUE?0:0x24);written=8;}
                } else if(type==5) {
                    const std::string_view name=file.native!=INVALID_HANDLE_VALUE?"FATX":"XDVDFS";
                    if(length<12+name.size()) result=buffer_too_small;
                    else {
                        memory.store<std::uint32_t>(output,file.native!=INVALID_HANDLE_VALUE?2:0x00080000);memory.store<std::uint32_t>(output+4,file.native!=INVALID_HANDLE_VALUE?42:255);
                        memory.store<std::uint32_t>(output+8,static_cast<std::uint32_t>(name.size()));
                        std::memcpy(memory.access(output+12,name.size()),name.data(),name.size());written=12+static_cast<unsigned>(name.size());
                    }
                } else throw NativeBoundary(cpu.eip,std::format("unsupported volume information class {}",type));
            } else if constexpr(Ordinal==198) {if(file.native!=INVALID_HANDLE_VALUE && !FlushFileBuffers(file.native)) result=file_error(GetLastError());}
            trace_io(cpu,Ordinal,result,written,file.path,Ordinal==198?0:argument(cpu,4),Ordinal==198?0:argument(cpu,3));
            io_status(argument(cpu,1),result,written);finish(cpu,cleanup,result);
        } else if constexpr(Ordinal==210) {
            const auto name=object_name(argument(cpu,0));const auto relative=name?disc_path(*name):std::nullopt;
            if(name && !relative) {
                const auto path=writable_path(*name);
                if(!path) {finish(cpu,8,name_not_found);return;}
                WIN32_FILE_ATTRIBUTE_DATA data{};
                if(!GetFileAttributesExW(path->c_str(),GetFileExInfoStandard,&data)) {finish(cpu,8,file_error(GetLastError()));return;}
                const auto output=argument(cpu,1);
                const FILETIME times[]={data.ftCreationTime,data.ftLastAccessTime,data.ftLastWriteTime,data.ftLastWriteTime};
                for(unsigned i=0;i<4;++i) memory.store<std::uint64_t>(output+i*8,(static_cast<std::uint64_t>(times[i].dwHighDateTime)<<32)|times[i].dwLowDateTime);
                const auto size=(static_cast<std::uint64_t>(data.nFileSizeHigh)<<32)|data.nFileSizeLow;
                memory.store<std::uint64_t>(output+32,(size+16383)&~16383ULL);memory.store<std::uint64_t>(output+40,size);
                memory.store<std::uint32_t>(output+48,data.dwFileAttributes);finish(cpu,8,success);return;
            }
            const auto entry=disc && relative?disc->lookup(*relative):std::nullopt;
            if(!entry) {finish(cpu,8,disc?name_not_found:no_media);return;}
            OpenFile file;file.entry=*entry;file.path=*name;
            std::uint32_t written=0;finish(cpu,8,file_information(file,argument(cpu,1),52,34,written));
        } else if constexpr(Ordinal==67 || Ordinal==69) {
            const auto name=folded(counted_string(argument(cpu,0)));
            if(name.empty() || !name.starts_with('\\')) {finish(cpu,Ordinal==67?8:4,invalid_parameter);return;}
            if constexpr(Ordinal==69) finish(cpu,4,links.erase(name)?success:name_not_found);
            else {
                const auto target=counted_string(argument(cpu,1));
                if(target.empty() || !target.starts_with('\\')) {finish(cpu,8,invalid_parameter);return;}
                const auto [entry,inserted]=links.emplace(name,target);finish(cpu,8,inserted?success:0xC0000035U);
            }
        } else if constexpr(Ordinal==203) {
            const auto name=object_name(argument(cpu,1),false);
            if(!name) {finish(cpu,8,invalid_parameter);return;}
            const auto link=links.find(folded(*name));
            if(link==links.end()) {finish(cpu,8,name_not_found);return;}
            const auto base=allocate_object();
            if(!base) {finish(cpu,8,no_memory);return;}
            const auto address=base|0x80000000U,handle=next_handle;next_handle+=4;
            auto data=std::make_shared<OpenFile>();data->link=link->second;
            const Object object{Object::Kind::link,address,nullptr,data};handles.emplace(handle,object);objects.emplace(address,object);
            memory.store<std::uint32_t>(argument(cpu,0),handle);finish(cpu,8,success);
        } else if constexpr(Ordinal==215) {
            const auto found=handles.find(argument(cpu,0));
            if(found==handles.end() || found->second.kind!=Object::Kind::link) {finish(cpu,12,invalid_handle);return;}
            const auto& target=found->second.file->link;
            const auto output=argument(cpu,1),returned=argument(cpu,2);
            const auto length=static_cast<std::uint16_t>(target.size());
            if(returned) memory.store<std::uint32_t>(returned,length);
            if(memory.load<std::uint16_t>(output+2)<length) {finish(cpu,12,buffer_too_small);return;}
            std::memcpy(memory.access(memory.load<std::uint32_t>(output+4),length),target.data(),length);
            memory.store<std::uint16_t>(output,length);finish(cpu,12,success);
        } else if constexpr(Ordinal==252 || Ordinal==253) {
            // The original menu queries its Ethernet cable before starting any
            // network stack. Read the native interfaces; never initialize MCPX.
            if constexpr(Ordinal==253) if(argument(cpu,1)) throw NativeBoundary(cpu.eip,"unsupported Ethernet initialization parameter");
            if(!ethernet_initialized || argument(cpu,0)) {
                MIB_IF_TABLE2* table=nullptr;const auto result=GetIfTable2(&table);
                if(result!=NO_ERROR) {
                    if constexpr(Ordinal==253) {finish(cpu,8,static_cast<std::uint32_t>(HRESULT_FROM_WIN32(result)));return;}
                    else throw NativeBoundary(cpu.eip,"native Ethernet status query failed");
                }
                std::unique_ptr<MIB_IF_TABLE2,decltype(&FreeMibTable)> interfaces(table,FreeMibTable);
                ethernet_state=0;
                for(unsigned i=0;i<table->NumEntries;++i) {
                    const auto& link=table->Table[i];
                    if(link.Type!=IF_TYPE_ETHERNET_CSMACD || !link.InterfaceAndOperStatusFlags.HardwareInterface ||
                        link.OperStatus!=IfOperStatusUp || link.MediaConnectState!=MediaConnectStateConnected) continue;
                    const auto speed=std::min(link.TransmitLinkSpeed,link.ReceiveLinkSpeed);
                    ethernet_state=1|(speed>=100000000?2U:speed>=10000000?4U:0U);break;
                }
                // Windows does not expose duplex here; do not invent either bit.
                ethernet_initialized=true;
            }
            finish(cpu,Ordinal==253?8:4,Ordinal==253?success:ethernet_state);
        } else if constexpr(Ordinal==255) {
            const auto output=argument(cpu,0),extension=argument(cpu,1),tls=argument(cpu,3),identifier=argument(cpu,4);
            if(!output || extension || argument(cpu,8)) throw NativeBoundary(cpu.eip,"thread extension or debugger-thread contract is unsupported");
            auto& thread=create(argument(cpu,5),argument(cpu,6),argument(cpu,9),argument(cpu,2),tls,argument(cpu,7)!=0);
            const auto handle=next_handle; next_handle+=4;
            handles.emplace(handle,Object{Object::Kind::thread,thread.object,&thread});
            memory.store<std::uint32_t>(output,handle);
            if(identifier) memory.store<std::uint32_t>(identifier,thread.id);
            finish(cpu,40,success);
        } else if constexpr(Ordinal==258) throw ThreadExit{argument(cpu,0)};
        else if constexpr(Ordinal==187) {
            const auto found=handles.find(argument(cpu,0));
            const auto result=found==handles.end()?invalid_handle:success;
            if(found!=handles.end()) {
                const auto address=found->second.address;
                if(found->second.kind==Object::Kind::file) trace_io(cpu,Ordinal,result,object_references[address],found->second.file->path);
                handles.erase(found);release_object(address);
            }
            finish(cpu,4,result);
        } else if constexpr(Ordinal==184) {
            const auto output=argument(cpu,0),size_pointer=argument(cpu,2),flags=argument(cpu,3),protect=argument(cpu,4);
            const auto base=memory.load<std::uint32_t>(output),size=memory.load<std::uint32_t>(size_pointer);
            if(!size || argument(cpu,1) || (flags&~(MEM_RESERVE|MEM_COMMIT|MEM_TOP_DOWN|memory_nozero)) || !(flags&(MEM_RESERVE|MEM_COMMIT))) {
                finish(cpu,20,invalid_parameter); return;
            }
            if(base && !(flags&MEM_RESERVE)) {
                const auto found=allocation(base);
                if(found==allocations.end() || base-found->first+static_cast<std::uint64_t>(size)>found->second.size) {
                    finish(cpu,20,conflicting_addresses); return;
                }
                const auto first=(base-found->first)/page,last=align_up(base-found->first+size,page)/page;
                if(!(flags&memory_nozero)) for(auto index=first;index<last;++index)
                    if(!found->second.pages[index]) std::memset(memory.access(found->first+index*page,page),0,page);
                std::fill(found->second.pages.begin()+first,found->second.pages.begin()+last,protect);
                memory.store<std::uint32_t>(output,base&~(page-1));
                memory.store<std::uint32_t>(size_pointer,(last-first)*page); finish(cpu,20,success); return;
            }
            const auto allocated=allocate(size,page,(flags&MEM_COMMIT)!=0,protect,base,0,UINT32_MAX,
                !(flags&memory_nozero),(flags&MEM_TOP_DOWN)!=0);
            if(allocated) allocations.at(allocated).kind=AllocationKind::virtual_memory;
            if(allocated) { memory.store<std::uint32_t>(output,allocated); memory.store<std::uint32_t>(size_pointer,align_up(size,page)); }
            finish(cpu,20,allocated?success:(base?conflicting_addresses:no_memory));
        } else if constexpr(Ordinal==199) {
            const auto pointer=argument(cpu,0),size_pointer=argument(cpu,1),flags=argument(cpu,2);
            const auto base=memory.load<std::uint32_t>(pointer),size=memory.load<std::uint32_t>(size_pointer);
            const auto found=allocation(base);
            if(found==allocations.end() || (flags!=MEM_RELEASE && flags!=MEM_DECOMMIT) ||
                (flags==MEM_RELEASE && (size || base!=found->first))) { finish(cpu,12,invalid_parameter); return; }
            if(flags==MEM_RELEASE) { release_allocation(found->first); memory.store<std::uint32_t>(pointer,0); memory.store<std::uint32_t>(size_pointer,0); }
            else {
                const auto first=(base-found->first)/page;
                const auto last=size?align_up(base-found->first+size,page)/page:found->second.size/page;
                if(last>found->second.pages.size()) { finish(cpu,12,invalid_parameter); return; }
                if(gpu) gpu->invalidate_memory(found->first+first*page,(last-first)*page);
                std::fill(found->second.pages.begin()+first,found->second.pages.begin()+last,0);
                std::memset(memory.access(found->first+first*page,(last-first)*page),0,(last-first)*page);
                memory.store<std::uint32_t>(pointer,found->first+first*page); memory.store<std::uint32_t>(size_pointer,(last-first)*page);
            }
            finish(cpu,12,success);
        } else if constexpr(Ordinal==178) {
            const auto address=argument(cpu,0)&0x0FFFFFFFU,size=argument(cpu,1);
            if(address>ram.size() || size>ram.size()-address) throw NativeBoundary(cpu.eip,"persistent memory range leaves native RAM");
            const auto end=align_up(address+size,page);
            for(auto cursor=address&~(page-1);size && cursor<end;cursor+=page) {
                if(argument(cpu,2)) persistent_pages.insert(cursor); else persistent_pages.erase(cursor);
            }
            finish(cpu,12);
        } else if constexpr(Ordinal==181) {
            const auto output=argument(cpu,0);
            if(!output || memory.load<std::uint32_t>(output)!=36) {finish(cpu,4,invalid_parameter);return;}
            std::uint32_t committed=0,reserved=0,vm_committed=0,pool=image.base/page,stacks=0,cache=0;
            for(const auto& [address,block]:allocations) {
                const auto pages=static_cast<std::uint32_t>(std::ranges::count_if(block.pages,[](auto value){return value!=0;}));
                committed+=pages;
                if(block.kind==AllocationKind::virtual_memory) {reserved+=block.size;vm_committed+=pages*page;}
                else if(block.kind==AllocationKind::stack) stacks+=pages;
                else if(block.kind==AllocationKind::cache) cache+=pages;
                else pool+=pages;
            }
            const auto total=static_cast<std::uint32_t>(ram.size()/page),images=align_up(image.image_size,page)/page;
            const auto used=heap_begin/page+committed;
            const std::uint32_t values[]={36,total,used<total?total-used:0U,vm_committed,reserved,cache,pool,stacks,images};
            std::memcpy(memory.access(output,sizeof(values)),values,sizeof(values));finish(cpu,4,success);
        } else if constexpr(Ordinal==37) {
            finish(cpu,4,cache_size(argument(cpu,0)));
        } else if constexpr(Ordinal==167) {
            const auto result=allocate(argument(cpu,0),page,true,argument(cpu,1));
            finish(cpu,8,result?result|0x80000000U:0U);
        } else if constexpr(Ordinal==172) {
            const auto address=argument(cpu,0)&0x0FFFFFFFU;
            const auto found=allocation(address);
            if(found==allocations.end()) {finish(cpu,8,0);return;}
            const auto base=found->first,offset=address-base;
            const auto requested=argument(cpu,1),size=requested?align_up(requested,page):found->second.size-offset;
            if((address&(page-1)) || size>found->second.size-offset) throw NativeBoundary(cpu.eip,"system-memory free crosses an allocation");
            if(gpu) gpu->invalidate_memory(address,size);
            auto block=std::move(found->second);allocations.erase(found);
            const auto first=offset/page,last=(offset+size)/page;
            if(offset) allocations.emplace(base,Allocation{offset,block.protect,{block.pages.begin(),block.pages.begin()+first},block.kind});
            if(offset+size<block.size) allocations.emplace(address+size,Allocation{block.size-offset-size,block.protect,
                {block.pages.begin()+last,block.pages.end()},block.kind});
            for(auto cursor=address;cursor<address+size;cursor+=page) persistent_pages.erase(cursor);
            finish(cpu,8,size/page);
        } else if constexpr(Ordinal==165 || Ordinal==166 || Ordinal==15) {
            const auto size=argument(cpu,0);
            const auto alignment=Ordinal==166?argument(cpu,3):page;
            const auto lowest=Ordinal==166?argument(cpu,1):0U,highest=Ordinal==166?argument(cpu,2):UINT32_MAX;
            const auto protect=Ordinal==166?argument(cpu,4):PAGE_READWRITE;
            const auto allocated=allocate(size,alignment?alignment:page,true,protect,0,lowest,highest);
            finish(cpu,Ordinal==166?20:Ordinal==15?8:4,allocated?allocated|0x80000000U:0U);
        } else if constexpr(Ordinal==171 || Ordinal==17) {
            const auto found=allocation(argument(cpu,0));
            if(found==allocations.end()) throw NativeBoundary(cpu.eip,"free of an unknown native guest allocation");
            release_allocation(found->first); finish(cpu,4);
        } else if constexpr(Ordinal==180 || Ordinal==23 || Ordinal==179) {
            const auto found=allocation(argument(cpu,0));
            finish(cpu,4,found==allocations.end()?0:Ordinal==179?found->second.pages[((argument(cpu,0)&0x0FFFFFFF)-found->first)/page]:found->second.size);
        } else if constexpr(Ordinal==182) {
            const auto found=allocation(argument(cpu,0));
            if(found==allocations.end() || argument(cpu,1)>found->second.size) throw NativeBoundary(cpu.eip,"protection change outside a guest allocation");
            const auto first=((argument(cpu,0)&0x0FFFFFFF)-found->first)/page;
            const auto last=align_up((argument(cpu,0)&0x0FFFFFFF)-found->first+argument(cpu,1),page)/page;
            if(last>found->second.pages.size()) throw NativeBoundary(cpu.eip,"protection change crosses a guest allocation");
            std::fill(found->second.pages.begin()+first,found->second.pages.begin()+last,argument(cpu,2)); finish(cpu,12);
        } else if constexpr(Ordinal==173) finish(cpu,4,argument(cpu,0)&0x0FFFFFFFU);
        else if constexpr(Ordinal==217) {
            const auto base=argument(cpu,0),output=argument(cpu,1);
            const auto found=allocation(base);
            if(found==allocations.end()) { finish(cpu,8,invalid_parameter); return; }
            const auto index=(base-found->first)/page,protect=found->second.pages[index];
            auto first=index,last=index+1;
            while(first && found->second.pages[first-1]==protect) --first;
            while(last<found->second.pages.size() && found->second.pages[last]==protect) ++last;
            const std::uint32_t information[]={found->first+first*page,found->first,found->second.protect,(last-first)*page,
                static_cast<std::uint32_t>(protect?MEM_COMMIT:MEM_RESERVE),protect,MEM_PRIVATE};
            std::memcpy(memory.access(output,sizeof(information)),information,sizeof(information)); finish(cpu,8,success);
        } else if constexpr(Ordinal==291) {
            const auto address=argument(cpu,0);
            memory.fill_elements<std::uint32_t>(address,0,7,false);
            memory.store<std::uint32_t>(address,0x00040001);
            memory.store<std::uint32_t>(address+8,address+8); memory.store<std::uint32_t>(address+12,address+8);
            memory.store<std::uint32_t>(address+16,UINT32_MAX); finish(cpu,4);
        } else if constexpr(Ordinal==277 || Ordinal==306) {
            const auto address=argument(cpu,0),owner=memory.load<std::uint32_t>(address+24);
            if constexpr(Ordinal==306) { if(owner && owner!=current->object) { finish(cpu,4,0); return; } }
            memory.store<std::uint32_t>(address+16,memory.load<std::uint32_t>(address+16)+1);
            while(const auto waiting=memory.load<std::uint32_t>(address+24)) {
                if(waiting==current->object) break;
                yield(ThreadState::waiting,address);
            }
            memory.store<std::uint32_t>(address+24,current->object);
            memory.store<std::uint32_t>(address+20,memory.load<std::uint32_t>(address+20)+1);
            finish(cpu,4,Ordinal==306?std::optional<std::uint64_t>{1}:std::nullopt);
        } else if constexpr(Ordinal==294) {
            const auto address=argument(cpu,0),recursion=memory.load<std::uint32_t>(address+20);
            if(!recursion || memory.load<std::uint32_t>(address+24)!=current->object)
                throw NativeBoundary(cpu.eip,"critical section released without ownership");
            memory.store<std::uint32_t>(address+20,recursion-1);
            memory.store<std::uint32_t>(address+16,memory.load<std::uint32_t>(address+16)-1);
            if(recursion==1) memory.store<std::uint32_t>(address+24,0);
            finish(cpu,4);
        } else if constexpr(Ordinal==301) {
            finish(cpu,4,dos_error(argument(cpu,0)));
        } else if constexpr(Ordinal==103) finish(cpu,0,memory.load<std::uint8_t>(cpu.fs_base+0x24));
        else if constexpr(Ordinal==104) finish(cpu,0,current->object);
        else if constexpr(Ordinal==129 || Ordinal==160 || Ordinal==161) {
            const auto prior=memory.load<std::uint8_t>(cpu.fs_base+0x24);
            memory.store<std::uint8_t>(cpu.fs_base+0x24,static_cast<std::uint8_t>(Ordinal==129?2:cpu.registers[ecx]));
            finish(cpu,0,Ordinal==161?std::nullopt:std::optional<std::uint64_t>{prior});
        } else if constexpr(Ordinal==126 || Ordinal==127) {
            LARGE_INTEGER value=performance_frequency;
            if constexpr(Ordinal==126) QueryPerformanceCounter(&value);
            finish(cpu,0,static_cast<std::uint64_t>(value.QuadPart),true);
        } else if constexpr(Ordinal==128) {
            FILETIME value; GetSystemTimeAsFileTime(&value);
            memory.store<std::uint64_t>(argument(cpu,0),(static_cast<std::uint64_t>(value.dwHighDateTime)<<32)|value.dwLowDateTime);
            finish(cpu,4);
        } else if constexpr(Ordinal==143) {
            const auto target=argument(cpu,0);
            const auto increment=static_cast<std::int32_t>(argument(cpu,1));
            const auto previous=memory.load<std::uint8_t>(target+0x70);
            memory.store<std::uint8_t>(target+0x70,static_cast<std::uint8_t>(std::clamp(8+increment,1,15)));
            finish(cpu,8,static_cast<std::uint32_t>(previous)-8);
        } else if constexpr(Ordinal==124) {
            finish(cpu,4,static_cast<std::uint32_t>(memory.load<std::uint8_t>(argument(cpu,0)+0x70))-8);
        } else if constexpr(Ordinal==144) {
            const auto target=argument(cpu,0);
            const auto found=std::ranges::find_if(threads,[&](const auto& thread){return thread->object==target;});
            if(found==threads.end()) throw NativeBoundary(cpu.eip,"disable-boost target is not a native thread");
            const auto previous=(*found)->disable_boost;(*found)->disable_boost=argument(cpu,1)!=0;
            finish(cpu,8,previous?1U:0U);
        } else if constexpr(Ordinal==224 || Ordinal==231) {
            const auto handle=argument(cpu,0),output=argument(cpu,1);
            const auto found=handles.find(handle);
            Thread* target=handle==0xFFFFFFFEU?current:found!=handles.end() && found->second.kind==Object::Kind::thread?found->second.thread:nullptr;
            if(!target) {finish(cpu,8,invalid_handle);return;}
            const auto previous=target->suspend_count;
            if constexpr(Ordinal==231) {
                if(previous==127) {finish(cpu,8,0xC000004A);return;}
                ++target->suspend_count;
            } else if(previous) --target->suspend_count;
            target->suspended=target->suspend_count!=0;
            if(output) memory.store<std::uint32_t>(output,previous);
            finish(cpu,8,success);
            if(target==current && target->suspended) yield();
        } else if constexpr(Ordinal==151) {
            const auto end=std::chrono::steady_clock::now()+std::chrono::microseconds(argument(cpu,0));
            while(std::chrono::steady_clock::now()<end) YieldProcessor();
            finish(cpu,4);
        } else if constexpr(Ordinal==99) {
            const auto interval=std::bit_cast<std::int64_t>(memory.load<std::uint64_t>(argument(cpu,2)));
            if(argument(cpu,0)>1) {finish(cpu,12,invalid_parameter);return;}
            const bool alertable=argument(cpu,1) && argument(cpu,0)==1;
            AlertableWait mode(*current,alertable);
            const auto due=due_time(interval);
            for(;;) {
                if(alertable) {dispatch_io();if(deliver_apcs()){finish(cpu,12,0xC0);return;}}
                current->wait_kind=WaitKind::delay;yield(ThreadState::waiting,0,due);
                if(alertable) {dispatch_io();if(deliver_apcs()){finish(cpu,12,0xC0);return;}}
                if(std::chrono::steady_clock::now()>=due) break;
            }
            finish(cpu,12,success);
        } else if constexpr(Ordinal==238) { yield(); finish(cpu,0,success); }
        else if constexpr(Ordinal==107) {
            const auto address=argument(cpu,0);
            memory.fill_elements<std::uint32_t>(address,0,7,false);
            memory.store<std::uint16_t>(address,19);
            memory.store<std::uint32_t>(address+4,address+4); memory.store<std::uint32_t>(address+8,address+4);
            memory.store<std::uint32_t>(address+12,argument(cpu,1)); memory.store<std::uint32_t>(address+16,argument(cpu,2));
            finish(cpu,12);
        } else if constexpr(Ordinal==108 || Ordinal==112 || Ordinal==113) {
            const auto address=argument(cpu,0),value=argument(cpu,1);
            if constexpr(Ordinal==108) {
                if(value>1) throw NativeBoundary(cpu.eip,"invalid event type");
                dispatcher(address,value,16,argument(cpu,2)!=0); finish(cpu,12);
            } else if constexpr(Ordinal==112) {
                const auto limit=argument(cpu,2);
                if(static_cast<std::int32_t>(value)<0 || value>limit || static_cast<std::int32_t>(limit)<=0)
                    throw NativeBoundary(cpu.eip,"invalid semaphore initialization");
                dispatcher(address,5,20,value); memory.store<std::uint32_t>(address+16,limit); finish(cpu,12);
            } else {
                if(value>1) throw NativeBoundary(cpu.eip,"invalid timer type");
                timers.erase(address); dispatcher(address,8+value,40,0);
                memory.store<std::uint32_t>(address+24,address+24); memory.store<std::uint32_t>(address+28,address+24); finish(cpu,8);
            }
        } else if constexpr(Ordinal==119) finish(cpu,12,queue_dpc(argument(cpu,0),argument(cpu,1),argument(cpu,2)));
        else if constexpr(Ordinal==137) {
            const auto address=argument(cpu,0);
            const bool queued=memory.load<std::uint8_t>(address+2)!=0;
            std::erase(dpcs,address); memory.store<std::uint8_t>(address+2,0); finish(cpu,4,queued);
        } else if constexpr(Ordinal==149 || Ordinal==150) {
            const auto address=argument(cpu,0),period=Ordinal==150?argument(cpu,3):0U;
            const auto dpc=argument(cpu,Ordinal==150?4:3);
            const auto raw=static_cast<std::uint64_t>(argument(cpu,1))|(static_cast<std::uint64_t>(argument(cpu,2))<<32);
            if(static_cast<std::int32_t>(period)<0) throw NativeBoundary(cpu.eip,"negative timer period");
            const auto due=due_time(std::bit_cast<std::int64_t>(raw)); const bool replaced=timers.contains(address);
            timers.insert_or_assign(address,Timer{due,period,dpc});
            memory.store<std::uint32_t>(address+4,0); memory.store<std::uint8_t>(address+3,1);
            memory.store<std::uint8_t>(address+1,static_cast<std::int64_t>(raw)>0);
            const auto relative=std::chrono::duration_cast<std::chrono::nanoseconds>(due-started).count()/100;
            memory.store<std::uint64_t>(address+16,static_cast<std::uint64_t>(relative));
            memory.store<std::uint32_t>(address+32,dpc); memory.store<std::uint32_t>(address+36,period);
            finish(cpu,Ordinal==150?20:16,replaced);
        } else if constexpr(Ordinal==97) {
            const auto address=argument(cpu,0); const bool cancelled=timers.erase(address)!=0;
            memory.store<std::uint8_t>(address+3,0); finish(cpu,4,cancelled);
        } else if constexpr(Ordinal==145) {
            const auto address=argument(cpu,0),prior=memory.load<std::uint32_t>(address+4);
            memory.store<std::uint32_t>(address+4,1); finish(cpu,12,prior);
        } else if constexpr(Ordinal==189 || Ordinal==193) {
            const auto attributes=argument(cpu,1),initial=argument(cpu,Ordinal==189?3:2);
            if(attributes && memory.load<std::uint32_t>(attributes+4)) throw NativeBoundary(cpu.eip,"named dispatcher objects require namespace support");
            if constexpr(Ordinal==189) {if(argument(cpu,2)>1) {finish(cpu,16,invalid_parameter);return;}}
            else if(static_cast<std::int32_t>(initial)<0 || static_cast<std::int32_t>(argument(cpu,3))<=0 || initial>argument(cpu,3)) {
                finish(cpu,16,invalid_parameter); return;
            }
            const auto base=allocate_object();
            if(!base) {finish(cpu,16,no_memory);return;}
            const auto address=base|0x80000000U,handle=next_handle; next_handle+=4;
            if constexpr(Ordinal==189) dispatcher(address,argument(cpu,2),16,initial!=0);
            else {dispatcher(address,5,20,initial);memory.store<std::uint32_t>(address+16,argument(cpu,3));}
            const Object object{Ordinal==189?Object::Kind::event:Object::Kind::semaphore,address};
            objects.emplace(address,object); handles.emplace(handle,object);
            memory.store<std::uint32_t>(argument(cpu,0),handle); finish(cpu,16,success);
        } else if constexpr(Ordinal==225 || Ordinal==186 || Ordinal==222) {
            const auto found=handles.find(argument(cpu,0));
            constexpr auto kind=Ordinal==222?Object::Kind::semaphore:Object::Kind::event;
            const auto cleanup=Ordinal==222?12:Ordinal==225?8:4;
            if(found==handles.end() || found->second.kind!=kind) {finish(cpu,cleanup,invalid_handle);return;}
            const auto address=found->second.address,prior=memory.load<std::uint32_t>(address+4);
            if constexpr(Ordinal==222) {
                const auto increment=argument(cpu,1),limit=memory.load<std::uint32_t>(address+16);
                if(static_cast<std::int32_t>(increment)<=0 || static_cast<std::uint64_t>(prior)+increment>limit) {finish(cpu,12,0xC0000047);return;}
                memory.store<std::uint32_t>(address+4,prior+increment);
            } else memory.store<std::uint32_t>(address+4,Ordinal==225?1:0);
            if constexpr(Ordinal!=186) {const auto previous=argument(cpu,Ordinal==222?2:1);if(previous) memory.store<std::uint32_t>(previous,prior);}
            finish(cpu,cleanup,success);
        } else if constexpr(Ordinal==232) {
            const auto routine=argument(cpu,0),status=argument(cpu,1);
            const auto result=memory.load<std::uint32_t>(status);
            const std::array arguments{(result>>30)==3?dos_error(result):0U,memory.load<std::uint32_t>(status+4),status};
            user_callback(cpu,routine,arguments);finish(cpu,12);
        } else if constexpr(Ordinal==159 || Ordinal==158 || Ordinal==233 || Ordinal==234) {
            std::array<std::uint32_t,64> addresses{}; unsigned count=1; bool all=false,alertable;
            std::uint32_t interval;
            if constexpr(Ordinal==158) {
                count=argument(cpu,0); if(!count || count>64 || argument(cpu,2)>1) {finish(cpu,32,invalid_parameter);return;}
                for(unsigned i=0;i<count;++i) addresses[i]=memory.load<std::uint32_t>(argument(cpu,1)+i*4);
                all=argument(cpu,2)==0; alertable=argument(cpu,5)!=0 && argument(cpu,4)==1; interval=argument(cpu,6);
            } else if constexpr(Ordinal==159) {addresses[0]=argument(cpu,0);alertable=argument(cpu,3)!=0 && argument(cpu,2)==1;interval=argument(cpu,4);}
            else {
                const auto found=handles.find(argument(cpu,0));
                if(found==handles.end()) {finish(cpu,Ordinal==233?12:16,invalid_handle);return;}
                if(found->second.kind==Object::Kind::link) {finish(cpu,Ordinal==233?12:16,0xC0000024);return;}
                addresses[0]=found->second.address+(found->second.kind==Object::Kind::file?0x38:0);
                alertable=argument(cpu,Ordinal==233?1:2)!=0; interval=argument(cpu,Ordinal==233?2:3);
                if constexpr(Ordinal==234) alertable &=argument(cpu,1)==1;
            }
            const auto result=wait(std::span(addresses).first(count),all,interval,alertable);
            finish(cpu,Ordinal==158?32:Ordinal==159?20:Ordinal==233?12:16,result);
        } else if constexpr(Ordinal==246) {
            const auto found=handles.find(argument(cpu,0));
            if(found==handles.end()) {finish(cpu,12,invalid_handle);return;}
            const auto type=argument(cpu,1);
            const auto expected=found->second.kind==Object::Kind::thread?thread_type:found->second.kind==Object::Kind::event?event_type:
                found->second.kind==Object::Kind::semaphore?semaphore_type:found->second.kind==Object::Kind::file?file_type:0;
            if(type && type!=expected) {finish(cpu,12,0xC0000024);return;}
            ++object_references[found->second.address]; memory.store<std::uint32_t>(argument(cpu,2),found->second.address); finish(cpu,12,success);
        } else if constexpr(Ordinal==250) {
            const auto address=cpu.registers[ecx];
            const auto found=object_references.find(address);
            if(found==object_references.end() || !found->second) throw NativeBoundary(cpu.eip,"object dereferenced without a native reference");
            --found->second;release_object(address);finish(cpu,0);
        } else if constexpr(Ordinal==2) {
            const auto option=argument(cpu,1),parameter=argument(cpu,2);
            if(option==6) // AV_PACK_STANDARD | AV_STANDARD_NTSC_M | AV_FLAGS_60Hz.
                memory.store<std::uint32_t>(argument(cpu,3),0x00400101|(host.widescreen?0x00010000U:0U));
            else if(option==11 && parameter<=5) flicker_filter=parameter;
            else if(option==14) luma_filter=parameter!=0;
            else throw NativeBoundary(cpu.eip,"native TV encoder option is unbound");
            // Preserve encoder controls. The D3D11 image is taken before the
            // original console's analog television filtering stage.
            finish(cpu,16);
        } else if constexpr(Ordinal==24) {
            const auto index=argument(cpu,0);
            // The compatibility profile owns these settings; they are not a dump
            // of the original machine's EEPROM or cryptographic keys.
            std::optional<std::uint32_t> value;
            if(index==7) value=1; // English.
            else if(index==8) value=host.widescreen?0x00010000U:0U;
            else if(index==9 || index==10 || index==17) value=0; // Stereo, no parental restriction.
            else if(index==0x103) value=0x00400100; // NTSC-M factory AV region.
            else if(index==0x104) value=1; // North American game region.
            if(!value) {finish(cpu,20,0xC0000034);return;}
            const auto length=argument(cpu,4); if(length) memory.store<std::uint32_t>(length,4);
            if(argument(cpu,3)<4) {finish(cpu,20,0xC0000023);return;}
            memory.store<std::uint32_t>(argument(cpu,1),REG_DWORD); memory.store<std::uint32_t>(argument(cpu,2),*value); finish(cpu,20,success);
        } else if constexpr(Ordinal==335 || Ordinal==336 || Ordinal==337) {
            const auto address=argument(cpu,0);Sha1 sha;
            auto storage=memory.access(address,sizeof(sha));std::memcpy(&sha,storage,sizeof(sha));
            if constexpr(Ordinal==335) sha.reset();
            else if constexpr(Ordinal==336) {
                const auto length=argument(cpu,2);
                if(length) sha.update(Bytes(static_cast<const std::byte*>(memory.access(argument(cpu,1),length)),length));
            } else {
                auto output=memory.access(argument(cpu,1),20);const auto digest=sha.finish();
                std::memcpy(storage,&sha,sizeof(sha));std::memcpy(output,digest.data(),digest.size());
                finish(cpu,8);return;
            }
            std::memcpy(storage,&sha,sizeof(sha));finish(cpu,Ordinal==335?4:12);
        } else if constexpr(Ordinal==340) {
            const auto span=[&](unsigned pointer,unsigned size,std::uint32_t limit=UINT32_MAX) {
                const auto length=std::min(argument(cpu,size),limit);
                return length?Bytes(static_cast<const std::byte*>(memory.access(argument(cpu,pointer),length)),length):Bytes{};
            };
            auto output=memory.access(argument(cpu,6),20);
            const auto digest=xbox_hmac(span(0,1,64),span(2,3),span(4,5));
            std::memcpy(output,digest.data(),digest.size());finish(cpu,28);
        } else if constexpr(Ordinal==260 || Ordinal==308) {
            finish(cpu,12,convert_string(cpu,Ordinal==260));
        } else if constexpr(Ordinal==289) {
            const auto output=argument(cpu,0),source=argument(cpu,1); unsigned length=0;
            if(source) while(length<65534 && memory.load<std::uint8_t>(source+length)) ++length;
            memory.store<std::uint16_t>(output,static_cast<std::uint16_t>(length));
            memory.store<std::uint16_t>(output+2,static_cast<std::uint16_t>(source?length+1:0));
            memory.store<std::uint32_t>(output+4,source);finish(cpu,8);
        } else if constexpr(Ordinal==279) {
            const auto first=argument(cpu,0),second=argument(cpu,1),insensitive=argument(cpu,2);
            const auto length=memory.load<std::uint16_t>(first); bool equal=length==memory.load<std::uint16_t>(second);
            const auto a=memory.load<std::uint32_t>(first+4),b=memory.load<std::uint32_t>(second+4);
            const auto upper=[](std::uint8_t ch) {return ch>='a' && ch<='z'?ch-32:ch;};
            for(unsigned i=0;equal && i<length;++i) {
                const auto x=memory.load<std::uint8_t>(a+i),y=memory.load<std::uint8_t>(b+i);
                equal=insensitive?upper(x)==upper(y):x==y;
            }
            finish(cpu,12,equal);
        } else if constexpr(Ordinal==269) {
            const auto address=argument(cpu,0),length=argument(cpu,1),pattern=argument(cpu,2);
            unsigned matched=0;
            while(length-matched>=4 && memory.load<std::uint32_t>(address+matched)==pattern) matched+=4;
            finish(cpu,12,matched);
        } else if constexpr(Ordinal==304 || Ordinal==305) {
            if constexpr(Ordinal==305) {
                const auto time=memory.load<std::uint64_t>(argument(cpu,0));
                const FILETIME value{static_cast<DWORD>(time),static_cast<DWORD>(time>>32)}; SYSTEMTIME system{};
                if(!FileTimeToSystemTime(&value,&system)) throw NativeBoundary(cpu.eip,"invalid guest system time");
                const std::uint16_t fields[]={system.wYear,system.wMonth,system.wDay,system.wHour,system.wMinute,system.wSecond,system.wMilliseconds,system.wDayOfWeek};
                std::memcpy(memory.access(argument(cpu,1),sizeof(fields)),fields,sizeof(fields)); finish(cpu,8);
            } else {
                const auto address=argument(cpu,0);
                const SYSTEMTIME system{memory.load<std::uint16_t>(address),memory.load<std::uint16_t>(address+2),
                    memory.load<std::uint16_t>(address+14),memory.load<std::uint16_t>(address+4),memory.load<std::uint16_t>(address+6),
                    memory.load<std::uint16_t>(address+8),memory.load<std::uint16_t>(address+10),memory.load<std::uint16_t>(address+12)};
                FILETIME time{}; const bool valid_time=SystemTimeToFileTime(&system,&time)!=0;
                if(valid_time) memory.store<std::uint64_t>(argument(cpu,1),(static_cast<std::uint64_t>(time.dwHighDateTime)<<32)|time.dwLowDateTime);
                finish(cpu,8,valid_time);
            }
        }
        else if constexpr(Ordinal==47) {
            const auto registration=argument(cpu,0);
            memory.access(registration,16);
            std::erase(shutdown_registrations,registration);
            if(argument(cpu,1)) shutdown_registrations.push_back(registration);
            std::ranges::sort(shutdown_registrations,[&](auto a,auto b) {
                return static_cast<std::int32_t>(memory.load<std::uint32_t>(a+4))>static_cast<std::int32_t>(memory.load<std::uint32_t>(b+4));
            });
            auto previous=shutdown_head;
            for(const auto item : shutdown_registrations) {
                const auto link=item+8;
                memory.store<std::uint32_t>(previous,link); memory.store<std::uint32_t>(link+4,previous); previous=link;
            }
            memory.store<std::uint32_t>(previous,shutdown_head); memory.store<std::uint32_t>(shutdown_head+4,previous);
            finish(cpu,8);
        }
        else if constexpr(Ordinal==49) throw NativeBoundary(cpu.eip,std::format("guest requested firmware action {}",argument(cpu,0)));
    }
    BootResult run(std::uint32_t budget,std::uint32_t break_address,std::uint32_t break_hit) {
        if(scheduler) throw std::runtime_error("A native Xbox instance can boot only once");
        if(IsThreadAFiber()) scheduler=GetCurrentFiber();
        else { scheduler=ConvertThreadToFiberEx(nullptr,FIBER_FLAG_FLOAT_SWITCH); converted=scheduler!=nullptr; }
        if(!scheduler) throw std::runtime_error("Cannot enter native Xbox scheduler");
        live=budget==0;
        diagnostic.remaining=live?UINT64_MAX:budget;
        diagnostic.watch_address=host.main_address;
        diagnostic.break_address=break_address;diagnostic.break_hit=break_hit;
        create(image.entry,0,0,image.stack_commit,0,false,true);
        const auto deadline=live?std::chrono::steady_clock::time_point::max():std::chrono::steady_clock::now()+std::chrono::seconds(30);
        while(boundary.empty()) {
            PerfScope scheduler_scope(host.performance,PerfPhase::scheduler);
            Thread* selected=nullptr;
            unsigned selected_priority=0;
            std::size_t selected_index=0;
            auto earliest=std::chrono::steady_clock::time_point::max();
            bool pending=false;
            const auto now=std::chrono::steady_clock::now();
            if(now>=deadline) { boundary="Native boot wall-clock budget exhausted"; break; }
            try {
                if(host.poll && !host.poll(simulation_steps)) {boundary="Native window closed";break;}
                dispatch_io();
                dispatch_timers();
                dispatch_video(std::chrono::steady_clock::now());
                if(!dpcs.empty()) for(auto& candidate:threads) if(candidate->state!=ThreadState::ended && !candidate->suspended) {
                    dispatch_dpcs(*candidate);break;
                }
            } catch(const std::exception& error) {boundary=error.what();break;}
            for(std::size_t offset=0;offset<threads.size();++offset) {
                const auto index=(next_runnable+offset)%threads.size();
                auto& candidate=threads[index];
                if(candidate->state==ThreadState::ended || candidate->suspended) continue;
                pending=true;
                if(candidate->state==ThreadState::waiting) {
                    if(candidate->due<=now || waiting_ready(*candidate))
                        candidate->state=ThreadState::ready;
                    else { earliest=std::min(earliest,candidate->due); continue; }
                }
                const auto priority=memory.load<std::uint8_t>(candidate->object+0x70);
                if(!selected || priority>selected_priority) {
                    selected=candidate.get();selected_priority=priority;selected_index=index;
                }
            }
            if(selected) next_runnable=(selected_index+1)%threads.size();
            for(const auto& [address,timer]:timers) earliest=std::min(earliest,timer.due);
            if(pending_io_count) earliest=std::min(earliest,now+std::chrono::milliseconds(1));
            if(host.vblank_callback_slot && memory.load<std::uint32_t>(host.vblank_callback_slot))
                earliest=std::min(earliest,video_started+std::chrono::nanoseconds(((video_ticks+1)*1000000000+59)/60));
            if(!selected) {
                if(!pending) break;
                if(earliest==std::chrono::steady_clock::time_point::max()) { boundary="All native guest threads are waiting without a signal source"; break; }
                if(host.idle) {PerfScope idle(host.performance,PerfPhase::pacing);host.idle(earliest);}
                else Sleep(1);
                continue;
            }
            current=selected; clock_data();
            quantum_due=std::chrono::steady_clock::now()+std::chrono::milliseconds(1);
            diagnostic_guest(&selected->cpu);
            {PerfScope guest_scope(host.performance,PerfPhase::guest);SwitchToFiber(selected->fiber);}
            diagnostic_guest(nullptr);
        }
        BootResult result;
        result.simulation_steps=simulation_steps;result.timing_epochs=timing_epochs;
        if(simulation_steps) result.simulation_ms=std::chrono::duration<double,std::milli>(timing_finished-timing_started).count();
        if(gpu) {
            gpu->cancel_capture("Execution stopped before the captured frame completed: "+boundary);
            const auto stats=gpu->stats();
            result.draws=stats.draws;result.frames=stats.flips;result.submissions=stats.submissions;result.vertices=stats.vertices;
            result.blits=stats.blits;result.graphics_callbacks=stats.callbacks;
            result.adapter=graphics->adapter();
            if(!host.capture.empty() && (stats.draws || stats.clears || stats.flips)) try {
                gpu->snapshot();save_png(graphics->readback(),host.capture,true);result.capture_written=true;
            } catch(const std::exception& error) {result.capture_error=error.what();}
        }
        result.entry_returned=entry_returned; result.main_reached=diagnostic.reached_watch;result.apcs_delivered=apcs_delivered;
        result.io.assign(io_trace.begin(),io_trace.end());
        if(!live) {
            result.allocations.reserve(allocations.size());
            for(const auto& [address,block]:allocations)
                result.allocations.push_back({address,block.size,static_cast<std::uint32_t>(std::ranges::count_if(block.pages,[](auto protect){return protect!=0;})*page),block.protect});
            result.last_failed_allocation=last_failed_allocation;
        }
        result.threads_created=static_cast<std::uint32_t>(threads.size());
        result.vblank_callbacks=vblank_callbacks;
        for(const auto& thread:threads) {
            const auto state=std::array{"ready","running","waiting","ended"}[static_cast<unsigned>(thread->state)];
            result.threads.push_back({thread->id,thread->routine,thread->system,thread->cpu.eip,
                thread->cpu.registers[esp],thread->wait_object,state,thread->suspended,
                memory.load<std::uint32_t>(thread->object+NativeThreadAbi::optional_callbacks)});
        }
        result.boundary=boundary; result.diagnostic=diagnostic; result.service_calls=service_calls;
        if(current) { result.cpu=current->cpu; result.active_thread=current->id; }
        const auto stack=result.cpu.registers[esp];
        if(stack<ram.size()) {
            result.stack_word_count=static_cast<std::uint32_t>(std::min<std::size_t>(result.stack_words.size(),(ram.size()-stack)/4));
            for(unsigned i=0;i<result.stack_word_count;++i) result.stack_words[i]=memory.load<std::uint32_t>(stack+i*4);
        }
        result.cpu.kernel=nullptr; result.cpu.diagnostic=nullptr; result.cpu.clock=nullptr; result.cpu.preempt=nullptr;
        // Resume sleeping native stacks with cancellation so their C++ and floating
        // scopes unwind before DeleteFiber releases host stacks.
        stopping=true;
        for(auto& thread : threads) if(thread->state!=ThreadState::ended) { current=thread.get(); SwitchToFiber(thread->fiber); }
        return result;
    }
};
Xbox::Xbox(Xbe& image,Memory& memory,std::span<std::byte> ram,const std::filesystem::path& disc,XboxHost host):state_(std::make_unique<State>(image,memory,ram,disc,std::move(host))) {}
Xbox::~Xbox()=default;
void Xbox::performance_recording(bool enabled) {if(state_->audio) state_->audio->recording(enabled);}
void Xbox::output_gain(float gain) {state_->output_gain=gain;if(state_->audio) state_->audio->output_gain(state_->host.mute_audio?0:gain);}
PerfCounters Xbox::performance_counters() const {
    PerfCounters counters{};const auto& s=*state_;
    if(s.gpu) {
        const auto g=s.gpu->stats();
        counters[unsigned(PerfMetric::draws)]=g.draws;counters[unsigned(PerfMetric::vertices)]=g.vertices;
        counters[unsigned(PerfMetric::submissions)]=g.submissions;counters[unsigned(PerfMetric::packets)]=g.packets;
        counters[unsigned(PerfMetric::methods)]=g.methods;counters[unsigned(PerfMetric::fences)]=g.fences;
        counters[unsigned(PerfMetric::displayed)]=s.host.presented_frames?s.host.presented_frames():0;
        counters[unsigned(PerfMetric::logic_ticks)]=s.simulation_steps;
    }
    if(s.graphics) {
        const auto r=s.graphics->stats();
        counters[unsigned(PerfMetric::buffer_maps)]=r.buffer_maps;counters[unsigned(PerfMetric::buffer_bytes)]=r.total_vertex_bytes;
        counters[unsigned(PerfMetric::texture_uploads)]=r.texture_uploads;counters[unsigned(PerfMetric::texture_bytes)]=r.texture_upload_bytes;
        counters[unsigned(PerfMetric::target_creations)]=r.target_creations;counters[unsigned(PerfMetric::target_copies)]=r.target_copies;
        counters[unsigned(PerfMetric::state_misses)]=r.state_misses;counters[unsigned(PerfMetric::layout_misses)]=r.layout_misses;
        counters[unsigned(PerfMetric::sampler_misses)]=r.sampler_misses;counters[unsigned(PerfMetric::program_creations)]=r.program_creations;
        counters[unsigned(PerfMetric::shader_compiles)]=r.shader_compilations;
    }
    if(s.audio) {
        const auto a=s.audio->stats();counters[unsigned(PerfMetric::audio_batches)]=a.batches;
        counters[unsigned(PerfMetric::audio_mix_ns)]=a.mix_ns;counters[unsigned(PerfMetric::audio_overruns)]=a.overruns;
    }
    return counters;
}
void Xbox::capture_frame(const std::filesystem::path& directory) {
    auto& s=*state_;
    if(!s.gpu) throw std::runtime_error("The game has not initialized graphics yet");
    s.gpu->capture_frame(directory,s.image.sha256);
    try {
        // Guest execution is paused in the host. Stream the existing RAM
        // without a second 64 MiB allocation or speculative title-specific slices.
        std::ofstream memory_file(directory/"guest-memory.bin",std::ios::binary);
        memory_file.exceptions(std::ios::failbit|std::ios::badbit);
        memory_file.write(reinterpret_cast<const char*>(s.ram.data()),static_cast<std::streamsize>(s.ram.size()));memory_file.close();
        std::ostringstream output;
        output<<std::format("{{\"format\":\"smbd-game-state-v1\",\"phase\":\"capture_request\",\"source_sha256\":{},\"frame\":{},\"active_thread\":{},\"gpu_cursor\":{},\"memory\":{{\"base\":0,\"size\":{},\"data\":\"guest-memory.bin\"}},\"recent_io_enabled\":{},\"audio_muted\":{},\"threads\":[",
            json(s.image.sha256),s.gpu->stats().flips,s.current?s.current->id:0,json(hex32(s.gpu_cursor)),s.ram.size(),s.host.trace_io,s.host.mute_audio);
        bool first=true;
        for(const auto& thread:s.threads) {
            if(!first) output<<',';first=false;const auto& cpu=thread->cpu;
            output<<std::format("{{\"id\":{},\"routine\":{},\"state\":{},\"suspended\":{},\"wait_object\":{},\"eip\":{},\"flags\":{},\"fs_base\":{},\"stack_base\":{},\"stack_size\":{},\"floating\":{},\"registers\":[",
                thread->id,json(hex32(thread->routine)),json(std::array{"ready","running","waiting","ended"}[unsigned(thread->state)]),
                thread->suspended,json(hex32(thread->wait_object)),json(hex32(cpu.eip)),json(hex32(cpu.flags)),json(hex32(cpu.fs_base)),
                json(hex32(thread->stack)),thread->stack_size,json(hex_bytes(std::as_bytes(std::span(&cpu.floating,1)))));
            for(unsigned i=0;i<cpu.registers.size();++i) {if(i) output<<',';output<<json(hex32(cpu.registers[i]));}
            output<<"]}";
        }
        output<<"],\"allocations\":[";first=true;
        for(const auto& [address,block]:s.allocations) {
            if(!first) output<<',';first=false;
            output<<std::format("{{\"address\":{},\"size\":{},\"protect\":{}}}",json(hex32(address)),block.size,json(hex32(block.protect)));
        }
        output<<"],\"io\":[";first=true;
        for(const auto& io:s.io_trace) {
            if(!first) output<<',';first=false;
            output<<std::format("{{\"ordinal\":{},\"status\":{},\"information\":{},\"callsite\":{},\"path\":{},\"options\":{},\"requested\":{},\"offset\":{},\"buffer\":{}}}",
                io.ordinal,json(hex32(io.status)),io.information,json(hex32(io.callsite)),json(io.path),io.options,io.requested,io.offset,json(hex32(io.buffer)));
        }
        output<<"],\"services\":[";first=true;
        for(unsigned i=0;i<s.service_calls.size();++i) if(s.service_calls[i]) {
            if(!first) output<<',';first=false;output<<std::format("{{\"ordinal\":{},\"calls\":{}}}",i,s.service_calls[i]);
        }
        write_text(directory/"game-state.json",output.str()+"]}\n",false);
        // An explicit capture arms the existing bounded recent-I/O history.
        s.host.trace_io=true;
    } catch(const std::exception& error) {s.gpu->cancel_capture(error.what());throw;}
}
const FrameCaptureStatus& Xbox::capture_status() const {
    static const FrameCaptureStatus empty;
    return state_->gpu?state_->gpu->capture_status():empty;
}
void Xbox::cancel_capture(std::string_view reason) {if(state_->gpu) state_->gpu->cancel_capture(reason);}
BootResult Xbox::run(std::uint32_t budget,std::uint32_t break_address,std::uint32_t break_hit) {
    if(break_address && !break_hit) throw std::runtime_error("A diagnostic breakpoint requires a positive hit count");
    return state_->run(budget,break_address,break_hit);
}
void native_platform(Cpu& cpu,Memory& memory,std::uint32_t address) {
    if(!cpu.kernel || !cpu.kernel->context || cpu.kernel->entries[255]!=Xbox::State::dispatch_kernel<255>)
        throw NativeBoundary(address,"native platform API requires the Xbox host provider");
    auto& host=*static_cast<Xbox::State*>(cpu.kernel->context);
    if(&host.memory!=&memory) throw NativeBoundary(address,"native platform API received another guest memory");
    HostFloatingScope floating(cpu.floating);
    host.platform(cpu,address);
}
void native_frame_wait(Cpu& cpu,Memory& memory) {
    if(!cpu.kernel || !cpu.kernel->context || cpu.kernel->entries[255]!=Xbox::State::dispatch_kernel<255>)
        throw NativeBoundary(cpu.eip,"game frame clock requires the Xbox host provider");
    auto& host=*static_cast<Xbox::State*>(cpu.kernel->context);
    if(&host.memory!=&memory) throw NativeBoundary(cpu.eip,"game frame clock received another guest memory");
    HostFloatingScope floating(cpu.floating);host.wait_frame(cpu);
}
void native_frame_begin(Cpu& cpu,Memory& memory) {
    if(!cpu.kernel || !cpu.kernel->context || cpu.kernel->entries[255]!=Xbox::State::dispatch_kernel<255>)
        throw NativeBoundary(cpu.eip,"game frame clock requires the Xbox host provider");
    auto& host=*static_cast<Xbox::State*>(cpu.kernel->context);
    if(&host.memory!=&memory) throw NativeBoundary(cpu.eip,"game frame clock received another guest memory");
    HostFloatingScope floating(cpu.floating);host.begin_frame();
}
void native_stage_time(Cpu& cpu,Memory& memory) {
    if(!cpu.kernel || !cpu.kernel->context || cpu.kernel->entries[255]!=Xbox::State::dispatch_kernel<255>)
        throw NativeBoundary(cpu.eip,"stage clock requires the Xbox host provider");
    auto& host=*static_cast<Xbox::State*>(cpu.kernel->context);
    if(&host.memory!=&memory) throw NativeBoundary(cpu.eip,"stage clock received another guest memory");
    HostFloatingScope floating(cpu.floating);host.sample_stage_time(cpu);
}
void native_sample_goals(Cpu& cpu,Memory& memory) {
    if(!cpu.kernel || !cpu.kernel->context || cpu.kernel->entries[255]!=Xbox::State::dispatch_kernel<255>)
        throw NativeBoundary(cpu.eip,"goal sampling requires the Xbox host provider");
    auto& host=*static_cast<Xbox::State*>(cpu.kernel->context);
    if(&host.memory!=&memory) throw NativeBoundary(cpu.eip,"goal sampling received another guest memory");
    host.sample_goals(cpu);
}
bool native_goal_query(Cpu& cpu,Memory& memory,std::uint32_t query) {
    if(!cpu.kernel || !cpu.kernel->context || cpu.kernel->entries[255]!=Xbox::State::dispatch_kernel<255>) return false;
    auto& host=*static_cast<Xbox::State*>(cpu.kernel->context);
    if(&host.memory!=&memory) throw NativeBoundary(cpu.eip,"goal query received another guest memory");
    return host.take_goal(cpu,query);
}
}
