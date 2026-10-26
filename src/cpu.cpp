#include "cpu.h"
#include "call_trace.h"
#include "file.h"
#include <algorithm>
#include <map>
#include <sstream>

namespace smbd {
namespace {
struct CallSample {
    std::uint64_t visits=0;
    std::array<std::uint32_t,8> registers{},stack{};
    std::array<std::array<std::uint32_t,4>,8> pointees{};
};
bool trace_calls=false;
std::uint64_t dropped_calls=0;
std::map<std::pair<std::uint32_t,std::uint32_t>,CallSample> call_samples;
void trace_call(const Cpu& cpu,const Memory& memory,std::uint32_t target) {
    const auto key=std::pair{cpu.eip,target};
    auto found=call_samples.find(key);
    if(found==call_samples.end()) {
        if(call_samples.size()>=4096) {++dropped_calls;return;}
        found=call_samples.emplace(key,CallSample{}).first;
        auto& sample=found->second;sample.registers=cpu.registers;
        // Sample ordinary RAM only; tracing must never read native device ports.
        for(unsigned reg=0;reg<8;++reg) {
            const auto address=cpu.registers[reg];
            if(address>=0x10000 && address<=0x04000000-sizeof(sample.pointees[reg]))
                std::memcpy(sample.pointees[reg].data(),memory.access(address,sizeof(sample.pointees[reg])),sizeof(sample.pointees[reg]));
        }
        const auto stack=cpu.registers[esp];
        if(stack>=0x10000 && stack<=0x04000000-sizeof(sample.stack))
            std::memcpy(sample.stack.data(),memory.access(stack,sizeof(sample.stack)),sizeof(sample.stack));
    }
    ++found->second.visits;
}
}
void enable_call_trace() {call_samples.clear();dropped_calls=0;trace_calls=true;}
std::string call_trace_report(std::string_view executable_sha256) {
    std::ostringstream out;
    out<<"{\"format\":\"smbd-call-trace-v1\",\"executable_sha256\":"<<json(executable_sha256)
        <<",\"exhaustive\":false,\"dropped_calls\":"<<dropped_calls<<",\"calls\":[";
    const auto words=[&](const auto& values) {
        out<<'[';bool first=true;
        for(auto value:values) {if(!first) out<<',';first=false;out<<json(hex32(value));}
        out<<']';
    };
    bool first=true;
    for(const auto& [key,sample]:call_samples) {
        if(!first) out<<',';first=false;
        out<<"{\"site\":"<<json(hex32(key.first))<<",\"target\":"<<json(hex32(key.second))<<",\"visits\":"<<sample.visits<<",\"registers\":";
        words(sample.registers);out<<",\"stack\":";words(sample.stack);out<<",\"pointees\":[";
        for(unsigned reg=0;reg<8;++reg) {if(reg) out<<',';words(sample.pointees[reg]);}
        out<<"]}";
    }
    return out.str()+"]}";
}
std::uint32_t read_port(Cpu& cpu,std::uint16_t port,unsigned width) {
    if(!cpu.kernel || !cpu.kernel->read_port) throw NativeBoundary(cpu.eip,std::format("unbound {}-bit input port 0x{:04X}",width,port));
    HostFloatingScope host(cpu.floating);
    return cpu.kernel->read_port(cpu.kernel->context,port,width);
}
void write_port(Cpu& cpu,std::uint16_t port,unsigned width,std::uint32_t value) {
    if(!cpu.kernel || !cpu.kernel->write_port) throw NativeBoundary(cpu.eip,std::format("unbound {}-bit output port 0x{:04X}",width,port));
    HostFloatingScope host(cpu.floating);
    cpu.kernel->write_port(cpu.kernel->context,port,width,value);
}
void invoke_native(Cpu& cpu, Memory& memory, std::uint32_t address) {
    if(trace_calls) trace_call(cpu,memory,address);
    cpu.eip = address;
    if(address>0xFFF00000U && address<0xFFF02000U && !(address&15U)) {
        const auto ordinal=(address-0xFFF00000U)/16;
        if(!cpu.kernel || !cpu.kernel->entries[ordinal]) throw NativeBoundary(address,"kernel function is unbound");
        HostFloatingScope host(cpu.floating);
        cpu.kernel->entries[ordinal](cpu.kernel->context,cpu,memory);
        return;
    }
    const auto found = std::ranges::lower_bound(compiled_batch, address, {}, &CompiledFunction::address);
    if (found == compiled_batch.end() || found->address != address)
        throw NativeBoundary(address, "function is absent from the fixed native table");
    found->run(cpu, memory);
}
}
