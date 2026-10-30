#include "cpu.h"
#include "call_trace.h"
#include "file.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <sstream>

namespace smbd {
[[noreturn,gnu::noinline,gnu::cold]] void Memory::range_error(std::uint32_t address,std::size_t size) {
    throw std::runtime_error(std::format("Guest memory access at 0x{:08X}, {} bytes, outside the mapped regions",address,size));
}
std::byte* Memory::mapped_pointer(std::uint32_t address,std::size_t size) const noexcept {
    for(unsigned i=0;i<region_count_;++i) {
        const auto& region=regions_[i];
        const auto offset=static_cast<std::uint64_t>(address)-region.base;
        if(size<=region.bytes.size() && offset<=region.bytes.size()-size) return region.bytes.data()+offset;
    }
    return nullptr;
}
double frame_gain_double(FloatingState& state,double gain) {
    if(frame_step.delta==1) return gain;
    HostFloatingScope floating(state);
    if(!std::isfinite(gain) || gain<0 || gain>1) throw std::runtime_error("Invalid continuous response coefficient");
    return gain==1?1:-std::expm1(std::log1p(-gain)*frame_step.delta);
}
__m128 frame_gain(FloatingState& state,__m128 gain) {
    if(frame_step.delta==1) return gain;
    return _mm_move_ss(gain,_mm_set_ss(static_cast<float>(frame_gain_double(state,_mm_cvtss_f32(gain)))));
}
void frame_ball_step(FloatingState& state,Memory& memory,std::uint32_t ball,std::array<std::uint32_t,3>& displacement) {
    const double dt=frame_step.delta;
    if(dt==1) return;
    HostFloatingScope floating(state);
    const auto value=[&](std::uint32_t address){return double(std::bit_cast<float>(memory.load<std::uint32_t>(address)));};
    std::array<double,3> velocity{},acceleration{},normal{};
    double normal_velocity=0,normal_acceleration=0,length=0;
    for(unsigned axis=0;axis<3;++axis) {
        velocity[axis]=std::bit_cast<float>(displacement[axis]);
        acceleration[axis]=dt?(value(ball+0x1C+axis*4)-velocity[axis])/dt:0;
        normal[axis]=value(ball+0x11C+axis*4);length+=normal[axis]*normal[axis];
        normal_velocity+=velocity[axis]*normal[axis];normal_acceleration+=acceleration[axis]*normal[axis];
    }
    double position_velocity=dt,position_force=dt*(dt+1)*0.5,velocity_force=dt;
    const bool grounded=(memory.load<std::uint32_t>(ball+0x128)&1) && length>0 && std::isfinite(length);
    if(grounded) {
        const auto loss=value(memory.load<std::uint32_t>(0x6C7EA8)==60?0x347BC8:0x3FAE78);
        // Fractional iterate of the original contact step:
        // p += v+a; v = (v+a)*(1-loss). This also preserves acceleration's
        // interaction with drag, rather than scaling the two independently.
        const auto gain=-std::expm1(std::log1p(-loss)*dt);
        position_velocity=gain/loss;
        position_force=(dt-(1-loss)*position_velocity)/loss;
        velocity_force=(1-loss)*position_velocity/(1-gain);
    }
    for(unsigned axis=0;axis<3;++axis) {
        const auto vn=grounded?normal[axis]*normal_velocity/length:velocity[axis];
        const auto an=grounded?normal[axis]*normal_acceleration/length:acceleration[axis];
        const auto vt=velocity[axis]-vn,at=acceleration[axis]-an;
        const auto distance=vn*dt+an*(dt*(dt+1)*0.5)+vt*position_velocity+at*position_force;
        displacement[axis]=std::bit_cast<std::uint32_t>(static_cast<float>(distance));
        if(grounded) memory.store<std::uint32_t>(ball+0x1C+axis*4,std::bit_cast<std::uint32_t>(static_cast<float>(velocity[axis]+an*dt+at*velocity_force)));
    }
}
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
