#include "cpu.h"
#include "call_trace.h"
#include "interface_marks.h"
#include "file.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <sstream>
#include <unordered_map>

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
    if(size && size<=page_size-(address&(page_size-1))) return nullptr;
    auto found=dynamic_regions_.upper_bound(address);
    if(found!=dynamic_regions_.begin()) {
        --found;const auto offset=std::uint64_t(address)-found->first;
        if(size<=found->second.size() && offset<=found->second.size()-size) return found->second.data()+offset;
    }
    return nullptr;
}
void Memory::map(std::uint32_t base,std::span<std::byte> bytes) {
    const auto end=std::uint64_t(base)+bytes.size();
    if(bytes.empty() || end>0x100000000ULL || region_count_==regions_.size())
        throw std::runtime_error("Invalid or exhausted guest memory mappings");
    const auto overlaps=[&](std::uint32_t other,std::size_t size) {
        return base<std::uint64_t(other)+size && other<end;
    };
    if(overlaps(base_,bytes_.size())) throw std::runtime_error("Guest mappings overlap");
    for(unsigned i=0;i<region_count_;++i) if(overlaps(regions_[i].base,regions_[i].bytes.size()))
        throw std::runtime_error("Guest mappings overlap");
    const auto next=dynamic_regions_.lower_bound(base);
    if((next!=dynamic_regions_.end() && next->first<end) ||
        (next!=dynamic_regions_.begin() && overlaps(std::prev(next)->first,std::prev(next)->second.size())))
        throw std::runtime_error("Guest mappings overlap");
    // Index only complete pages: a partial mapping must never make its
    // unmapped neighbours accessible through the fast page lookup.
    const auto first_page=(std::uint64_t(base)+page_size-1)>>page_bits,limit=end>>page_bits;
    if(first_page<limit) {
        for(auto index=first_page>>table_bits;index<=((limit-1)>>table_bits);++index) {
            auto& table=pages_[index];
            if(!table) table=std::make_unique<PageTable>();
        }
        for(auto page=first_page;page<limit;++page) {
            auto& table=*pages_[page>>table_bits];
            table[page&(table.size()-1)]=bytes.data()+((page<<page_bits)-base);
        }
    }
    regions_[region_count_++]={base,bytes};
}
void Memory::map_dynamic(std::uint32_t base,std::span<std::byte> bytes) {
    const auto end=std::uint64_t(base)+bytes.size();
    if(bytes.empty() || (base&(page_size-1)) || (bytes.size()&(page_size-1)) ||
        end>0x100000000ULL || dynamic_regions_.size()>=32768)
        throw std::runtime_error("Invalid or exhausted dynamic guest mappings");
    const auto overlaps=[&](std::uint32_t other,std::size_t size) {return base<std::uint64_t(other)+size && other<end;};
    if(overlaps(base_,bytes_.size())) throw std::runtime_error("Dynamic guest mapping overlaps RAM");
    for(unsigned i=0;i<region_count_;++i) if(overlaps(regions_[i].base,regions_[i].bytes.size()))
        throw std::runtime_error("Dynamic guest mapping overlaps a static mapping");
    auto next=dynamic_regions_.lower_bound(base);
    if(next!=dynamic_regions_.end() && next->first<end) throw std::runtime_error("Dynamic guest mappings overlap");
    const auto previous=next==dynamic_regions_.begin()?dynamic_regions_.end():std::prev(next);
    if(previous!=dynamic_regions_.end() && std::uint64_t(previous->first)+previous->second.size()>base)
        throw std::runtime_error("Dynamic guest mappings overlap");
    for(auto index=base>>(page_bits+table_bits);index<=((end-1)>>(page_bits+table_bits));++index) {
        auto& table=pages_[index];
        if(!table) table=std::make_unique<PageTable>();
    }
    auto mapped=dynamic_regions_.emplace_hint(next,base,bytes);
    if(next!=dynamic_regions_.end() && end==next->first && bytes.data()+bytes.size()==next->second.data()) {
        mapped->second={bytes.data(),bytes.size()+next->second.size()};
        dynamic_regions_.erase(next);
    }
    if(previous!=dynamic_regions_.end() && std::uint64_t(previous->first)+previous->second.size()==base &&
        previous->second.data()+previous->second.size()==bytes.data()) {
        previous->second={previous->second.data(),previous->second.size()+mapped->second.size()};
        dynamic_regions_.erase(mapped);
    }
    for(std::size_t offset=0;offset<bytes.size();offset+=page_size) {
        const auto page=(base+offset)>>page_bits;
        auto& table=*pages_[page>>table_bits];
        table[page&(table.size()-1)]=bytes.data()+offset;
    }
}
void Memory::unmap_dynamic(std::uint32_t base,std::uint32_t size) {
    const auto end=std::uint64_t(base)+size;
    if(!size) return;
    if(((base|size)&(page_size-1)) || end>0x100000000ULL)
        throw std::runtime_error("Invalid dynamic guest unmap");
    auto found=dynamic_regions_.upper_bound(base);
    if(found!=dynamic_regions_.begin()) --found;
    while(found!=dynamic_regions_.end() && found->first<end) {
        const auto start=found->first;const auto bytes=found->second;
        const auto limit=std::uint64_t(start)+bytes.size();
        if(limit<=base) {++found;continue;}
        if(limit>end) dynamic_regions_.emplace_hint(std::next(found),static_cast<std::uint32_t>(end),bytes.subspan(end-start));
        if(start<base) {
            found->second=bytes.first(base-start);
            ++found;
        } else found=dynamic_regions_.erase(found);
        for(auto page=std::max(start,base)>>page_bits;page<(std::min(limit,end)>>page_bits);++page) {
            auto& table=*pages_[page>>table_bits];
            table[page&(table.size()-1)]=nullptr;
        }
    }
}
double frame_gain_interval(FloatingState& state,double gain,float interval) {
    if(interval==1) return gain;
    HostFloatingScope floating(state);
    if(!std::isfinite(gain) || gain<0 || gain>1) throw std::runtime_error("Invalid continuous response coefficient");
    if(interval==0) return 0;
    return gain==1?1:-std::expm1(std::log1p(-gain)*interval);
}
double frame_gain_double(FloatingState& state,double gain) {
    return frame_gain_interval(state,gain,frame_step.delta);
}
__m128 frame_gain_interval(FloatingState& state,__m128 gain,float interval) {
    if(interval==1) return gain;
    return _mm_move_ss(gain,_mm_set_ss(static_cast<float>(frame_gain_interval(state,_mm_cvtss_f32(gain),interval))));
}
__m128 frame_gain(FloatingState& state,__m128 gain) {
    if(frame_step.delta==1) return gain;
    return _mm_move_ss(gain,_mm_set_ss(static_cast<float>(frame_gain_double(state,_mm_cvtss_f32(gain)))));
}
std::uint32_t frame_hud_fall_turn(FloatingState& state,std::uint32_t count,std::uint32_t turn) {
    if(frame_step.delta==1 && frame_step.phase==0 && frame_step.previous_phase==0) return turn;
    HostFloatingScope floating(state);
    // 4D41F..4D434 accelerates the fallout icon by 32 units per original
    // tick, capped at 4096. Integrate that original integer sequence, keeping
    // the same quantized boundary at both ends of adjacent scene intervals.
    const auto angle=[](double time) {
        time=std::max(time,0.0);
        const auto ramp=std::min(time,128.0);
        return static_cast<std::int64_t>(16*ramp*(ramp+1)+4096*std::max(time-128,0.0));
    };
    return static_cast<std::uint32_t>(angle(double(count)+frame_step.phase)-
        angle(double(count)-(frame_step.logic?1:0)+frame_step.previous_phase));
}
namespace {
struct WorldResponse {double value=0;std::uint16_t previous=0;bool initialized=false;unsigned kind=0;};
// A8060's four WORLD records at 73E930 have two signed 16-bit tilt angles.
thread_local std::array<WorldResponse,8> world_responses;
// ADFC0 and B7350 own five 0x28C-byte camera records.
struct CameraPose {
    std::array<std::uint32_t,6> position;
    std::array<std::uint16_t,3> angle;
};
struct CameraFrame {CameraPose rule{},sample{};std::uint32_t controller=0;bool initialized=false;};
thread_local std::array<CameraFrame,5> camera_frames;
using RankingCameraPose = std::array<float,6>;
struct RankingCameraStep {
    std::uint32_t camera=0;
    std::uint64_t rule=0;
    RankingCameraPose before{},after{};
};
thread_local RankingCameraStep ranking_camera;
struct CameraFov {std::uint16_t rule=0,sample=0;bool initialized=false;};
thread_local std::array<CameraFov,5> camera_fovs;
struct CameraShakeFrame {
    std::array<std::uint32_t,3> output{},sample{};
    std::uint64_t serial=0;
    bool initialized=false,drawn=false;
};
thread_local std::array<CameraShakeFrame,5> camera_shakes;
struct ClipStep {
    std::uint64_t rule;
    std::uint32_t after, bounds;
    float before, increment;
};
thread_local std::unordered_map<std::uint32_t,ClipStep> clip_steps;
thread_local std::uint64_t clip_pruned_rule=0;
// 2454B0 walks 0x9C-byte text boxes. 6BB50 passes the inner record to drawing.
constexpr std::uint32_t text_box_size=0x9C, text_box_inner=0x10;
constexpr std::uint32_t text_pose_fields[]={0x20,0x24,0x28,0x40,0x44,0x48,0x4C,0x50};
struct TextPose {
    std::array<std::uint32_t,std::size(text_pose_fields)> position;
    std::uint16_t roll;
    bool operator==(const TextPose&) const=default;
};
struct TextPoseSample {
    std::uint32_t box,descriptor;
    TextPose committed,draw;
};
thread_local std::vector<TextPoseSample> text_poses;
thread_local std::uint64_t text_pose_tick=0;
thread_local float text_pose_phase=0;

TextPose read_text_pose(Memory& memory,std::uint32_t box) {
    TextPose pose;
    for(unsigned i=0;i<std::size(text_pose_fields);++i)
        pose.position[i]=memory.load<std::uint32_t>(box+text_pose_fields[i]);
    pose.roll=memory.load<std::uint16_t>(box+0x60);
    return pose;
}
bool sample_text_pose() {
    return frame_step.phase>0 && frame_step.delta==frame_step.interval;
}
WorldResponse& world_response(std::uint32_t address) {
    if(address<0x73E930 || address>=0x73EA20 || (address-0x73E930)%0x3C>2 || (address&1))
        throw std::runtime_error("World response is outside the original tilt array");
    return world_responses[(address-0x73E930)/0x3C*2+((address-0x73E930)%0x3C)/2];
}
CameraFrame& camera_frame(std::uint32_t camera) {
    const auto offset=camera-0x73F1A0U;
    if(offset>=5*0x28C || offset%0x28C)
        throw std::runtime_error("Camera is outside the original camera array");
    return camera_frames[offset/0x28C];
}
CameraFov& camera_fov(std::uint32_t camera) {
    const auto offset=camera-0x73F1A0U;
    if(offset>=5*0x28C || offset%0x28C)
        throw std::runtime_error("FOV is outside the original camera array");
    return camera_fovs[offset/0x28C];
}
unsigned camera_shake_index(std::uint32_t camera) {
    const auto offset=camera-0x73F1A0U;
    if(offset>=5*0x28C || offset%0x28C)
        throw std::runtime_error("Shake is outside the original camera array");
    return offset/0x28C;
}
CameraPose read_camera_pose(Memory& memory,std::uint32_t camera) {
    CameraPose pose;
    for(unsigned i=0;i<pose.position.size();++i) pose.position[i]=memory.load<std::uint32_t>(camera+i*4);
    for(unsigned i=0;i<pose.angle.size();++i) pose.angle[i]=memory.load<std::uint16_t>(camera+0x18+i*2);
    return pose;
}
void write_camera_pose(Memory& memory,std::uint32_t camera,const CameraPose& pose) {
    for(unsigned i=0;i<pose.position.size();++i) memory.store<std::uint32_t>(camera+i*4,pose.position[i]);
    for(unsigned i=0;i<pose.angle.size();++i) memory.store<std::uint16_t>(camera+0x18+i*2,pose.angle[i]);
}
bool ranking_camera_active(Memory& memory) {
    return memory.load<std::uint32_t>(0x6C9364)==4 && memory.load<std::uint8_t>(0x8A6764);
}
bool ranking_camera_moves(Memory& memory) {
    const auto remaining=std::bit_cast<std::int32_t>(memory.load<std::uint32_t>(0x8A5FFC));
    if(remaining<=0) return false;
    const auto next=remaining-1;
    const auto ticks=std::int64_t(memory.load<std::uint32_t>(0x6C7EA8));
    // 2755F0 decrements the timer before its three linear camera calls.
    switch(memory.load<std::uint8_t>(0x8A6000)) {
    case 0xA: return next>ticks*3/2 || next<ticks;
    case 0xC: return next<=ticks*5/2 && next>ticks*2;
    case 0xD: return true;
    default: return false;
    }
}
}
void frame_reset_world() {world_responses={};}
void frame_reset_camera() {camera_frames={};camera_fovs={};camera_shakes={};ranking_camera={};}
void frame_reset_camera(std::uint32_t camera) {
    const auto offset=camera-0x73F1A0U;
    if(offset<5*0x28C && offset%0x28C==0) {
        camera_frames[offset/0x28C]={};camera_fovs[offset/0x28C]={};camera_shakes[offset/0x28C]={};
    }
    if(ranking_camera.camera==camera) ranking_camera={};
}
void reset_frame_history() {frame_reset_world();frame_reset_camera();clip_steps.clear();text_poses.clear();}

void frame_ranking_camera_step(Cpu& cpu,Memory& memory,void (*update)(Cpu&,Memory&)) {
    if(!ranking_camera_active(memory)) {update(cpu,memory);return;}
    const auto camera=memory.load<std::uint32_t>(0x73F19C);
    auto* pose=memory.access(camera,sizeof(RankingCameraPose));
    ranking_camera.camera=camera;
    ranking_camera.rule=frame_rule_serial;
    std::memcpy(ranking_camera.before.data(),pose,sizeof(RankingCameraPose));
    update(cpu,memory);
    std::memcpy(ranking_camera.after.data(),pose,sizeof(RankingCameraPose));
}

void frame_ranking_camera_view(Cpu& cpu,Memory& memory,void (*update)(Cpu&,Memory&)) {
    if(frame_step.phase==0 || !ranking_camera_active(memory) || (memory.load<std::uint8_t>(0x892114)&0xA) ||
       (memory.load<std::uint32_t>(0x892118)&0x100)) {update(cpu,memory);return;}
    const auto camera=memory.load<std::uint32_t>(0x73F19C);
    if(memory.load<std::uint8_t>(camera+0x1E)!=3) {update(cpu,memory);return;}
    const bool next=frame_step.logic && ranking_camera_moves(memory);
    const bool previous=!frame_step.logic && ranking_camera.camera==camera && ranking_camera.rule==frame_rule_serial;
    if(!next && !previous) {update(cpu,memory);return;}

    auto* pose=memory.access(camera,sizeof(RankingCameraPose));
    RankingCameraPose committed;
    std::memcpy(committed.data(),pose,sizeof(committed));
    if(previous && std::memcmp(committed.data(),ranking_camera.after.data(),sizeof(committed))) {update(cpu,memory);return;}
    // CAMERA builds the matrices before SPRITE commits the next rule step.
    // Sample that linear segment without changing the position used by rules.
    RankingCameraPose sample;
    {
        HostFloatingScope floating(cpu.floating);
        constexpr std::uint32_t velocity[]={0x128,0x12C,0x130,0x140,0x144,0x148};
        for(unsigned i=0;i<sample.size();++i) {
            const auto start=next?committed[i]:ranking_camera.before[i];
            const auto end=next?start+std::bit_cast<float>(memory.load<std::uint32_t>(camera+velocity[i])):committed[i];
            sample[i]=std::lerp(start,end,frame_step.phase);
        }
    }
    struct RestorePose {
        void* destination;
        const RankingCameraPose& position;
        ~RestorePose() {std::memcpy(destination,position.data(),sizeof(position));}
    } restore{pose,committed};
    std::memcpy(pose,sample.data(),sizeof(sample));
    update(cpu,memory);
}

void frame_reset_camera_shake(std::uint32_t camera) {
    const auto offset=camera-0x73F1A0U;
    if(offset<5*0x28C && offset%0x28C==0) camera_shakes[offset/0x28C]={};
}

void frame_camera_shake_update(Cpu& cpu,Memory& memory,
    void (*update)(Cpu&,Memory&,bool,bool,std::uint32_t)) {
    const auto stack=cpu.registers[esp],camera=memory.load<std::uint32_t>(stack+4);
    const auto index=camera_shake_index(camera);
    auto frame=camera_shakes[index];
    const bool active=memory.load<std::uint32_t>(camera+0xF4)!=0;
    if(!active && !frame.initialized) {update(cpu,memory,true,true,0);return;}
    const bool paused=(memory.load<std::uint8_t>(0x892114)&0xA)!=0;
    const auto read_motion=[&] {
        std::array<std::uint32_t,6> motion;
        for(unsigned i=0;i<motion.size();++i) motion[i]=memory.load<std::uint32_t>(camera+0xFC+i*4);
        return motion;
    };
    const auto before=read_motion();
    if(!frame.initialized) {
        std::copy_n(before.begin(),3,frame.output.begin());frame.sample=frame.output;
        frame.serial=frame_rule_serial;frame.initialized=frame.drawn=active;
    }
    // B74C0 rebuilds both bases each scene. Sample only the shake offset;
    // neither the base camera nor the committed spring motion is interpolated.
    std::array<std::byte,96> bases;
    std::memcpy(bases.data(),memory.access(camera+0x14C,48),48);
    std::memcpy(bases.data()+48,memory.access(camera+0x1AC,48),48);
    const auto preview_stack=stack-0x320,shadow=preview_stack+0x10;
    const auto spring=[&](const std::array<std::uint32_t,6>& motion) {
        std::memcpy(memory.access(shadow,0x28C),memory.access(camera,0x28C),0x28C);
        for(unsigned i=0;i<motion.size();++i) memory.store<std::uint32_t>(shadow+0xFC+i*4,motion[i]);
        memory.store<std::uint32_t>(shadow+0xF4,1);
        memory.store<std::uint32_t>(preview_stack,memory.load<std::uint32_t>(stack));
        memory.store<std::uint32_t>(preview_stack+4,shadow);
        HostFloatingScope host(cpu.floating);
        Cpu preview=cpu;preview.registers[esp]=preview_stack;
        preview.preempt=nullptr;preview.diagnostic=nullptr;
        // The original spring runs privately. Drawing and expiry/reset are
        // disabled, so camera+20C cannot redirect a preview reset to real state.
        {FloatingScope floating(preview.floating);update(preview,memory,true,false,0);}
        std::array<std::uint32_t,3> position;
        for(unsigned i=0;i<position.size();++i) position[i]=memory.load<std::uint32_t>(shadow+0xFC+i*4);
        return position;
    };
    if(frame_step.logic) {
        update(cpu,memory,true,true,0);
        if(active) {
            if(paused) std::copy_n(before.begin(),3,frame.output.begin());
            else if(memory.load<std::uint32_t>(camera+0xF4)) {
                for(unsigned i=0;i<frame.output.size();++i) frame.output[i]=memory.load<std::uint32_t>(camera+0xFC+i*4);
            } else frame.output=spring(before);
        } else frame.output={};
        frame.drawn=active;frame.serial=frame_rule_serial;frame.initialized=true;
    } else if(!active && frame.serial!=frame_rule_serial) {
        frame.output={};frame.drawn=false;
    }
    const bool remaining=memory.load<std::uint32_t>(camera+0xF4)!=0;
    if(!(paused && remaining)) {
        if(frame_step.phase==0) frame.sample=frame.output;
        else {
            const auto next=remaining?spring(read_motion()):std::array<std::uint32_t,3>{};
            HostFloatingScope host(cpu.floating);
            for(unsigned i=0;i<frame.sample.size();++i)
                frame.sample[i]=std::bit_cast<std::uint32_t>(std::lerp(std::bit_cast<float>(frame.output[i]),
                    std::bit_cast<float>(next[i]),frame_step.phase));
        }
    }
    bool original_draw=frame_step.logic && frame_step.phase==0;
    if(original_draw && paused && remaining)
        for(unsigned i=0;i<frame.sample.size();++i) original_draw&=frame.sample[i]==before[i];
    if(!original_draw) {
        std::memcpy(memory.access(camera+0x14C,48),bases.data(),48);
        std::memcpy(memory.access(camera+0x1AC,48),bases.data()+48,48);
        if(frame.drawn || remaining) {
            for(unsigned i=0;i<frame.sample.size();++i) memory.store<std::uint32_t>(shadow+i*4,frame.sample[i]);
            memory.store<std::uint32_t>(preview_stack,memory.load<std::uint32_t>(stack));
            memory.store<std::uint32_t>(preview_stack+4,camera);
            HostFloatingScope host(cpu.floating);
            Cpu draw=cpu;draw.registers[esp]=preview_stack;draw.preempt=nullptr;draw.diagnostic=nullptr;
            {FloatingScope floating(draw.floating);update(draw,memory,false,true,shadow);}
        }
    }
    if(!frame_step.logic) {cpu.eip=memory.load<std::uint32_t>(stack);cpu.registers[esp]=stack+4;}
    camera_shakes[index]=!remaining && !frame.drawn?CameraShakeFrame{}:frame;
}

void frame_camera_fov_begin(Memory& memory,std::uint32_t camera) {
    if(memory.load<std::uint8_t>(0x892114)&0xA) return;
    auto& frame=camera_fov(camera);
    const auto current=memory.load<std::uint16_t>(camera+0x30);
    if(!frame.initialized || current!=frame.sample) {
        frame.rule=current;frame.initialized=true;
    }
    memory.store<std::uint16_t>(camera+0x30,frame.rule);
}
std::uint32_t frame_camera_fov_max(Memory& memory,std::uint32_t camera) {
    auto& frame=camera_fov(camera);
    if(frame_step.logic) frame.rule=0x5555;
    frame.sample=0x5555;
    return frame.sample;
}
std::uint32_t frame_camera_fov_response(FloatingState& floating,Memory& memory,std::uint32_t camera,
    std::uint32_t difference,std::uint32_t increment) {
    auto& frame=camera_fov(camera);
    const auto current=memory.load<std::uint16_t>(camera+0x30);
    const auto original=static_cast<std::uint16_t>(current+increment);
    const auto target=std::bit_cast<std::int32_t>(difference+
        static_cast<std::uint32_t>(std::bit_cast<std::int16_t>(current)));
    if(frame_step.logic) {
        frame.rule=original;
        // The original ADD exposes all 32 bits before storing the low word.
        if(frame_step.phase==0) {frame.sample=original;return increment;}
    }
    const auto next_difference=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(target)-
        static_cast<std::uint32_t>(std::bit_cast<std::int16_t>(frame.rule)));
    const auto next=frame_step.logic?static_cast<std::uint16_t>(frame.rule+
        (next_difference>>5)):original;
    // Retain the original per-rule SAR rounding. Only this scene's displayed
    // angle samples the prospective original map from the current geometry.
    HostFloatingScope host(floating);
    const auto from=std::bit_cast<std::int16_t>(frame.rule);
    frame.sample=static_cast<std::uint16_t>(static_cast<std::int32_t>(from+
        (std::bit_cast<std::int16_t>(next)-from)*double(frame_step.phase)));
    return std::uint32_t(frame.sample)-current;
}

void frame_camera_update(Cpu& cpu,Memory& memory,void (*update)(Cpu&,Memory&),bool goal) {
    if(memory.load<std::uint8_t>(0x892114)&0xA) {update(cpu,memory);return;}
    const auto stack=cpu.registers[esp],camera=memory.load<std::uint32_t>(stack+4);
    auto& frame=camera_frame(camera);
    const auto controller=goal?0xB00D0U:0xB3BA0U;
    if(frame.controller!=controller) {frame={};frame.controller=controller;}
    if(frame_step.logic && frame_step.delta==1 && frame_step.phase==0 && !frame.initialized) {update(cpu,memory);return;}
    const auto current=read_camera_pose(memory,camera);
    if(!frame.initialized) {frame.rule=current;frame.initialized=true;}
    else {
        for(unsigned i=0;i<current.position.size();++i)
            if(current.position[i]!=frame.sample.position[i]) frame.rule.position[i]=current.position[i];
        for(unsigned i=0;i<current.angle.size();++i)
            if(current.angle[i]!=frame.sample.angle[i]) frame.rule.angle[i]=current.angle[i];
    }
    write_camera_pose(memory,camera,frame.rule);
    struct StepRestore {FrameStep saved=frame_step;~StepRestore(){frame_step=saved;}} step;
    frame_step.delta=1;
    if(frame_step.logic) {update(cpu,memory);frame.rule=read_camera_pose(memory,camera);}
    frame.sample=frame.rule;
    if(frame_step.phase==0) {
        if(!frame_step.logic) {cpu.eip=memory.load<std::uint32_t>(stack);cpu.registers[esp]=stack+4;}
        return;
    }

    // B3BA0 and B00D0 have no draw/service calls. Their next original camera
    // rule executes on a private record; response history stays committed.
    const auto preview_stack=stack-0x460,shadow=preview_stack+0x10,ball_shadow=preview_stack+0x2A0;
    const auto ball=memory.load<std::uint32_t>(stack+8);
    std::memcpy(memory.access(shadow,0x28C),memory.access(camera,0x28C),0x28C);
    std::memcpy(memory.access(ball_shadow,0x1B0),memory.access(ball,0x1B0),0x1B0);
    memory.store<std::uint32_t>(preview_stack,memory.load<std::uint32_t>(stack));
    memory.store<std::uint32_t>(preview_stack+4,shadow);
    memory.store<std::uint32_t>(preview_stack+8,ball_shadow);
    struct SharedRestore {
        Memory& memory;
        std::array<std::byte,48> matrix;
        std::array<std::byte,10> counters;
        ~SharedRestore() {
            std::memcpy(memory.access(0x6DD5A0,matrix.size()),matrix.data(),matrix.size());
            std::memcpy(memory.access(0x73FE60,counters.size()),counters.data(),counters.size());
        }
    } shared{memory,{},{}};
    std::memcpy(shared.matrix.data(),memory.access(0x6DD5A0,shared.matrix.size()),shared.matrix.size());
    std::memcpy(shared.counters.data(),memory.access(0x73FE60,shared.counters.size()),shared.counters.size());
    HostFloatingScope host(cpu.floating);
    if(memory.load<std::uint8_t>(ball) && frame_continuous_ball(memory.load<std::uint8_t>(ball+3))) {
        // This one-tick forecast belongs only to drawing. Actual camera rules
        // always consume the original current ball argument above.
        for(unsigned axis=0;axis<3;++axis) {
            const auto position=std::bit_cast<float>(memory.load<std::uint32_t>(ball+4+axis*4));
            const auto velocity=std::bit_cast<float>(memory.load<std::uint32_t>(ball+0x1C+axis*4));
            memory.store<std::uint32_t>(ball_shadow+4+axis*4,std::bit_cast<std::uint32_t>(
                position+velocity*(1-frame_step.phase)));
        }
    }
    Cpu preview=cpu;
    preview.registers[esp]=preview_stack;preview.preempt=nullptr;preview.diagnostic=nullptr;
    frame_step.logic=true;
    {FloatingScope floating(preview.floating);update(preview,memory);}
    const auto next=read_camera_pose(memory,shadow);
    if(goal) {
        // The goal camera owns a lagged target as well as an orbit response.
        // Sample its complete prospective rule, rather than replacing that
        // target with the ordinary rolling camera's direct ball attachment.
        for(unsigned i=0;i<frame.sample.position.size();++i)
            frame.sample.position[i]=std::bit_cast<std::uint32_t>(std::lerp(
                std::bit_cast<float>(frame.rule.position[i]),std::bit_cast<float>(next.position[i]),frame_step.phase));
    } else {
        std::array<float,3> target;
        target[0]=std::bit_cast<float>(memory.load<std::uint32_t>(ball+4));
        target[2]=std::bit_cast<float>(memory.load<std::uint32_t>(ball+0xC));
        const auto height=memory.load<std::uint16_t>(0x730110)==0x15A?0.0f:
            std::bit_cast<float>(memory.load<std::uint32_t>(0x402438));
        target[1]=(height+std::bit_cast<float>(memory.load<std::uint32_t>(0x378DC8)))+
            std::bit_cast<float>(memory.load<std::uint32_t>(ball+8));
        for(unsigned axis=0;axis<3;++axis) {
            const auto from=std::bit_cast<float>(frame.rule.position[axis])-
                std::bit_cast<float>(frame.rule.position[axis+3]);
            const auto to=std::bit_cast<float>(next.position[axis])-
                std::bit_cast<float>(next.position[axis+3]);
            frame.sample.position[axis]=std::bit_cast<std::uint32_t>(target[axis]+std::lerp(from,to,frame_step.phase));
            frame.sample.position[axis+3]=std::bit_cast<std::uint32_t>(target[axis]);
        }
    }
    for(unsigned i=0;i<frame.sample.angle.size();++i) {
        const auto from=std::bit_cast<std::int16_t>(frame.rule.angle[i]);
        const auto turn=static_cast<std::int16_t>(next.angle[i]-frame.rule.angle[i]);
        frame.sample.angle[i]=static_cast<std::uint16_t>(static_cast<std::int32_t>(from+turn*double(frame_step.phase)));
    }
    write_camera_pose(memory,camera,frame.sample);
    if(!step.saved.logic) {cpu.eip=memory.load<std::uint32_t>(stack);cpu.registers[esp]=stack+4;}
}

void frame_text_box_update(Cpu& cpu,Memory& memory,void (*update)(Cpu&,Memory&,bool,bool)) {
    const bool fractional=sample_text_pose();
    if(!fractional || (memory.load<std::uint8_t>(0x892114)&0xA)) {
        update(cpu,memory,true,frame_step.logic);
        return;
    }
    const auto stack=cpu.registers[esp];
    const auto box=memory.load<std::uint32_t>(stack+4);
    update(cpu,memory,true,frame_step.logic);
    const auto descriptor=memory.load<std::uint32_t>(box+0x94);
    if(!descriptor) return;

    // Evaluate the original next placement on a private guest-stack record.
    // The real box's spring always compares two consecutive rule poses.
    const auto preview_stack=stack-0xB0;
    const auto shadow=preview_stack+0x10;
    std::memcpy(memory.access(shadow,text_box_size),memory.access(box,text_box_size),text_box_size);
    memory.store<std::uint32_t>(preview_stack,cpu.eip);
    memory.store<std::uint32_t>(preview_stack+4,shadow);
    struct MatrixRestore {
        void* address;
        std::array<std::byte,48> saved;
        ~MatrixRestore() {std::memcpy(address,saved.data(),saved.size());}
    } matrix{memory.access(0x6DD5A0,48),{}};
    std::memcpy(matrix.saved.data(),matrix.address,matrix.saved.size());
    HostFloatingScope host(cpu.floating);
    Cpu preview=cpu;
    preview.registers[esp]=preview_stack;
    preview.preempt=nullptr;
    preview.diagnostic=nullptr;
    {
        FloatingScope floating(preview.floating);
        update(preview,memory,false,true);
    }
    const auto from=read_text_pose(memory,box),to=read_text_pose(memory,shadow);
    TextPose draw=from;
    for(unsigned i=0;i<std::size(text_pose_fields);++i) {
        const auto start=std::bit_cast<float>(from.position[i]),end=std::bit_cast<float>(to.position[i]);
        draw.position[i]=std::bit_cast<std::uint32_t>(std::lerp(start,end,frame_step.phase));
    }
    const auto turn=static_cast<std::int16_t>(to.roll-from.roll);
    draw.roll=static_cast<std::uint16_t>(from.roll+static_cast<int>(turn*frame_step.phase));
    if(text_pose_tick!=frame_rule_serial || text_pose_phase!=frame_step.phase) {
        text_poses.clear();
        text_pose_tick=frame_rule_serial;
        text_pose_phase=frame_step.phase;
    }
    const auto entry=std::ranges::find(text_poses,box,&TextPoseSample::box);
    const TextPoseSample sample{box,descriptor,from,draw};
    if(entry==text_poses.end()) text_poses.push_back(sample);
    else *entry=sample;
}

void frame_text_box_draw(Cpu& cpu,Memory& memory,void (*draw)(Cpu&,Memory&)) {
    const auto inner=cpu.registers[esi];
    const auto box=inner-text_box_inner;
    if(!sample_text_pose() || text_pose_tick!=frame_rule_serial || text_pose_phase!=frame_step.phase ||
       (memory.load<std::uint8_t>(0x892114)&0xA)) {
        draw(cpu,memory);
        return;
    }
    const auto sample=std::ranges::find(text_poses,box,&TextPoseSample::box);
    if(sample==text_poses.end() || sample->descriptor!=memory.load<std::uint32_t>(box+0x94) ||
       sample->committed!=read_text_pose(memory,box)) {
        draw(cpu,memory);
        return;
    }
    const auto stack=cpu.registers[esp];
    const auto draw_stack=stack-0xB0,shadow=draw_stack+0x10;
    std::memcpy(memory.access(shadow,text_box_size),memory.access(box,text_box_size),text_box_size);
    for(unsigned i=0;i<std::size(text_pose_fields);++i)
        memory.store<std::uint32_t>(shadow+text_pose_fields[i],sample->draw.position[i]);
    memory.store<std::uint16_t>(shadow+0x60,sample->draw.roll);
    memory.store<std::uint32_t>(draw_stack,memory.load<std::uint32_t>(stack));
    cpu.registers[esp]=draw_stack;
    cpu.registers[esi]=shadow+text_box_inner;
    struct DrawStackRestore {
        Cpu& cpu;
        std::uint32_t inner;
        ~DrawStackRestore() {cpu.registers[esp]+=0xB0;cpu.registers[esi]=inner;}
    } restore{cpu,inner};
    draw(cpu,memory);
}

void frame_clip_reset(std::uint32_t clip) {clip_steps.erase(clip);}
void frame_clip_copy(std::uint32_t model) {
    const auto primary=clip_steps.find(model+0x40);
    if(primary==clip_steps.end()) frame_clip_reset(model+0x58);
    else {
        const auto step=primary->second;
        clip_steps.insert_or_assign(model+0x58,step);
    }
}
void frame_clip_commit(Memory& memory,std::uint32_t clip,std::uint32_t bounds,float before,float increment) {
    if(clip_pruned_rule!=frame_rule_serial) {
        // Keep the previous rule's entries available for reuse by live clips.
        std::erase_if(clip_steps,[](const auto& entry){return frame_rule_serial-entry.second.rule>1;});
        clip_pruned_rule=frame_rule_serial;
    }
    clip_steps.insert_or_assign(clip,ClipStep{frame_rule_serial,memory.load<std::uint32_t>(clip+0xC),bounds,before,increment});
}
std::uint32_t frame_clip_sample(Memory& memory,std::uint32_t model,bool secondary) {
    const auto clip=model+(secondary?0x58U:0x40U);
    const auto current=memory.load<std::uint32_t>(clip+0xC);
    if(frame_step.phase==0 || frame_step.delta!=frame_step.interval) return current;

    float before,increment;
    std::uint32_t bounds;
    if(frame_step.logic) {
        if(memory.load<std::uint16_t>(clip)&2) return current;
        before=std::bit_cast<float>(current);
        increment=std::bit_cast<float>(memory.load<std::uint32_t>(clip+8))*
                  std::bit_cast<float>(memory.load<std::uint32_t>(model+4));
        bounds=memory.load<std::uint32_t>(clip+2);
    } else {
        const auto step=clip_steps.find(clip);
        if(step==clip_steps.end() || step->second.rule!=frame_rule_serial || step->second.after!=current) return current;
        before=step->second.before;
        increment=step->second.increment;
        bounds=step->second.bounds;
    }
    // The pose is drawn before the rule advances the playhead. Sample that
    // same interval after advancement, holding its endpoint until a wrap.
    const auto sample=before+increment*frame_step.phase;
    const auto time=increment>0?std::min(sample,std::max(before,float(bounds>>16))):
                    std::max(sample,std::min(before,float(bounds&0xFFFF)));
    return std::bit_cast<std::uint32_t>(time);
}

void frame_reset_world(std::uint32_t address) {
    if((address-0x73E930)%0x3C) throw std::runtime_error("World initializer is not at a record boundary");
    world_response(address)={};world_response(address+2)={};
}
std::uint32_t frame_world_response(FloatingState& floating,Memory& memory,std::uint32_t address,std::uint32_t difference,bool replay) {
    auto& state=world_response(address);
    const auto delta=static_cast<std::int16_t>(difference);
    if(frame_step.delta==1 && !state.initialized) return static_cast<std::uint32_t>(delta>>2);
    HostFloatingScope host(floating);
    const auto current=memory.load<std::uint16_t>(address);
    if(!state.initialized || state.previous!=current || state.kind!=unsigned(replay))
        state.value=static_cast<std::int16_t>(current);
    const auto fraction=std::remainder(state.value-static_cast<std::int16_t>(current),65536.0);
    const auto gain=frame_step.delta==1?0.25:-std::expm1(std::log(0.75)*frame_step.delta);
    state.value=std::remainder(state.value+(double(delta)-fraction)*gain,65536.0);
    // Carry sub-unit angle precision between scenes. Truncating each small
    // response separately changes the resting angle as refresh rate increases.
    state.previous=static_cast<std::uint16_t>(static_cast<std::int32_t>(std::floor(state.value)));
    state.initialized=true;state.kind=unsigned(replay);
    return static_cast<std::uint16_t>(state.previous-current);
}
std::uint32_t frame_world_steering(FloatingState& floating,Memory& memory,std::uint32_t address,std::uint32_t difference,std::uint32_t original) {
    auto& state=world_response(address);
    if(frame_step.delta==1 && !state.initialized) return original;
    HostFloatingScope host(floating);
    const auto current=memory.load<std::uint16_t>(address);
    if(!state.initialized || state.previous!=current || state.kind!=2)
        state.value=static_cast<std::int16_t>(current);
    // A7F20 first approaches the input by .2, then damps the result by .8.
    // Fractionally iterating these separately changes the equilibrium. Compose
    // the original affine map first; retain sub-unit precision until the store.
    constexpr double gain=0.2f,decay=0.8f,retention=decay*(1-gain);
    const double target=static_cast<std::int16_t>(current)-static_cast<std::int16_t>(difference);
    const auto equilibrium=decay*gain*target/(1-retention);
    state.value=equilibrium+(state.value-equilibrium)*std::pow(retention,frame_step.delta);
    state.previous=static_cast<std::uint16_t>(static_cast<std::int32_t>(state.value));
    state.initialized=true;state.kind=2;
    return state.previous;
}
std::uint32_t frame_world_decay(FloatingState& floating,Memory& memory,std::uint32_t address,std::uint32_t original) {
    auto& state=world_response(address);
    if(frame_step.delta==1 && !state.initialized) return original;
    HostFloatingScope host(floating);
    const auto current=memory.load<std::uint16_t>(address);
    if(!state.initialized || state.previous!=current || state.kind!=3)
        state.value=static_cast<std::int16_t>(current);
    // A7D05's gain had already been scaled, but rounding the resulting angle
    // every scene still lost time-dependent precision. Carry it at the store.
    state.value*=std::pow(1-double(0.2f),frame_step.delta);
    state.previous=static_cast<std::uint16_t>(static_cast<std::int32_t>(state.value));
    state.initialized=true;state.kind=3;
    return state.previous;
}
__m128 frame_decay(FloatingState& state,__m128 retention) {
    if(frame_step.delta==1) return retention;
    HostFloatingScope floating(state);
    const auto r=_mm_cvtss_f32(retention);
    if(!std::isfinite(r) || r<0 || r>1) throw std::runtime_error("Invalid continuous damping coefficient");
    return _mm_move_ss(retention,_mm_set_ss(std::pow(r,frame_step.delta)));
}
struct FrameReplayBallSample {FrameReplayBallSample();~FrameReplayBallSample();};
namespace {thread_local unsigned replay_ball_samples=0;}
FrameReplayBallSample::FrameReplayBallSample() {++replay_ball_samples;}
FrameReplayBallSample::~FrameReplayBallSample() {--replay_ball_samples;}
std::int32_t frame_nearest_angle(std::int32_t next,std::int32_t previous) {
    // Within the replay ball's sample, use the representation of a 16-bit
    // angle nearest the previous frame, so interpolation follows the shorter
    // arc across the wrap. Other callers keep the original interpolation.
    if(!replay_ball_samples) return next;
    const auto difference=next-previous;
    return difference>32768?next-65536:difference<-32768?next+65536:next;
}
__m128 frame_power(FloatingState& state,__m128 factor) {
    if(frame_step.delta==1) return factor;
    HostFloatingScope floating(state);
    const auto f=_mm_cvtss_f32(factor);
    if(!std::isfinite(f) || f<=0) throw std::runtime_error("Invalid continuous scaling factor");
    return _mm_move_ss(factor,_mm_set_ss(std::pow(f,frame_step.delta)));
}
__m128 frame_rotation_threshold(FloatingState& state,__m128 cosine,float interval) {
    if(interval==1) return cosine;
    HostFloatingScope floating(state);
    return _mm_move_ss(cosine,_mm_set_ss(static_cast<float>(std::cos(std::acos(_mm_cvtss_f32(cosine))*interval))));
}
float frame_rotation_snap(FloatingState& state,Memory& memory,float interval) {
    const auto gap=std::bit_cast<float>(memory.load<std::uint32_t>(0x347BC8));
    if(interval==1) return gap;
    HostFloatingScope floating(state);
    // 88F58 compares 1-dot(up, pose), not an angle or a linear distance.
    // A fractional scene may snap only as far as its actual 88FF1 turn.
    // Use that same 50/60 Hz degree conversion and integer phase boundary.
    const auto degrees=scalar_vector(memory.load<std::uint32_t>(memory.load<std::uint32_t>(0x6C7EA8)==50?0x378D98:0x34A1B4));
    const auto turn=_mm_cvttss_si32(_mm_mul_ss(degrees,scalar_vector(memory.load<std::uint32_t>(0x3A1680))));
    const auto sine=std::sin(double(frame_integer_increment(turn))*3.14159265358979323846/65536);
    return std::min(gap,static_cast<float>(2*sine*sine));
}
bool frame_upright_rotation(Cpu& cpu,Memory& memory,float interval) {
    if(interval==1) return false;
    HostFloatingScope floating(cpu.floating);
    const auto stack=cpu.registers[esp],output=memory.load<std::uint32_t>(stack+4);
    const auto left=memory.load<std::uint32_t>(stack+8),right=memory.load<std::uint32_t>(stack+12);
    std::array<double,3> a{},b{};
    double dot=0,aa=0,bb=0;
    for(unsigned i=0;i<3;++i) {
        a[i]=std::bit_cast<float>(memory.load<std::uint32_t>(left+4*i));
        b[i]=std::bit_cast<float>(memory.load<std::uint32_t>(right+4*i));
        dot+=a[i]*b[i];aa+=a[i]*a[i];bb+=b[i]*b[i];
    }
    // 29F40 discards rotations with dot > .99999. Fractional recovery can
    // reach that region before becoming upright and then never finish. Keep
    // its cross-product rotation for this caller, using the stable half-angle
    // form. All other angles/callers and original whole-tick arithmetic remain
    // in the original AOT routine, including its antiparallel handling.
    if(!(dot>std::bit_cast<float>(memory.load<std::uint32_t>(0x34B75C)))) return false;
    std::array<double,4> q{a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0],std::sqrt(aa*bb)+dot};
    const auto length=std::hypot(std::hypot(q[0],q[1]),std::hypot(q[2],q[3]));
    for(unsigned i=0;i<4;++i) memory.store<std::uint32_t>(output+4*i,std::bit_cast<std::uint32_t>(static_cast<float>(q[i]/length)));
    cpu.eip=memory.load<std::uint32_t>(stack);cpu.registers[esp]=stack+4;
    return true;
}
void frame_quaternion_step(FloatingState& state,Memory& memory,std::uint32_t address,float interval,bool whole_tick) {
    if(interval==1) return;
    HostFloatingScope floating(state);
    std::array<double,4> q{};
    for(unsigned i=0;i<4;++i) q[i]=std::bit_cast<float>(memory.load<std::uint32_t>(address+4*i));
    const auto length=std::hypot(q[0],q[1],q[2]);
    if(!length) return;
    const auto scale=whole_tick?(interval>0?1.0/interval:0):interval;
    const auto angle=std::atan2(length,q[3])*scale;
    const auto factor=std::sin(angle)/length;
    for(unsigned i=0;i<3;++i) q[i]*=factor;
    q[3]=std::cos(angle);
    for(unsigned i=0;i<4;++i) memory.store<std::uint32_t>(address+4*i,std::bit_cast<std::uint32_t>(static_cast<float>(q[i])));
}
__m128 frame_damped_increment(FloatingState& state,__m128 velocity,float retention) {
    if(frame_step.delta==1) return velocity;
    HostFloatingScope floating(state);
    const double r=retention;
    if(!std::isfinite(r) || r<0 || r>1) throw std::runtime_error("Invalid continuous damping coefficient");
    // Fractional iterate of v *= r; p += v, using the already-decayed velocity.
    const double scale=r==1?frame_step.delta:r==0?0:r*std::expm1(-std::log(r)*frame_step.delta)/(1-r);
    return _mm_mul_ss(velocity,_mm_set_ss(static_cast<float>(scale)));
}
void frame_record_sample(FloatingState& state,Memory& memory,std::array<std::uint32_t,24>& saved,bool restore) {
    HostFloatingScope floating(state);
    const double back=frame_step.delta>0?std::clamp(double(frame_step.phase)/frame_step.delta,0.0,1.0):0;
    constexpr std::array offsets{4U,8U,12U,0x28U,0x2CU};
    for(unsigned player=0;player<4;++player) {
        const auto ball=0x738DF0U+player*0x1B0,world=0x73E930U+player*0x3C;
        for(unsigned i=0;i<offsets.size();++i) {
            if(restore) memory.store<std::uint32_t>(ball+offsets[i],saved[player*6+i]);
            else saved[player*6+i]=memory.load<std::uint32_t>(ball+offsets[i]);
        }
        if(restore) {memory.store<std::uint32_t>(world,saved[player*6+5]);continue;}
        saved[player*6+5]=memory.load<std::uint32_t>(world);
        const auto type=memory.load<std::uint8_t>(ball+3);
        if(!memory.load<std::uint8_t>(ball) || (type!=4 && type!=6 && type!=20 && type!=28 && type!=32)) continue;
        // The recorder writes uniform 60 Hz samples. A rule tick can fall inside
        // the current scene interval; sample that instant, then restore live poses.
        for(unsigned axis=0;axis<3;++axis) {
            const double now=std::bit_cast<float>(saved[player*6+axis]);
            const double previous=std::bit_cast<float>(memory.load<std::uint32_t>(ball+0x10+axis*4));
            memory.store<std::uint32_t>(ball+4+axis*4,std::bit_cast<std::uint32_t>(static_cast<float>(now+(previous-now)*back)));
        }
        const auto angle=[&](std::uint32_t address,std::int16_t movement) {
            const auto now=memory.load<std::uint16_t>(address);
            memory.store<std::uint16_t>(address,static_cast<std::uint16_t>(now-std::lround(movement*back)));
        };
        for(unsigned axis=0;axis<3;++axis)
            angle(ball+0x28+axis*2,std::bit_cast<std::int16_t>(memory.load<std::uint16_t>(ball+0x60+axis*2)));
        for(unsigned axis=0;axis<2;++axis)
            angle(world+axis*2,std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(memory.load<std::uint16_t>(world+axis*2)-memory.load<std::uint16_t>(world+4+axis*2))));
    }
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
InterfaceHooks interface_hooks;
}
void set_interface_hooks(const InterfaceHooks& hooks) {interface_hooks=hooks;}
// The original immediate frontend (31200..31462) supplies plain XYZ quads,
// sometimes with packed colour and UVs. Complete their shared visibility and
// screen-edge contracts before 313B0 submits them, including font batches.
void complete_screen_primitives(Memory& memory,bool ultrawide) {
    if(memory.load<std::uint32_t>(0x6DEF70U)!=8 || memory.load<std::uint32_t>(0x6DEF7CU)) return;
    const auto format=memory.load<std::uint32_t>(0x6DE3E0U);
    unsigned stride;
    switch(format) {
    case 0x200:stride=12;break;
    case 0xA00:stride=16;break;
    case 0x2200:stride=20;break;
    case 0x2A00:stride=24;break;
    case 0x6200:stride=28;break;
    default:return; // Normal/skinning layouts are scene geometry.
    }
    // Texture coordinate sets 0x2000/0x4000 sample stages 0/1. SDK SetTexture
    // (31E4A0) keeps each stage's texture at device+F88+stage*4; the D3D
    // object holds its data address at +4, as the NV097 texture offset uses.
    if(interface_hooks.texture) for(unsigned stage=0;stage<2;++stage) if(format&(0x2000U<<stage)) {
        const auto read=[&](std::uint32_t address) {
            std::uint32_t value=0;
            if(const auto* bytes=memory.try_access(address,sizeof(value))) std::memcpy(&value,bytes,sizeof(value));
            return value;
        };
        const auto device=read(0x32B6A8U),object=device?read(device+0xF88+stage*4):0U,data=object?read(object+4):0U;
        if(data) interface_hooks.texture(interface_hooks.context,data);
    }
    // Four vertices occupy stride DWORDs. Process each quad in a batch with
    // fixed scratch storage; there is no packet scan or deferred state.
    const auto words=memory.load<std::uint32_t>(0x6DEF6CU);
    if(!words || words%stride || words/stride>8192) return;
    struct Restore {unsigned status=_mm_getcsr();~Restore(){_mm_setcsr(status);}} restore;
    const auto begin=memory.load<std::uint32_t>(0x6DEF60U)-4*words;
    const bool deferred=memory.load<std::uint32_t>(0x6DEF74U)!=0;
    const auto total=deferred?memory.load<std::uint32_t>(0x6DEF78U):0;
    const bool clip=ultrawide && memory.load<std::uint8_t>(0x6C934CU) &&
        memory.load<std::uint32_t>(0x7196ECU) && memory.load<std::uint32_t>(0x6DE0D4U)==1 &&
        (deferred?total>=words:words<=0x7FF && memory.load<std::uint32_t>(begin-4)==(0x40001818U|(words<<18)));
    const float anchor=clip?static_cast<std::int16_t>(memory.load<std::uint16_t>(0x71B310U)):0;
    const float native_width=clip?memory.load<std::uint16_t>(memory.load<std::uint32_t>(0x6DD520U)+4):0;
    const auto visible=[&](std::uint32_t vertices) {
        // 74E70's original 16:9 transform scales around the UI anchor. Preserve
        // its visibility before widening, using every rotated corner. Vertex
        // layouts and batched outlines share this contract with single sprites.
        float minimum=0,maximum=0;
        for(unsigned i=0;i<4;++i) {
            const auto x=std::bit_cast<float>(memory.load<std::uint32_t>(vertices+i*stride));
            if(!std::isfinite(x)) return true;
            const float native=anchor+(x-anchor)*0.75f;
            if(!i) minimum=maximum=native;
            else {minimum=std::min(minimum,native);maximum=std::max(maximum,native);}
        }
        return maximum>=0 && minimum<=native_width;
    };
    // One quad's vertices and vertex-space bounds: a flat, finite candidate
    // rectangle, or false.
    struct Quad {
        std::array<std::array<std::uint32_t,7>,4> vertices{};
        std::array<float,2> minimum{},maximum{};
        float value(unsigned i,unsigned component) const {return std::bit_cast<float>(vertices[i][component]);}
    };
    const auto read_quad=[&](std::uint32_t begin,Quad& quad) {
        for(unsigned i=0;i<4;++i) std::memcpy(quad.vertices[i].data(),memory.access(begin+i*stride,stride),stride);
        const float z=quad.value(0,2);
        quad.minimum={quad.value(0,0),quad.value(0,1)};quad.maximum=quad.minimum;
        for(unsigned i=0;i<4;++i) {
            if(quad.value(i,2)!=z || !std::isfinite(z)) return false;
            for(unsigned axis=0;axis<2;++axis) {
                const float v=quad.value(i,axis);if(!std::isfinite(v)) return false;
                quad.minimum[axis]=std::min(quad.minimum[axis],v);quad.maximum[axis]=std::max(quad.maximum[axis],v);
            }
        }
        return quad.minimum[0]!=quad.maximum[0] && quad.minimum[1]!=quad.maximum[1];
    };
    // 2F790 uploads projection * the current 3x4 model/view to k96..99.
    // Use that same matrix and the SDK viewport; never infer camera dimensions
    // from a particular menu, sprite number or render-target allocation.
    struct Screen {bool identity=true;std::array<std::uint32_t,4> viewport{};std::array<float,2> scale{},offset{};};
    const auto screen_transform=[&](float z,Screen& view) {
        std::array<std::array<float,4>,4> projection{},matrix{};
        std::array<std::array<float,4>,3> model{};
        std::memcpy(projection.data(),memory.access(0x6DDF28U,sizeof(projection)),sizeof(projection));
        std::memcpy(model.data(),memory.access(memory.load<std::uint32_t>(0x6DDEC4U),sizeof(model)),sizeof(model));
        for(unsigned row=0;row<3;++row) for(unsigned col=0;col<4;++col) view.identity&=model[row][col]==float(row==col);
        for(unsigned row=0;row<4;++row) for(unsigned col=0;col<4;++col) {
            matrix[row][col]=projection[row][0]*model[0][col]+projection[row][1]*model[1][col]+projection[row][2]*model[2][col];
            if(col==3) matrix[row][col]+=projection[row][3];
        }
        if(matrix[0][1]!=0 || matrix[1][0]!=0 || matrix[2][0]!=0 || matrix[2][1]!=0 || matrix[3][0]!=0 || matrix[3][1]!=0) return false;
        const float w=matrix[3][2]*z+matrix[3][3];if(!std::isfinite(w) || w<=0) return false;
        const auto device=memory.load<std::uint32_t>(0x32B6A8U);
        std::memcpy(view.viewport.data(),memory.access(device+0xEE0,sizeof(view.viewport)),sizeof(view.viewport));
        if(!view.viewport[2] || !view.viewport[3]) return false;
        for(unsigned axis=0;axis<2;++axis) {
            const float half=float(view.viewport[axis+2])/2,sign=axis?-1.0f:1.0f;
            view.scale[axis]=sign*half*matrix[axis][axis]/w;
            view.offset[axis]=float(view.viewport[axis])+half+sign*half*(matrix[axis][2]*z+matrix[axis][3])/w;
            if(!std::isfinite(view.scale[axis]) || !std::isfinite(view.offset[axis]) || view.scale[axis]==0) return false;
        }
        return true;
    };
    // NV2A rasterizes the viewport position plus the SDK's 0.53125 screen
    // offset, truncated to 1/16 pixel, less a 1/32 bias. Pixel centres lie
    // at k+0.5 in this space.
    const auto rasterize=[](float p) {return std::trunc((p+0.53125f)*16)/16-0.03125f;};
    const auto complete=[&](std::uint32_t begin) {
        Quad quad;Screen view;
        if(!read_quad(begin,quad) || !screen_transform(quad.value(0,2),view)) return;
        auto& vertices=quad.vertices;const auto& minimum=quad.minimum;const auto& maximum=quad.maximum;
        const auto value=[&](unsigned i,unsigned component) {return quad.value(i,component);};
        const auto& viewport=view.viewport;const bool identity=view.identity;
        const auto screen=memory.load<std::uint32_t>(0x6DD520U);
        const float width=memory.load<std::uint16_t>(screen+4),height=memory.load<std::uint16_t>(screen+6);
        // SDK render-state mirrors: ZERO + destination ONE with ADD preserves
        // every colour/alpha channel. A plain quad in that state is a depth mask,
        // including masks smaller than their projection viewport.
        const bool depth_only=format==0x200 && memory.load<std::uint32_t>(0x32B4FCU)!=0 &&
            memory.load<std::uint32_t>(0x32B508U)==0 && memory.load<std::uint32_t>(0x32B50CU)==1 &&
            memory.load<std::uint32_t>(0x32B538U)==0x8006;
        const bool canvas=identity && memory.load<std::uint32_t>(0x6DE0D4U)==1 && viewport[0]==0 && viewport[1]==0 &&
            viewport[2]==width && viewport[3]==height && std::round(minimum[0])<=0 && std::round(maximum[0])>=width &&
            std::round(minimum[1])<=0 && std::round(maximum[1])>=height;
        struct Edge {float nominal,position,raster;};
        struct Axis {float scale,offset;Edge low,high;};
        std::array<Axis,2> axes{};
        bool changed=false;
        for(unsigned axis=0;axis<2;++axis) {
            auto& a=axes[axis];a.scale=view.scale[axis];a.offset=view.offset[axis];
            const auto p0=a.scale*minimum[axis]+a.offset,p1=a.scale*maximum[axis]+a.offset;
            const float first=float(viewport[axis]),last=first+float(viewport[axis+2]);
            const auto low=std::min(p0,p1),high=std::max(p0,p1);
            if(!canvas && !depth_only && (std::round(low)>first || std::round(high)<last)) return;
            const auto edge=[&](float p,float boundary,bool upper) {
                const auto raster=rasterize(p);
                const auto snapped=std::round(boundary*16)/16+(upper?0.0625f:0);
                const auto aligned=snapped-0.03125f;
                const bool fit=depth_only || std::round(p)==boundary || raster==aligned || (canvas && (upper?p<boundary:p>boundary));
                changed|=fit && raster!=aligned;
                // Invert NV2A's signed truncation at the centre of its 1/16 cell.
                // This gives the same outward coverage at zero and at positive
                // viewport origins; subtracting one fixed offset does not.
                const auto position=snapped+(snapped>0?0.03125f:snapped<0?-0.03125f:0)-0.53125f;
                return fit?Edge{boundary,position,aligned}:Edge{p,p,raster};
            };
            a.low=edge(low,depth_only?low:first,false);a.high=edge(high,depth_only?high:last,true);
        }
        if(!changed) return;
        std::array<unsigned,4> corners{},corner{};unsigned mask=0;
        for(unsigned i=0;i<4;++i) {
            if((value(i,0)!=minimum[0] && value(i,0)!=maximum[0]) || (value(i,1)!=minimum[1] && value(i,1)!=maximum[1])) return;
            corner[i]=unsigned((value(i,0)==maximum[0])==(axes[0].scale>0))|
                (unsigned((value(i,1)==maximum[1])==(axes[1].scale>0))<<1);
            if(mask&(1U<<corner[i])) return;
            mask|=1U<<corner[i];corners[corner[i]]=i;
        }
        for(unsigned i=0;i<4;++i) if((corner[i]^corner[(i+1)%4])==3) return;
        const auto original=vertices;
        const unsigned colour=(format&0x800)?1:0,uv=3+colour;
        for(unsigned i=0;i<4;++i) {
            std::array<float,2> ratio{};
            for(unsigned axis=0;axis<2;++axis) {
                const auto& a=axes[axis];const auto& e=(corner[i]&(1U<<axis))?a.high:a.low;
                vertices[i][axis]=std::bit_cast<std::uint32_t>((e.position-a.offset)/a.scale);
                ratio[axis]=(e.raster-a.low.nominal)/(a.high.nominal-a.low.nominal);
            }
            const auto interpolate=[&](const auto& component) {
                const auto low=component(corners[0])+(component(corners[1])-component(corners[0]))*ratio[0];
                const auto high=component(corners[2])+(component(corners[3])-component(corners[2]))*ratio[0];
                return low+(high-low)*ratio[1];
            };
            // Reparameterize every UV set with the actual raster edges. This is
            // necessary for a scene copy to keep its texels aligned, not stretched.
            for(unsigned component=uv;component<stride/4;++component) {
                const auto v=interpolate([&](unsigned n){return std::bit_cast<float>(original[n][component]);});
                if(!std::isfinite(v)) return;
                vertices[i][component]=std::bit_cast<std::uint32_t>(v);
            }
            if(colour) {
                vertices[i][3]=0;
                for(unsigned channel=0;channel<4;++channel) {
                    const auto v=interpolate([&](unsigned n){return float((original[n][3]>>(8*channel))&255);});
                    vertices[i][3]|=std::uint32_t(std::clamp(std::round(v),0.0f,255.0f))<<(8*channel);
                }
            }
        }
        for(unsigned i=0;i<4;++i) std::memcpy(memory.access(begin+i*stride,stride),vertices[i].data(),stride);
    };
    // Report the texture rectangle each UV set of a frontend quad spans.
    // Font and sprite atlases pack cells edge to edge. Above 1x, samples near
    // a quad's edge blend in the neighbouring cell (thin dark lines beside
    // HUD and menu text); the GPU keeps them half a texel inside this
    // rectangle. Any screen rotation qualifies; the UVs must form a rectangle.
    const unsigned sets=((format>>13)&1)+((format>>14)&1),first_uv=(format&0x800)?4:3;
    // The UVs the original raster samples at its outermost covered pixel
    // centres. NV2A interpolates them from the rasterized vertex positions, and
    // a centre lies 1/32 pixel inside a leading edge, so bilinear filtering
    // there reads the texel before the rectangle: glyphs whose UVs start one
    // texel into their cell keep their top row only through those samples.
    // Screen-aligned quads whose UV components each follow one axis qualify.
    // Also report, per screen axis, the vertex coordinate that rasterizes at
    // the boundary of the original's first covered pixel (k-1/32): the GPU
    // extends a leading edge there when it holds art the UVs skip.
    const auto original_samples=[&](const Quad& quad,InterfaceSampleBounds& bounds) {
        Screen view;
        if(!screen_transform(quad.value(0,2),view)) return;
        std::array<std::array<float,2>,2> raster{},centre{};
        for(unsigned axis=0;axis<2;++axis) {
            for(unsigned i=0;i<4;++i) if(quad.value(i,axis)!=quad.minimum[axis] && quad.value(i,axis)!=quad.maximum[axis]) return;
            raster[axis]={rasterize(view.scale[axis]*quad.minimum[axis]+view.offset[axis]),
                rasterize(view.scale[axis]*quad.maximum[axis]+view.offset[axis])};
            const auto low=std::min(raster[axis][0],raster[axis][1]),high=std::max(raster[axis][0],raster[axis][1]);
            // Centres at k+0.5 with low<=centre<high; the 1/32 bias rules out ties.
            centre[axis]={std::ceil(low-0.5f)+0.5f,std::ceil(high-0.5f)-0.5f};
            if(!(centre[axis][0]<=centre[axis][1])) return;
            // Invert NV2A's signed 1/16 truncation at the centre of the cell.
            const auto snapped=centre[axis][0]-0.5f;
            const auto position=snapped+(snapped>0?0.03125f:snapped<0?-0.03125f:0)-0.53125f;
            bounds.leading_edge[axis]=(position-view.offset[axis])/view.scale[axis];
            if(raster[axis][1]<raster[axis][0]) bounds.leading_max|=1U<<axis;
        }
        bounds.uv_word=first_uv;
        for(unsigned set=0;set<sets;++set) {
            if(!(bounds.mask&(1U<<set))) continue;
            std::array<float,4> range{};bool complete=true;
            for(unsigned c=0;c<2 && complete;++c) {
                const unsigned component=first_uv+set*2+c;
                bool found=false;
                for(unsigned axis=0;axis<2 && !found;++axis) {
                    std::array<float,2> at{};std::array<bool,2> seen{};bool follows=true;
                    for(unsigned i=0;i<4;++i) {
                        const unsigned side=quad.value(i,axis)==quad.maximum[axis];
                        if(seen[side] && at[side]!=quad.value(i,component)) follows=false;
                        at[side]=quad.value(i,component);seen[side]=true;
                    }
                    if(!follows || !seen[0] || !seen[1]) continue;
                    const auto uv=[&](float p) {return at[0]+(at[1]-at[0])*(p-raster[axis][0])/(raster[axis][1]-raster[axis][0]);};
                    const auto first=uv(centre[axis][0]),last=uv(centre[axis][1]);
                    if(!std::isfinite(first) || !std::isfinite(last)) break;
                    range[c]=std::min(first,last);range[c+2]=std::max(first,last);found=true;
                    if(first>last) bounds.leading_high|=1U<<(set*2+c);
                    if(axis) bounds.follows_y|=1U<<(set*2+c);
                }
                complete=found;
            }
            if(complete) {bounds.sampled[set]=range;bounds.sampled_mask|=1U<<set;}
            else {bounds.leading_high&=~(3U<<(set*2));bounds.follows_y&=~(3U<<(set*2));}
        }
    };
    // The game's own texture rectangle and the original's samples.
    const auto sample_bounds=[&](std::uint32_t begin,InterfaceSampleBounds& bounds) {
        Quad quad;
        if(!read_quad(begin,quad)) return false;
        for(unsigned set=0;set<sets;++set) {
            auto& range=bounds.sets[set];
            for(unsigned c=0;c<2;++c) {
                const auto at=[&](unsigned i) {return quad.value(i,first_uv+set*2+c);};
                float low=at(0),high=low;
                for(unsigned i=0;i<4;++i) {if(!std::isfinite(at(i))) return false;low=std::min(low,at(i));high=std::max(high,at(i));}
                if(!(high>low)) return false;
                for(unsigned i=0;i<4;++i) if(at(i)!=low && at(i)!=high) return false;
                range[c]=low;range[c+2]=high;
            }
            bounds.mask|=1U<<set;
        }
        if(bounds.mask) original_samples(quad,bounds);
        return bounds.mask!=0;
    };
    unsigned kept=0;
    for(unsigned quad=0;quad<words/stride;++quad) {
        const auto source=begin+quad*stride*4;
        if(clip && !visible(source)) continue;
        complete(source);
        InterfaceSampleBounds bounds;
        if(sets && interface_hooks.bounds && sample_bounds(source,bounds)) interface_hooks.bounds(interface_hooks.context,
            interface_quad_identity({static_cast<const std::byte*>(memory.access(source,stride*4)),stride*4}),bounds);
        if(kept!=quad) std::memcpy(memory.access(begin+kept*stride*4,stride*4),memory.access(source,stride*4),stride*4);
        ++kept;
    }
    const auto retained=kept*stride;
    if(retained==words) return;
    // Compact actual primitives, preserving every attribute and all surrounding
    // GPU state. 31170 emits deferred batches later; immediate batches already
    // have their inline-array header. Both allow an empty begin/end sequence.
    if(deferred) memory.store<std::uint32_t>(0x6DEF78U,total-words+retained);
    else memory.store<std::uint32_t>(begin-4,0x40001818U|(retained<<18));
    memory.store<std::uint32_t>(0x6DEF6CU,retained);
    memory.store<std::uint32_t>(0x6DEF60U,begin+retained*4);
}
namespace {
struct CallSample {
    std::uint64_t visits=0;
    std::array<std::uint32_t,8> registers{},stack{};
    std::array<std::array<std::uint32_t,4>,8> pointees{};
};
bool trace_calls=false;
std::array<std::uint64_t,2> ui_draw_calls{};
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
void enable_call_trace() {call_samples.clear();dropped_calls=0;ui_draw_calls={};trace_calls=true;}
void trace_ui_draw(Cpu&,Memory&) {if(trace_calls) ++ui_draw_calls[frame_step.logic?1:0];}
std::string call_trace_report(std::string_view executable_sha256) {
    std::ostringstream out;
    out<<"{\"format\":\"smbd-call-trace-v1\",\"ui_draw_calls\":{\"intermediate\":"<<ui_draw_calls[0]<<",\"logic\":"<<ui_draw_calls[1]<<"},\"executable_sha256\":"<<json(executable_sha256)
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
