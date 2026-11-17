#include "pipeline.h"
#include "cpu.h"
#include "frame_step.h"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <functional>
#include <iomanip>
#include <sstream>

namespace smbd {
double frame_gain_double(FloatingState&,double);
void reset_frame_history();
// Recipes contain reset, reg NAME WORD, xmm INDEX WORD WORD WORD WORD,
// write32 ADDRESS WORD, step DELTA_BITS PHASE_BITS LOGIC, load ADDRESS FILE,
// call ENTRY CHECKPOINT_LIMIT STACK [ARG...],
// call-until ENTRY BREAKPOINT CHECKPOINT_LIMIT STACK [ARG...], and
// read ADDRESS BYTES. Calls execute only the verified AOT registry.
std::string probe_cpu(Xbe& image,const std::filesystem::path& capture,const std::filesystem::path& recipe) {
    struct Restore {FrameStep saved=frame_step;~Restore(){frame_step=saved;}} restore;
    frame_step={};reset_frame_history();
    if(!image.supported()) throw std::runtime_error("CPU probes require the supported executable");
    for(const auto& function:compiled_batch)
        if(function.source_sha256!=image.sha256) throw std::runtime_error("Compiled code belongs to another executable");
    File commands(recipe);
    if(commands.size()>65536) throw std::runtime_error("CPU probe recipe exceeds its fixed bound");
    std::vector<std::byte> original;
    std::string snapshot_hash;
    if(capture=="-") {
        original.resize(64*1024*1024);
        if(std::uint64_t(image.base)+image.image_size>original.size()) throw std::runtime_error("CPU probe image exceeds memory");
        image.load(std::span(original).subspan(image.base,image.image_size),false);
    } else {
        File metadata(capture/"capture.json"),snapshot(capture/"guest-memory.bin");
        if(metadata.size()>4096 || snapshot.size()!=64*1024*1024)
            throw std::runtime_error("CPU probe capture exceeds its fixed bounds");
        const auto meta=metadata.read(0,static_cast<std::size_t>(metadata.size()));
        const std::string_view description(reinterpret_cast<const char*>(meta.data()),meta.size());
        if(description.find("\"source_sha256\":"+json(image.sha256))==description.npos)
            throw std::runtime_error("CPU snapshot belongs to another executable");
        original=snapshot.read(0,static_cast<std::size_t>(snapshot.size()));
        snapshot_hash=hash(snapshot,0,snapshot.size());
    }
    auto storage=original;Memory memory(0,storage);
    const auto heap_begin=(image.base+image.image_size+4095U)&~4095U;
    if(heap_begin>=storage.size()) throw std::runtime_error("CPU snapshot has no heap range");
    memory.map(0x80000000U+heap_begin,std::span(storage).subspan(heap_begin));
    memory.map(0xF0000000U+heap_begin,std::span(storage).subspan(heap_begin));
    struct VirtualSnapshot {std::uint32_t base;std::vector<std::byte> original,bytes;};
    std::vector<VirtualSnapshot> virtual_ranges;std::uint64_t virtual_bytes=0;
    std::string virtual_hash;
    if(capture!="-" && std::filesystem::exists(capture/"virtual-memory.bin")) {
        File virtual_file(capture/"virtual-memory.bin");
        if(virtual_file.size()<16 || virtual_file.size()>64*1024*1024+16384*8+16)
            throw std::runtime_error("Virtual snapshot exceeds its physical-memory bound");
        const auto magic=virtual_file.read(0,8);
        if(std::memcmp(magic.data(),"SMBDVM1\n",8)) throw std::runtime_error("Invalid virtual snapshot header");
        std::uint64_t offset=8;
        for(;;) {
            const auto header=virtual_file.read(offset,8);offset+=8;
            const auto base=u32(header,0),size=u32(header,4);
            if(!base && !size) {if(offset!=virtual_file.size()) throw std::runtime_error("Trailing virtual snapshot bytes");break;}
            if(base<storage.size() || (base&4095) || !size || (size&4095) || std::uint64_t(base)+size>0x7FFF0000 ||
                virtual_bytes+size>64*1024*1024 || virtual_ranges.size()>=16384)
                throw std::runtime_error("Invalid virtual snapshot range");
            auto bytes=virtual_file.read(offset,size);offset+=size;virtual_bytes+=size;
            virtual_ranges.push_back({base,bytes,std::move(bytes)});
            auto& range=virtual_ranges.back();memory.map_dynamic(base,range.bytes);
        }
        virtual_hash=hash(virtual_file,0,virtual_file.size());
    }
    const auto recipe_bytes=commands.read(0,static_cast<std::size_t>(commands.size()));
    std::istringstream lines(std::string(reinterpret_cast<const char*>(recipe_bytes.data()),recipe_bytes.size()));
    const auto number=[](std::string text) {
        const bool hex=text.starts_with("0x") || text.starts_with("0X");
        std::uint32_t value=0;const auto first=text.data()+(hex?2:0);
        const auto parsed=std::from_chars(first,text.data()+text.size(),value,hex?16:10);
        if(parsed.ec!=std::errc{} || parsed.ptr!=text.data()+text.size() || first==parsed.ptr)
            throw std::runtime_error("Invalid CPU probe word: "+text);
        return value;
    };
    auto started=std::chrono::steady_clock::now();
    GuestClock clock{&started,[](void* context) {
        const auto ns=std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now()-*static_cast<std::chrono::steady_clock::time_point*>(context)).count());
        return (ns/1000000000)*733333333+(ns%1000000000)*733333333/1000000000;
    }};
    Cpu cpu;cpu.clock=&clock;std::ostringstream records;std::string error,line;
    std::uint64_t budget=0;unsigned line_number=0,calls=0,read_bytes=0,loaded_bytes=0,records_count=0;
    const auto record=[&](const std::string& text) {if(records_count++) records<<',';records<<text;};
    try {
        while(std::getline(lines,line)) {
            if(++line_number>8192) throw std::runtime_error("CPU probe command limit exceeded");
            std::istringstream words(line);std::string command;words>>command;
            if(command.empty() || command.starts_with('#')) continue;
            const auto next=[&] {std::string text;if(!(words>>text)) throw std::runtime_error("Missing CPU probe operand");return number(text);};
            if(command=="reset") {
                std::memcpy(storage.data(),original.data(),storage.size());
                for(auto& range:virtual_ranges) std::memcpy(range.bytes.data(),range.original.data(),range.bytes.size());
                cpu=Cpu{};cpu.clock=&clock;frame_step={};reset_frame_history();
            }
            else if(command=="step") {
                const auto delta=std::bit_cast<float>(next()),phase=std::bit_cast<float>(next());const auto logic=next();
                if(!std::isfinite(delta) || delta<=0 || delta>2 || !std::isfinite(phase) || phase<0 || phase>=1 || logic>1)
                    throw std::runtime_error("Invalid CPU probe timestep");
                advance_frame_step(delta,phase,logic!=0);
            }
            else if(command=="reg") {
                std::string name;words>>name;const auto value=next();
                constexpr std::string_view names[]={"eax","ecx","edx","ebx","esp","ebp","esi","edi"};
                const auto at=std::ranges::find(names,name);
                if(at!=std::end(names)) cpu.registers[at-std::begin(names)]=value;
                else if(name=="flags") cpu.flags=value;
                else if(name=="fs_base") cpu.fs_base=value;
                else if(name=="gs_base") cpu.gs_base=value;
                else if(name=="mxcsr" && !(value&~0xFFFFU)) cpu.floating.mxcsr=value;
                else if(name=="ultrawide" && value<=1) cpu.ultrawide=value!=0;
                else if(name=="unlimited_render_distance" && value<=1) cpu.unlimited_render_distance=value!=0;
                else throw std::runtime_error("Unknown CPU probe register");
            } else if(command=="xmm") {
                const auto index=next();if(index>=8) throw std::runtime_error("Invalid CPU probe XMM index");
                for(auto& value:cpu.floating.xmm[index]) value=next();
            } else if(command=="write32") {const auto address=next(),value=next();memory.store<std::uint32_t>(address,value);}
            else if(command=="load") {
                const auto address=next();std::string text;
                if(!(words>>std::quoted(text))) throw std::runtime_error("Missing CPU probe asset path");
                auto path=utf8_path(text);if(path.is_relative()) path=recipe.parent_path()/path;
                File asset(path);
                if(!asset.size() || asset.size()>1024*1024 || loaded_bytes+asset.size()>8*1024*1024)
                    throw std::runtime_error("CPU probe asset load limit exceeded");
                const auto size=static_cast<std::size_t>(asset.size());
                asset.read(0,{static_cast<std::byte*>(memory.access(address,size)),size});
                loaded_bytes+=static_cast<unsigned>(size);
                record(std::format("{{\"line\":{},\"load\":{},\"address\":{},\"size\":{},\"sha256\":{}}}",line_number,json(path_text(path)),json(hex32(address)),size,json(hash(asset,0,size))));
            }
            else if(command=="read") {
                const auto address=next(),size=next();
                if(size>4096 || read_bytes+size>1024*1024) throw std::runtime_error("CPU probe read limit exceeded");
                read_bytes+=size;
                record(std::format("{{\"line\":{},\"address\":{},\"bytes\":{}}}",line_number,json(hex32(address)),json(hex_bytes(Bytes(static_cast<const std::byte*>(memory.access(address,size)),size)))));
            } else if(command=="call" || command=="call-until") {
                const auto entry=next(),stop=command=="call-until"?next():0,limit=next(),stack=next();
                if(command=="call-until") {if(!stop) throw std::runtime_error("CPU probe breakpoint is zero");image.code_section(stop);}
                if(!limit || limit>1000000 || budget+limit>16777216 || ++calls>4096)
                    throw std::runtime_error("CPU probe execution limit exceeded");
                budget+=limit;image.code_section(entry);
                constexpr std::uint32_t sentinel=0xDEADC0DE;
                cpu.registers[esp]=stack;memory.store<std::uint32_t>(stack,sentinel);
                std::string argument;unsigned count=0;
                while(words>>argument) {
                    if(++count>32) throw std::runtime_error("CPU probe argument limit exceeded");
                    memory.store<std::uint32_t>(stack+4*count,number(argument));
                }
                ExecutionDiagnostic diagnostic;diagnostic.remaining=limit;diagnostic.break_address=stop;cpu.diagnostic=&diagnostic;
                bool stopped=false;
                try {invoke_native(cpu,memory,entry);}
                catch(const NativeBoundary& boundary) {
                    cpu.diagnostic=nullptr;
                    if(!stop || boundary.address!=stop || diagnostic.break_hit) throw;
                    stopped=true;
                } catch(...) {cpu.diagnostic=nullptr;throw;}
                cpu.diagnostic=nullptr;
                if(stop?!stopped:cpu.eip!=sentinel) throw std::runtime_error("CPU probe routine missed its requested return or breakpoint");
                std::string registers;
                for(auto value:cpu.registers) {if(!registers.empty()) registers+=',';registers+=json(hex32(value));}
                record(std::format("{{\"line\":{},\"entry\":{},\"checkpoints\":{},\"returned\":{},\"breakpoint\":{},\"registers\":[{}],\"flags\":{}}}",line_number,json(hex32(entry)),diagnostic.visits,!stopped,json(hex32(stop)),registers,json(hex32(cpu.flags))));
            } else throw std::runtime_error("Unknown CPU probe command: "+command);
            std::string extra;if(words>>extra) throw std::runtime_error("Unexpected CPU probe operand");
        }
    } catch(const std::exception& failure) {error=failure.what();cpu.diagnostic=nullptr;}
    return std::format("{{\"format\":\"smbd-cpu-probe-v1\",\"passed\":{},\"source_sha256\":{},\"snapshot_sha256\":{},\"virtual_snapshot_sha256\":{},\"virtual_snapshot_bytes\":{},\"recipe_sha256\":{},\"calls\":{},\"loaded_bytes\":{},\"line\":{},\"error\":{},\"eip\":{},\"records\":[{}]}}",
        error.empty(),json(image.sha256),json(snapshot_hash),json(virtual_hash),virtual_bytes,json(hash(commands,0,commands.size())),calls,loaded_bytes,line_number,json(error),json(hex32(cpu.eip)),records.str());
}
std::string check_cpu(Xbe& image) {
    if (!image.supported()) throw std::runtime_error("CPU diagnostics require the supported executable");
    for (const auto& function : compiled_batch)
        if (function.source_sha256 != image.sha256) throw std::runtime_error("Compiled code belongs to another executable");
    constexpr std::uint32_t data = 0x00C00000, tls = 0x00C10000, stack = 0x00F00000;
    constexpr std::uint32_t sentinel = 0xDEADC0DE;
    if (std::uint64_t(image.base) + image.image_size > data)
        throw std::runtime_error("Diagnostic workspace overlaps the executable image");
    std::vector<std::byte> storage(16 * 1024 * 1024);
    image.load(std::span(storage).first(image.image_size), false);
    Memory memory(image.base, storage);
    const auto require = [](bool condition, std::string_view reason) {
        if (!condition) throw std::runtime_error(std::string(reason));
    };
    const auto run = [&](std::uint32_t address, std::initializer_list<std::uint32_t> arguments,bool cdecl=false,std::uint32_t receiver=0,unsigned receiver_register=eax) {
        Cpu cpu;
        cpu.registers[receiver_register]=receiver;
        ExecutionDiagnostic diagnostic;
        diagnostic.remaining = 10000;
        cpu.diagnostic = &diagnostic;
        cpu.fs_base = tls;
        cpu.registers[esp] = stack;
        memory.store<std::uint32_t>(tls, UINT32_MAX);
        memory.store<std::uint32_t>(stack, sentinel);
        unsigned index = 1;
        for (const auto argument : arguments) memory.store<std::uint32_t>(stack + 4 * index++, argument);
        invoke_native(cpu, memory, address);
        require(cpu.eip == sentinel, "Original routine returned to the wrong address");
        require(cpu.registers[esp] == stack + 4 * (cdecl?1:index), "Original routine left the wrong stack position");
        cpu.diagnostic = nullptr;
        return cpu;
    };
    std::ostringstream results;
    unsigned cases = 0, failures = 0;
    const auto check = [&](std::string_view name, std::uint32_t address, const std::function<void()>& execute) {
        if (cases++) results << ',';
        results << "{\"name\":" << json(name) << ",\"address\":" << json(hex32(address));
        try { execute(); results << ",\"passed\":true}"; }
        catch (const std::exception& error) { ++failures; results << ",\"passed\":false,\"reason\":" << json(error.what()) << '}'; }
    };
    check("arithmetic parity across byte values and operand widths",0,[&] {
        for(unsigned low=0;low<256;++low) {
            const auto expected=(std::popcount(low)&1)?0U:parity;
            for(const auto high:{0U,0x12345600U,0xFFFFFF00U}) {
                const auto value=high|low;
                require((szp<8>(value)&parity)==expected && (szp<16>(value)&parity)==expected &&
                        (szp<32>(value)&parity)==expected,"Arithmetic parity changed");
            }
        }
    });
    for (const std::string_view text : {"", "Super Monkey Ball Deluxe", "123456789"}) {
        check("original lstrlenA", 0x00308BDE, [&] {
            std::memcpy(memory.access(data, text.size() + 1), text.data(), text.size() + 1);
            const auto cpu = run(0x00308BDE, {data});
            require(cpu.registers[eax] == text.size(), "String length differs");
            require(memory.load<std::uint32_t>(tls) == UINT32_MAX, "SEH chain was not restored");
        });
    }
    for (const std::array<float, 3> scale : {std::array{2.0f, -3.0f, 4.0f}, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}}) {
        check("original D3DXMatrixScaling", 0x0032E876, [&] {
            std::memset(memory.access(data, 64), 0xA5, 64);
            const auto cpu = run(0x0032E876, {data, std::bit_cast<std::uint32_t>(scale[0]),
                std::bit_cast<std::uint32_t>(scale[1]), std::bit_cast<std::uint32_t>(scale[2])});
            require(cpu.registers[eax] == data, "Scaling returned the wrong output pointer");
            for (unsigned i = 0; i < 16; ++i) {
                const float expected = i == 0 ? scale[0] : i == 5 ? scale[1] : i == 10 ? scale[2] : i == 15 ? 1.0f : 0.0f;
                require(memory.load<std::uint32_t>(data + i * 4) == std::bit_cast<std::uint32_t>(expected),
                    "Scaling matrix differs from independent expected values");
            }
        });
    }
    for (const std::array<float, 3> vector : {std::array{3.0f, 4.0f, 0.0f}, {-2.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 5.0f}}) {
        check("original D3DXVec3Normalize", 0x0032EB5A, [&] {
            std::memcpy(memory.access(data + 128, 12), vector.data(), 12);
            const auto cpu = run(0x0032EB5A, {data, data + 128});
            require(cpu.registers[eax] == data, "Normalization returned the wrong output pointer");
            const auto length = std::sqrt(double(vector[0]) * vector[0] + double(vector[1]) * vector[1] + double(vector[2]) * vector[2]);
            for (unsigned i = 0; i < 3; ++i)
                require(std::abs(std::bit_cast<float>(memory.load<std::uint32_t>(data + i * 4)) - vector[i] / length) < 0.000001,
                    "Normalized vector differs from independently calculated values");
        });
    }
    check("original D3DXMatrixMultiply", 0x0032EBD6, [&] {
        const std::array<float, 16> left{1,2,3,4, 5,6,7,8, 2,0,-2,1, 0,0,0,1};
        const std::array<float, 16> right{2,0,0,0, 0,3,0,0, 0,0,4,0, 1,2,3,1};
        std::memcpy(memory.access(data + 128, 64), left.data(), 64);
        std::memcpy(memory.access(data + 256, 64), right.data(), 64);
        const auto cpu = run(0x0032EBD6, {data, data + 128, data + 256});
        require(cpu.registers[eax] == data, "Multiplication returned the wrong output pointer");
        for (unsigned row = 0; row < 4; ++row) for (unsigned column = 0; column < 4; ++column) {
            float expected = 0;
            for (unsigned k = 0; k < 4; ++k) expected += left[row * 4 + k] * right[k * 4 + column];
            require(std::bit_cast<float>(memory.load<std::uint32_t>(data + (row * 4 + column) * 4)) == expected,
                "Matrix product differs from independently calculated values");
        }
    });
    check("original ball integration at 30..1000 Hz",0x0008A2C0,[&] {
        struct Restore {FrameStep saved=frame_step;~Restore(){frame_step=saved;}} restore;
        const auto store=[&](std::uint32_t address,float value){memory.store<std::uint32_t>(address,std::bit_cast<std::uint32_t>(value));};
        const auto load=[&](std::uint32_t address){return std::bit_cast<float>(memory.load<std::uint32_t>(address));};
        constexpr std::array<float,3> position{2,20,-3},velocity{0.3f,-0.4f,0.2f};
        for(const unsigned rate:{30,60,120,144,165,240,360,1000}) {
            std::memset(memory.access(data,0x1B0),0,0x1B0);
            for(unsigned axis=0;axis<3;++axis) {store(data+4+axis*4,position[axis]);store(data+0x1C+axis*4,velocity[axis]);}
            store(data+0x6C,0.01f);memory.store<std::uint32_t>(data+0x9C,1);
            frame_step.delta=60.0f/rate;
            for(unsigned frame=0;frame<rate*2;++frame) run(0x0008A2C0,{data},true);
            for(unsigned axis=0;axis<3;++axis) {
                const double acceleration=axis==1?-double(0.01f):0;
                const double expected_velocity=velocity[axis]+acceleration*120;
                const double expected_position=position[axis]+velocity[axis]*120+acceleration*120*121/2;
                require(std::abs(load(data+0x1C+axis*4)-expected_velocity)<0.0002,"Ball velocity depends on refresh rate");
                require(std::abs(load(data+4+axis*4)-expected_position)<0.02,"Ball displacement depends on refresh rate");
            }
        }
    });
    check("original sprite search loop",0x00073900,[&] {
        memory.store<std::uint32_t>(0x6C93B4,data+0x1000);
        std::memset(memory.access(data+0x1000,80),0,80);
        memory.store<std::uint8_t>(data+0x1003,1);
        memory.store<std::uint8_t>(0x7289F8+3*0xD0+15,0xFF);
        auto cpu=run(0x73900,{0xFFFFFFFF},true);
        require(cpu.registers[eax]==0x7289F8+3*0xD0 && cpu.registers[ecx]==3 && cpu.registers[edx]==data+0x1003,"Sprite search lost its signed tag or register result");
        cpu.flags=0x202|arithmetic_flags;cpu.registers[esp]=stack;
        memory.store<std::uint32_t>(stack,sentinel);memory.store<std::uint32_t>(stack+4,0xFFFFFFFF);
        invoke_native(cpu,memory,0x73900);
        require(cpu.flags==0x246,"Sprite search must retain the original successful CMP flags");
        cpu=run(0x73900,{128},true);
        require(cpu.registers[eax]==0 && cpu.registers[ecx]==80 && cpu.registers[edx]==data+0x1050,"Sprite search exceeded its pool or matched an inactive entry");
    });
    check("original Baseball UI array callback publication and dispatch",0x00077080,[&] {
        constexpr auto master=data+0x2000,flags=data+0x1000;
        std::memset(memory.access(master,0x500),0,0x500);
        std::memset(memory.access(flags,80),0,80);
        memory.store<std::uint32_t>(0x6C93A8,80);memory.store<std::uint32_t>(0x6C93AC,0);
        memory.store<std::uint32_t>(0x6C93B0,0);memory.store<std::uint32_t>(0x6C93B4,flags);
        memory.store<std::uint32_t>(0x7700A4,master);
        const auto reach=[&](std::uint32_t entry,std::uint32_t stop,std::uint32_t row=0) {
            Cpu cpu;ExecutionDiagnostic diagnostic;diagnostic.remaining=10000;diagnostic.break_address=stop;
            cpu.diagnostic=&diagnostic;cpu.registers[esp]=stack;cpu.registers[esi]=row;
            memory.store<std::uint32_t>(stack,sentinel);
            bool reached=false;
            try {invoke_native(cpu,memory,entry);}
            catch(const NativeBoundary& boundary) {
                reached=boundary.address==stop && cpu.eip==stop &&
                    std::string_view(boundary.what()).find("execution diagnostic breakpoint reached")!=std::string_view::npos;
                if(!reached) throw;
            }
            require(reached,"Original Baseball path did not reach the requested boundary");cpu.diagnostic=nullptr;return cpu;
        };
        // Execute the real builder until immediately after all three stores.
        reach(0x000F3E40,0x000F3B50);
        for(unsigned index=1;index<=3;++index) {
            const auto row=memory.load<std::uint32_t>(master+0x338+(50+index)*4);
            require(row==0x7289F8+index*0xD0 && memory.load<std::uint32_t>(row+0x38)==0xF3AA0,
                "Original Baseball builder published a different UI callback");
            const auto cpu=reach(0x00077080,0x000F3AA0,row);
            require(memory.load<std::uint32_t>(cpu.registers[esp])==0x770DD &&
                memory.load<std::uint32_t>(cpu.registers[esp]+4)==row,
                "Baseball callback lost its row argument or draw-loop return address");
        }
        const auto row=memory.load<std::uint32_t>(master+0x338+51*4);
        memory.store<std::uint32_t>(row+0x38,0x73900);
        bool rejected=false;
        try {reach(0x00077080,0x00073900,row);}
        catch(const NativeBoundary& boundary) {
            rejected=std::string_view(boundary.what()).find("call target is outside its verified table")!=std::string_view::npos;
            if(!rejected) throw;
        }
        require(rejected,"UI dispatch admitted an unrelated compiled function");
    });
    check("original Baseball actor dispatch for both player callbacks",0x0009DAB0,[&] {
        constexpr auto actor=data+0x3000;
        std::memset(memory.access(actor,0x2F0),0,0x2F0);
        std::memset(memory.access(0x738DF0,8*0x1B0),0,8*0x1B0);
        memory.store<std::uint8_t>(0x892114,0);
        const auto reach=[&](std::uint32_t target) {
            Cpu cpu;ExecutionDiagnostic diagnostic;diagnostic.remaining=10000;diagnostic.break_address=target;
            cpu.diagnostic=&diagnostic;cpu.registers[esp]=stack;
            memory.store<std::uint32_t>(stack,sentinel);
            bool reached=false;
            try {invoke_native(cpu,memory,0x0009DAB0);}
            catch(const NativeBoundary& boundary) {
                reached=boundary.address==target && cpu.eip==target &&
                    std::string_view(boundary.what()).find("execution diagnostic breakpoint reached")!=std::string_view::npos;
                if(!reached) throw;
            }
            require(reached,"Original actor dispatch did not reach the requested callback");
            require(memory.load<std::uint32_t>(cpu.registers[esp])==0x9DAFF &&
                memory.load<std::uint32_t>(cpu.registers[esp]+4)==actor &&
                memory.load<std::uint32_t>(cpu.registers[esp]+8)==0,
                "Actor callback lost its arguments or return address");
        };
        for(unsigned slot=0;slot<8;++slot) {
            const auto record=0x738DF0+slot*0x1B0;
            memory.store<std::uint8_t>(record,2);memory.store<std::uint32_t>(record+0x104,actor);
            for(const auto literal:{0x235BAC,0x235BFC}) {
                const auto target=memory.load<std::uint32_t>(literal);
                memory.store<std::uint32_t>(actor+0x5C,target);
                reach(target);
            }
            memory.store<std::uint8_t>(record,0);
        }
        memory.store<std::uint8_t>(0x738DF0,2);memory.store<std::uint32_t>(0x738EF4,actor);
        memory.store<std::uint32_t>(actor+0x5C,0x73900);
        bool rejected=false;
        try {reach(0x73900);}
        catch(const NativeBoundary& boundary) {
            rejected=std::string_view(boundary.what()).find("call target is outside its verified table")!=std::string_view::npos;
            if(!rejected) throw;
        }
        require(rejected,"Actor dispatch admitted an unrelated compiled function");
        memory.store<std::uint8_t>(0x738DF0,0);
    });
    check("original main-game force integration at 30..1000 Hz",0x0008FE80,[&] {
        struct Restore {FrameStep saved=frame_step;~Restore(){frame_step=saved;}} restore;
        const auto store=[&](std::uint32_t address,float value){memory.store<std::uint32_t>(address,std::bit_cast<std::uint32_t>(value));};
        std::array<float,6> reference{};
        store(0x73E940,0);store(0x73E944,1);store(0x73E948,0);
        for(const unsigned rate:{60,30,120,144,165,240,360,1000}) {
            std::memset(memory.access(data,0x1B0),0,0x1B0);
            for(unsigned axis=0;axis<3;++axis) {store(data+4+axis*4,axis==1?20.0f:2.0f);store(data+0x1C+axis*4,axis==1?-0.4f:0.3f);}
            store(data+0x6C,0.01f);memory.store<std::uint32_t>(data+0x9C,1);
            frame_step.delta=60.0f/rate;
            for(unsigned frame=0;frame<rate*2;++frame) run(0x0008FE80,{},false,data);
            for(unsigned axis=0;axis<3;++axis) for(unsigned kind=0;kind<2;++kind) {
                const auto value=std::bit_cast<float>(memory.load<std::uint32_t>(data+(kind?0x1C:4)+axis*4));
                if(rate==60) reference[kind*3+axis]=value;
                else if(!std::isfinite(value) || !std::isfinite(reference[kind*3+axis]) ||
                        std::abs(value-reference[kind*3+axis])>=(kind?0.0002:0.02))
                    throw std::runtime_error(std::format("Main-game integration at {} Hz, axis {}, kind {}: {} versus {}",rate,axis,kind,value,reference[kind*3+axis]));
            }
        }
    });
    check("original force and contact drag preserve the 60 Hz trajectory",0x0008FE80,[&] {
        struct Restore {FrameStep saved=frame_step;~Restore(){frame_step=saved;}} restore;
        const auto store=[&](std::uint32_t address,float value){memory.store<std::uint32_t>(address,std::bit_cast<std::uint32_t>(value));};
        memory.store<std::uint32_t>(0x6C7EA8,60);
        store(0x73E940,0);store(0x73E944,-0.9205058f);store(0x73E948,-0.390729f);
        std::array<float,4> reference{};
        for(const unsigned rate:{60,30,120,144,165,240,360,1000}) {
            std::memset(memory.access(data,0x300),0,0x300);
            store(data+0x24,-0.2f);store(data+0x6C,0.01f);store(data+0x120,-1);
            frame_step.delta=60.0f/rate;
            for(unsigned frame=0;frame<rate*2;++frame) {
                memory.store<std::uint32_t>(data+0x98,1);memory.store<std::uint32_t>(data+0x9C,1);memory.store<std::uint32_t>(data+0x128,1);
                run(0x8FE80,{},false,data);
                run(0x8B3B0,{data,data+0x200},true);
                const auto loss=std::bit_cast<float>(memory.load<std::uint32_t>(data+0x258));
                for(const unsigned axis:{0U,2U}) {const auto address=data+0x1C+axis*4;const auto v=std::bit_cast<float>(memory.load<std::uint32_t>(address));store(address,v-v*loss);}
                store(data+8,0);store(data+0x20,0);
            }
            unsigned component=0;
            for(const unsigned offset:{4U,12U,0x1CU,0x24U}) {
                const auto value=std::bit_cast<float>(memory.load<std::uint32_t>(data+offset));
                if(rate==60) reference[component]=value;
                else if(!std::isfinite(value) || std::abs(value-reference[component])>=(component<2?0.02f:0.0002f))
                    throw std::runtime_error(std::format("Ground trajectory at {} Hz, component {}: {} versus {}",rate,component,value,reference[component]));
                ++component;
            }
        }
    });
    check("original moving-platform velocity at 30..1000 Hz",0x0007B320,[&] {
        struct Restore {FrameStep saved=frame_step;~Restore(){frame_step=saved;}} restore;
        const auto store=[&](std::uint32_t address,float value){memory.store<std::uint32_t>(address,std::bit_cast<std::uint32_t>(value));};
        memory.store<std::uint32_t>(0x72FF78,data+0x1000);
        for(const unsigned rate:{30,60,120,144,165,240,360,1000}) {
            std::memset(memory.access(data,0x2000),0,0x2000);
            frame_step.delta=60.0f/rate;
            const auto group=data+0x1000+0xA4;
            for(const unsigned matrix:{group+0x30,group+0x60}) for(unsigned axis=0;axis<3;++axis) store(matrix+axis*20,1);
            store(group+0x3C,frame_step.delta);
            run(0x7B320,{data,1},true);
            const auto relative=std::bit_cast<float>(memory.load<std::uint32_t>(data+0x1C));
            require(std::isfinite(relative) && std::abs(relative+1)<0.00001f,"Platform-relative speed depends on refresh rate");
            memory.store<std::uint32_t>(0x892118,0);
            memory.store<std::uint32_t>(0x730058,0);
            run(0x7E300,{data,data+0x1800},true);
            const auto world=std::bit_cast<float>(memory.load<std::uint32_t>(data+0x1C));
            require(std::isfinite(world) && std::abs(world)<0.00001f,"Platform transform changed world speed");
            // The main collision routine has its own return-to-world conversion.
            // Exercise a rotating surface as well as translation through that path.
            std::memset(memory.access(data,0x100),0,0x100);
            const float angle=0.01f*frame_step.delta,c=std::cos(angle),s=std::sin(angle);
            store(group+0x30,c);store(group+0x38,-s);store(group+0x50,s);store(group+0x58,c);
            store(data+4,3);store(data+0xC,7);store(data+0x10,3);store(data+0x18,7);
            store(data+0x1C,0.04f);store(data+0x24,0.02f);
            run(0x7B320,{data,1},true);
            run(0x7E300,{data,data+0x1800},true);
            require(std::abs(std::bit_cast<float>(memory.load<std::uint32_t>(data+0x1C))-0.04f)<0.0001f &&
                std::abs(std::bit_cast<float>(memory.load<std::uint32_t>(data+0x24))-0.02f)<0.0001f,
                "Rotating platform changed world speed");
        }
    });
    check("stage group clocks and integer rotation retain fractional time",0x0007F520,[&] {
        struct Restore {FrameStep saved=frame_step;~Restore(){frame_step=saved;}} restore;
        for(const unsigned rate:{60,75,120,144,165,240,360,1000}) for(const int speed:{-3640,-512,-1,0,1,257,1024,3640}) {
            FrameCounter clock;std::uint32_t count=0;std::int32_t angle=0;double phase=0;
            frame_step={};clock.sample(0,0);
            for(unsigned frame=0;frame<rate*2;++frame) {
                phase+=60.0/rate;const bool tick=phase>=1-1e-9;if(tick) {phase=std::max(0.0,phase-1);count+=speed;}
                advance_frame_step(60.0f/rate,static_cast<float>(phase),tick);
                clock.sample(count,speed);angle+=frame_integer_increment(speed);
            }
            require(std::abs(clock.value-120.0*speed)<0.00001*std::max(1,std::abs(speed)),"Stage group clock depends on rendering rate");
            require(angle==120*speed,"Integer animation lost its fractional rotation");
            const auto paused=clock.value;
            for(unsigned frame=0;frame<rate;++frame) clock.sample(count,0);
            require(clock.value==paused,"Paused group animation advanced");
            require(clock.sample(12345,0)==12345,"External group clock reset was ignored");
        }
    });
    check("original primary and secondary goal query ABIs and sweeps",0x00012F30,[&] {
        std::memset(memory.access(data,0x2000),0,0x2000);
        const auto scene=data+0x1000,groups=data+0x1200,goal=data+0x1800,secondary=data+0x1840,output=data+0x400;
        memory.store<std::uint32_t>(0x730130,scene);
        memory.store<std::uint32_t>(scene+8,1);
        memory.store<std::uint32_t>(scene+12,groups);
        memory.store<std::uint32_t>(groups+0x44,1);memory.store<std::uint32_t>(groups+0x48,goal);
        memory.store<std::uint8_t>(goal+0x12,'B');
        memory.store<std::uint32_t>(groups+0xC4,1);memory.store<std::uint32_t>(groups+0xC8,secondary);
        const auto store=[&](std::uint32_t address,float value){memory.store<std::uint32_t>(address,std::bit_cast<std::uint32_t>(value));};
        memory.store<std::uint32_t>(data,0x20000002);
        store(data+4,0.5f);store(data+8,1.5f);store(data+12,-1);
        store(data+16,0.5f);store(data+20,1.5f);
        for(const unsigned query:{0x12F30U,0x13180U}) {
            const bool primary=query==0x12F30;
            const auto invoke=[&]{return primary?run(query,{output,output+4,output+8},true,data,ecx):run(query,{output},true,data,ecx);};
            store(data+24,1);
            for(unsigned offset=0;offset<12;offset+=4) memory.store<std::uint32_t>(output+offset,UINT32_MAX);
            auto cpu=invoke();
            require(cpu.registers[eax]==1 && memory.load<std::uint32_t>(output)==0,"Original goal query did not detect the swept crossing");
            require(memory.load<std::uint32_t>(data)==0x20000002,"Goal query overwrote its receiver");
            if(primary) require(memory.load<std::uint32_t>(output+4)==0 && memory.load<std::uint32_t>(output+8)==0xFFFFFF42,"Primary goal query lost its group or byte-sized type output");
            store(data+24,-2);memory.store<std::uint32_t>(output,UINT32_MAX);
            cpu=invoke();
            require(cpu.registers[eax]==0 && memory.load<std::uint32_t>(output)==UINT32_MAX,"Goal query reported a crossing on one side of the plane");
        }
    });
    check("original animation rule clock and end queries at 30..1000 Hz",0x00097D20,[&] {
        struct Restore {FrameStep saved=frame_step;~Restore(){frame_step=saved;}} restore;
        for(const unsigned rate:{30,60,120,144,165,240,360,1000}) {
            const auto scene_rate=std::max(60U,rate);
            std::memset(memory.access(data,32),0,32);
            memory.store<std::uint16_t>(data+4,1000);
            memory.store<std::uint32_t>(data+8,std::bit_cast<std::uint32_t>(1.0f));
            for(unsigned scene=1;scene<=scene_rate*2;++scene) {
                const auto ticks=scene*60/scene_rate;
                advance_frame_step(60.0f/scene_rate,float(scene*60%scene_rate)/scene_rate,ticks!=(scene-1)*60/scene_rate);
                run(0x97D20,{std::bit_cast<std::uint32_t>(1.0f)},true,data,esi);
                require(std::bit_cast<float>(memory.load<std::uint32_t>(data+12))==float(ticks),"Animation playhead advanced outside its rule tick");
            }
            require(memory.load<std::uint32_t>(data+8)==std::bit_cast<std::uint32_t>(1.0f),"Animation stepping changed its playback speed");
        }
        const auto actor=data+0x100,model=data+0x400;
        memory.store<std::uint32_t>(actor+0xC,model);
        memory.store<std::uint32_t>(actor+0x60,std::bit_cast<std::uint32_t>(-1.0f));
        memory.store<std::uint32_t>(model+0x28,std::bit_cast<std::uint32_t>(3.0f));
        memory.store<std::uint32_t>(model+0x2C,std::bit_cast<std::uint32_t>(2.5f));
        memory.store<std::uint32_t>(model+0x48,std::bit_cast<std::uint32_t>(1.0f));
        memory.store<std::uint32_t>(model+0x4C,std::bit_cast<std::uint32_t>(2.25f));
        memory.store<std::uint32_t>(model+0x50,std::bit_cast<std::uint32_t>(3.0f));
        for(const bool tick:{false,true}) {
            advance_frame_step(0.5f,tick?0.0f:0.5f,tick);
            require(run(0x9AEA0,{actor},true).registers[eax]==tick,"Clip completion escaped its rule tick");
            require(run(0x9AED0,{actor},true).registers[eax]==tick,"Blend completion escaped its rule tick");
        }
    });
    check("continuous response preserves equal elapsed-time damping",0x000B3E21,[&] {
        struct Restore {FrameStep saved=frame_step;~Restore(){frame_step=saved;}} restore;
        for(const unsigned rate:{30,60,120,144,165,240,360,1000}) {
            frame_step.delta=60.0f/rate;
            FloatingState floating;
            const auto gain=frame_gain_double(floating,0.02);
            double value=0;
            for(unsigned frame=0;frame<rate*2;++frame) value+=(1-value)*gain;
            require(std::abs(value-(1-std::pow(0.98,120)))<0.000001,"Damping depends on refresh rate");
        }
    });
    check("replay ball angles follow the shorter arc between recorded frames",0x0008CAF0,[&] {
        // A small original-layout recording: header +26 ring size, +34 write
        // index, +36 valid frames, +38 32-byte records. Sample time t reads
        // record 6-t; its z angle (+10) rises 1500 units per frame and wraps
        // between t=1 and t=2.
        struct Restore {FrameStep saved=frame_step;~Restore(){frame_step=saved;}} restore;
        constexpr std::uint32_t header=data+0x3000,records=data+0x3100,ball=0x738DF0,output=data+0x3400;
        const auto prepare=[&](float clock) {
            std::memset(memory.access(header,0x40),0,0x40);
            memory.store<std::uint16_t>(header+0x26,8);memory.store<std::uint16_t>(header+0x34,7);memory.store<std::uint16_t>(header+0x36,7);
            memory.store<std::uint32_t>(header+0x38,records);
            for(int k=0;k<8;++k) {
                const auto record=records+32*static_cast<std::uint32_t>(k);
                std::memset(memory.access(record,32),0,32);
                memory.store<std::uint16_t>(record+0x10,static_cast<std::uint16_t>(30000+1500*(6-k)));
                memory.store<std::uint16_t>(record+0x14,0x7FFF);
            }
            memory.store<std::uint32_t>(0x8AFA80,0);memory.store<std::uint32_t>(0x8AFA98,header);memory.store<std::uint32_t>(0x8AFA94,0);
            memory.store<std::uint32_t>(0x8AFA90,std::bit_cast<std::uint32_t>(clock));memory.store<std::uint32_t>(0x739BB4,ball);
            memory.store<std::uint8_t>(ball+3,10);memory.store<std::uint8_t>(ball+0x2E,0);
            for(const std::uint32_t field:{0x98U,0x9CU,0x128U}) memory.store<std::uint32_t>(ball+field,0);
        };
        const auto angle=[&] {return static_cast<int>(std::bit_cast<std::int16_t>(memory.load<std::uint16_t>(ball+0x2C)));};
        const auto wrapped=[](int difference) {return difference>32768?difference-65536:difference<-32768?difference+65536:difference;};
        for(const unsigned rate:{60U,144U,360U}) {
            prepare(5);frame_step={};reset_frame_history();
            const auto delta=60.0f/rate;double elapsed=0;int previous=0;bool first=true;
            for(unsigned scene=0;scene<5*rate/60;++scene) {
                const auto before=std::floor(elapsed+1e-9);elapsed+=delta;auto whole=std::floor(elapsed+1e-9);
                if(std::abs(elapsed-whole)<1e-9) elapsed=whole;
                advance_frame_step(delta,static_cast<float>(std::max(0.0,elapsed-whole)),whole>before);
                run(0x8CAF0,{ball},true);
                const auto z=angle();
                if(!first) require(std::abs(wrapped(z-previous)+1500*delta)<=2,"Replay ball angle left the recorded arc between frames");
                previous=z;first=false;
            }
            require(angle()==30000,"Replay ball did not finish on the recorded frame");
        }
        // Replay camera planners call the sampler directly at fractional times
        // at 60 Hz; they keep the original (unwrapped) interpolation.
        prepare(0);frame_step={};reset_frame_history();
        run(0x27A2E0,{0,std::bit_cast<std::uint32_t>(1.5f),output},true);
        require(std::bit_cast<std::int16_t>(memory.load<std::uint16_t>(output+0x10))==-518,"Direct sampler interpolation changed");
    });
    check("original REP MOVS across RAM aliases",0x0030D820,[&] {
        memory.map(0x80000000U+data,{static_cast<std::byte*>(memory.access(data,256)),256});
        memory.map(0xF0000000U+data,{static_cast<std::byte*>(memory.access(data,256)),256});
        constexpr std::uint32_t first=0x03020100;
        for(const auto aliases:{std::array{0x80000000U,0U},std::array{0U,0x80000000U},std::array{0x80000000U,0xF0000000U}}) {
            for(unsigned i=0;i<17;++i) memory.store<std::uint32_t>(data+i*4,first+i);
            const auto destination=(data+4)|aliases[0],source=data|aliases[1];
            const auto cpu=run(0x0030D820,{destination,source,64},true);
            require(cpu.registers[eax]==destination,"Original memmove changed its returned guest address");
            for(unsigned i=0;i<17;++i) require(memory.load<std::uint32_t>(data+i*4)==first,
                "Aliased REP MOVSD lost its forward write dependencies");
        }
        for(unsigned i=0;i<17;++i) memory.store<std::uint32_t>(data+i*4,first+i);
        memory.move_elements<std::uint32_t>(data+60,0xF0000000U+data+64,16,true);
        for(unsigned i=0;i<17;++i) require(memory.load<std::uint32_t>(data+i*4)==first+16,
            "Aliased reverse REP MOVSD lost its backward write dependencies");
    });
    check("host RAM bounds and address aliases",0,[&] {
        std::array<std::byte,64> bytes{};
        std::array<std::byte,16> high{};
        Memory ranges(0x1000,bytes);
        ranges.map(0x80001000,bytes);ranges.map(0xFFFFFFF0,high);
        require(ranges.access(0x1000,64)==bytes.data() && ranges.access(0x1040,0)==bytes.data()+64,
                "Primary RAM endpoints changed");
        require(ranges.access(0x80001000,64)==bytes.data() && ranges.access(0x80001040,0)==bytes.data()+64,
                "Mapped RAM endpoints changed");
        ranges.store<std::uint32_t>(0x8000103C,0x12345678);
        require(ranges.load<std::uint32_t>(0x103C)==0x12345678,"RAM aliases no longer share their bytes");
        ranges.store<std::uint64_t>(0xFFFFFFF8,0x123456789ABCDEF0ULL);
        require(ranges.load<std::uint64_t>(0xFFFFFFF8)==0x123456789ABCDEF0ULL,"Top-of-address-space access failed");
        Memory top(0xFFFFFFF0,high);
        require(top.access(UINT32_MAX,1)==high.data()+15 &&
                !top.try_access(0,0) && !top.try_access(0,1) && !top.try_access(UINT32_MAX,2) &&
                !top.try_access(UINT32_MAX,SIZE_MAX),"Primary RAM wrapped at the address-space boundary");
        Memory empty(0x1000,std::span(bytes).first(0));
        require(empty.access(0x1000,0)==bytes.data() && !empty.try_access(0xFFF,0),
                "Empty primary RAM endpoints changed");
        const auto rejected=[&](std::uint32_t address,std::size_t size) {
            bool failed=false;
            try {(void)ranges.access(address,size);} catch(const std::runtime_error& error) {
                failed=error.what()==std::format("Guest memory access at 0x{:08X}, {} bytes, outside the mapped regions",address,size);
            }
            require(failed,"Invalid RAM access did not preserve its error");
        };
        rejected(0xFFF,1);rejected(0x1040,1);rejected(0x103F,2);rejected(0x1000,65);
        rejected(0x80000FFF,1);rejected(0x8000103F,2);rejected(UINT32_MAX,2);
        rejected(0x1000,SIZE_MAX);rejected(0x20000000,0);
        ranges.map(0x20000000,high);
        require(ranges.access(0x20000000,16)==high.data(),"Lookup retained a stale missing mapping");
        std::array<std::byte,0x3000> pages{};
        ranges.map(0x90000003,pages);
        require(ranges.access(0x90001000,0x1000)==pages.data()+0xFFD &&
                ranges.access(0x90000FFF,2)==pages.data()+0xFFC &&
                ranges.access(0x90002FFF,4)==pages.data()+0x2FFC,
                "Static page index changed partial-page or cross-page accesses");
        rejected(0x90000002,1);rejected(0x90003002,2);rejected(0x90003003,1);
        require(ranges.try_access(0x90003003,1)==nullptr,"Static page index exposed unmapped bytes");
        // A dynamic unmap in the same table must leave static aliases intact.
        ranges.map_dynamic(0x90004000,std::span(pages).first(0x1000));
        ranges.unmap_dynamic(0x90000000,0x5000);
        require(ranges.access(0x90001000,4)==pages.data()+0xFFD &&
                ranges.try_access(0x90004000,4)==nullptr,"Dynamic unmap changed a static page");
    });
    return "{\"format\":\"smbd-cpu-check-v1\",\"passed\":" + std::string(failures ? "false" : "true") +
        ",\"sha256\":" + json(image.sha256) + ",\"compiled_functions\":" + std::to_string(compiled_batch.size()) +
        ",\"cases\":" + std::to_string(cases) + ",\"failures\":" + std::to_string(failures) +
        ",\"kernel_bindings\":0,\"platform_bindings\":0,\"results\":[" + results.str() + "]}";
}
}
