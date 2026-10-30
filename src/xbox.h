#pragma once
#include "cpu.h"
#include "xbe.h"
#include "performance.h"
#include <chrono>
#include <functional>
#include <memory>

namespace smbd {
class Renderer;
class Input;
struct FrameCaptureStatus;
struct XboxHost {
    Renderer* renderer=nullptr;
    Input* input=nullptr;
    std::function<void()> flip;
    std::function<bool(std::uint64_t logic_ticks)> poll;
    std::function<unsigned()> refresh_rate;
    std::function<void(std::chrono::steady_clock::time_point)> idle;
    std::function<void()> publish_memory;
    std::function<void(Cpu&, Memory&, std::uint32_t)> platform;
    std::function<std::uint64_t()> frames;
    std::function<std::uint64_t()> presented_frames;
    std::filesystem::path capture;
    std::filesystem::path storage;
    Performance* performance=nullptr;
    std::uint32_t main_address = 0, vblank_callback_slot = 0;
    bool trace_io = false, mute_audio = true;
    bool trace_storage=false;
    bool widescreen=false,ultrawide=false;
};
struct BootIo {
    std::uint32_t ordinal=0,status=0,information=0,callsite=0;
    std::string path;
    std::uint32_t options=0,requested=0;
    std::uint64_t offset=0;
    std::uint32_t buffer=0;
};
struct BootAllocation { std::uint32_t address=0,size=0,committed=0,protect=0; };
struct BootThread {
    std::uint32_t id=0,routine=0,system=0,eip=0,stack=0,wait_object=0;
    std::string state;
    bool suspended=false;
    std::uint32_t optional_callback_record=0;
};
struct BootResult {
    bool entry_returned=false,main_reached=false;
    std::vector<BootIo> io;
    std::vector<BootAllocation> allocations;
    std::vector<BootThread> threads;
    std::array<std::uint32_t,5> last_failed_allocation{};
    std::uint32_t active_thread=0,threads_created=0;
    std::uint64_t vblank_callbacks=0,graphics_callbacks=0;
    std::uint64_t apcs_delivered=0;
    std::uint64_t draws=0,frames=0,submissions=0,vertices=0,blits=0;
    std::uint64_t simulation_steps=0,timing_epochs=0;
    double simulation_ms=0;
    std::string adapter;
    std::string capture_error;
    bool capture_written=false;
    std::string boundary;
    Cpu cpu;
    ExecutionDiagnostic diagnostic;
    std::array<std::uint32_t,16> stack_words{};
    std::uint32_t stack_word_count=0;
    std::array<std::uint64_t,512> service_calls{};
};
class Xbox {
public:
    Xbox(Xbe&,Memory&,std::span<std::byte>,const std::filesystem::path& disc={},XboxHost host={});
    ~Xbox();
    Xbox(const Xbox&)=delete;
    Xbox& operator=(const Xbox&)=delete;
    BootResult run(std::uint32_t budget,std::uint32_t break_address=0,std::uint32_t break_hit=1);
    void capture_frame(const std::filesystem::path& directory);
    void cancel_capture(std::string_view reason);
    const FrameCaptureStatus& capture_status() const;
    PerfCounters performance_counters() const;
    void performance_recording(bool);
    void output_gain(float);
private:
    friend void native_platform(Cpu&,Memory&,std::uint32_t);
    friend void native_frame_wait(Cpu&,Memory&);
    friend void native_frame_begin(Cpu&,Memory&);
    friend void native_stage_time(Cpu&,Memory&);
    friend void native_sample_goals(Cpu&,Memory&);
    friend bool native_goal_query(Cpu&,Memory&,std::uint32_t);
    struct State;
    std::unique_ptr<State> state_;
};
std::string check_boot(Xbe&,const std::filesystem::path&,std::uint32_t budget,const std::filesystem::path& capture={});
std::string boot_report(const Xbe&,const BootResult&);
}
