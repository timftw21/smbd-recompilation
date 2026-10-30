#include "pipeline.h"
#include "cpu.h"
#include "frame_step.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <functional>
#include <iomanip>
#include <sstream>

namespace smbd {
double frame_gain_double(FloatingState&,double);
// Recipes contain reset, reg NAME WORD, xmm INDEX WORD WORD WORD WORD,
// write32 ADDRESS WORD, load ADDRESS FILE, call ENTRY CHECKPOINT_LIMIT STACK [ARG...], and
// read ADDRESS BYTES. Calls execute only the verified AOT registry.
std::string probe_cpu(Xbe& image,const std::filesystem::path& capture,const std::filesystem::path& recipe) {
    if(!image.supported()) throw std::runtime_error("CPU probes require the supported executable");
    for(const auto& function:compiled_batch)
        if(function.source_sha256!=image.sha256) throw std::runtime_error("Compiled code belongs to another executable");
    File metadata(capture/"capture.json"),snapshot(capture/"guest-memory.bin"),commands(recipe);
    if(metadata.size()>4096 || commands.size()>65536 || snapshot.size()!=64*1024*1024)
        throw std::runtime_error("CPU probe capture or recipe exceeds its fixed bounds");
    const auto meta=metadata.read(0,static_cast<std::size_t>(metadata.size()));
    const std::string_view description(reinterpret_cast<const char*>(meta.data()),meta.size());
    if(description.find("\"source_sha256\":"+json(image.sha256))==description.npos)
        throw std::runtime_error("CPU snapshot belongs to another executable");
    const auto original=snapshot.read(0,static_cast<std::size_t>(snapshot.size()));
    auto storage=original;Memory memory(0,storage);
    const auto heap_begin=(image.base+image.image_size+4095U)&~4095U;
    if(heap_begin>=storage.size()) throw std::runtime_error("CPU snapshot has no heap range");
    memory.map(0x80000000U+heap_begin,std::span(storage).subspan(heap_begin));
    memory.map(0xF0000000U+heap_begin,std::span(storage).subspan(heap_begin));
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
    Cpu cpu;std::ostringstream records;std::string error,line;
    std::uint64_t budget=0;unsigned line_number=0,calls=0,read_bytes=0,loaded_bytes=0,records_count=0;
    const auto record=[&](const std::string& text) {if(records_count++) records<<',';records<<text;};
    try {
        while(std::getline(lines,line)) {
            if(++line_number>8192) throw std::runtime_error("CPU probe command limit exceeded");
            std::istringstream words(line);std::string command;words>>command;
            if(command.empty() || command.starts_with('#')) continue;
            const auto next=[&] {std::string text;if(!(words>>text)) throw std::runtime_error("Missing CPU probe operand");return number(text);};
            if(command=="reset") {std::memcpy(storage.data(),original.data(),storage.size());cpu=Cpu{};}
            else if(command=="reg") {
                std::string name;words>>name;const auto value=next();
                constexpr std::string_view names[]={"eax","ecx","edx","ebx","esp","ebp","esi","edi"};
                const auto at=std::ranges::find(names,name);
                if(at!=std::end(names)) cpu.registers[at-std::begin(names)]=value;
                else if(name=="flags") cpu.flags=value;
                else if(name=="fs_base") cpu.fs_base=value;
                else if(name=="gs_base") cpu.gs_base=value;
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
            } else if(command=="call") {
                const auto entry=next(),limit=next(),stack=next();
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
                ExecutionDiagnostic diagnostic;diagnostic.remaining=limit;cpu.diagnostic=&diagnostic;
                try {invoke_native(cpu,memory,entry);} catch(...) {cpu.diagnostic=nullptr;throw;}
                cpu.diagnostic=nullptr;
                if(cpu.eip!=sentinel) throw std::runtime_error("CPU probe routine did not return to its sentinel");
                std::string registers;
                for(auto value:cpu.registers) {if(!registers.empty()) registers+=',';registers+=json(hex32(value));}
                record(std::format("{{\"line\":{},\"entry\":{},\"checkpoints\":{},\"registers\":[{}],\"flags\":{}}}",line_number,json(hex32(entry)),diagnostic.visits,registers,json(hex32(cpu.flags))));
            } else throw std::runtime_error("Unknown CPU probe command: "+command);
            std::string extra;if(words>>extra) throw std::runtime_error("Unexpected CPU probe operand");
        }
    } catch(const std::exception& failure) {error=failure.what();cpu.diagnostic=nullptr;}
    return std::format("{{\"format\":\"smbd-cpu-probe-v1\",\"passed\":{},\"source_sha256\":{},\"snapshot_sha256\":{},\"recipe_sha256\":{},\"calls\":{},\"loaded_bytes\":{},\"line\":{},\"error\":{},\"eip\":{},\"records\":[{}]}}",
        error.empty(),json(image.sha256),json(hash(snapshot,0,snapshot.size())),json(hash(commands,0,commands.size())),calls,loaded_bytes,line_number,json(error),json(hex32(cpu.eip)),records.str());
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
        cpu=run(0x73900,{128},true);
        require(cpu.registers[eax]==0 && cpu.registers[ecx]==80 && cpu.registers[edx]==data+0x1050,"Sprite search exceeded its pool or matched an inactive entry");
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
            run(0x7B320,{data,0},true);
            const auto world=std::bit_cast<float>(memory.load<std::uint32_t>(data+0x1C));
            require(std::isfinite(world) && std::abs(world)<0.00001f,"Platform transform changed world speed");
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
    check("original animation clock at 30..1000 Hz",0x00097D20,[&] {
        struct Restore {FrameStep saved=frame_step;~Restore(){frame_step=saved;}} restore;
        for(const unsigned rate:{30,60,120,144,165,240,360,1000}) {
            std::memset(memory.access(data,32),0,32);
            memory.store<std::uint16_t>(data+4,1000);
            memory.store<std::uint32_t>(data+8,std::bit_cast<std::uint32_t>(1.0f));
            frame_step.delta=60.0f/rate;
            for(unsigned frame=0;frame<rate*2;++frame) run(0x97D20,{std::bit_cast<std::uint32_t>(1.0f)},true,data,esi);
            const auto time=std::bit_cast<float>(memory.load<std::uint32_t>(data+12));
            require(std::isfinite(time) && std::abs(time-120)<0.01f,"Animation time depends on refresh rate");
            require(memory.load<std::uint32_t>(data+8)==std::bit_cast<std::uint32_t>(1.0f),"Animation stepping changed its playback speed");
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
    });
    return "{\"format\":\"smbd-cpu-check-v1\",\"passed\":" + std::string(failures ? "false" : "true") +
        ",\"sha256\":" + json(image.sha256) + ",\"compiled_functions\":" + std::to_string(compiled_batch.size()) +
        ",\"cases\":" + std::to_string(cases) + ",\"failures\":" + std::to_string(failures) +
        ",\"kernel_bindings\":0,\"platform_bindings\":0,\"results\":[" + results.str() + "]}";
}
}
