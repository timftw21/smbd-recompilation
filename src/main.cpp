#include "xbox.h"
#include "render.h"
#include "gpu.h"
#include "input.h"
#include "automation.h"
#include "call_trace.h"
#include "diagnostics.h"
#include "settings.h"
#include "frame_clock.h"
#include <SDL3/SDL.h>
#include <charconv>
#include <cmath>
#include <chrono>
#include <fstream>
#include <format>
#include <iostream>
#include <Windows.h>

namespace {
struct Sdl {
    Sdl() {if(!SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMEPAD)) throw std::runtime_error(SDL_GetError());}
    ~Sdl() {SDL_Quit();}
};
unsigned presentation_rate(const smbd::Settings& settings,const smbd::SettingsUi& ui) {
    auto rate=static_cast<unsigned>(settings.refresh_rate);
    if(settings.vsync && ui.display_refresh()>=30) {
        const auto display=static_cast<unsigned>(std::lround(ui.display_refresh()));
        if(!rate || rate>display) rate=display;
    }
    return rate;
}
std::filesystem::path application_root() {
    std::array<wchar_t,32768> path{};const auto count=GetModuleFileNameW(nullptr,path.data(),static_cast<DWORD>(path.size()));
    if(!count || count==path.size()) throw std::runtime_error("Cannot locate the application folder");
    const auto executable=std::filesystem::path(std::wstring_view(path.data(),count)).parent_path();
    auto candidate=executable;
    for(unsigned depth=0;depth<4;++depth,candidate=candidate.parent_path())
        if(std::filesystem::exists(candidate/"knowledge/supported_target.json")) return candidate;
    return executable;
}
std::string window_diagnostic(SDL_Window* window,const smbd::Renderer& renderer,const smbd::Settings& settings,
                              bool muted,bool automated,std::uint64_t frames,std::string_view sha,bool playing,int aspect) {
    int width=0,height=0,pixel_width=0,pixel_height=0;
    if(!SDL_GetWindowSize(window,&width,&height) || !SDL_GetWindowSizeInPixels(window,&pixel_width,&pixel_height))
        throw std::runtime_error(SDL_GetError());
    const auto utc=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    return std::format("{{\"format\":\"smbd-window-state-v1\",\"phase\":\"capture_request\",\"utc_ms\":{},\"source_sha256\":{},\"build\":{},\"platform\":{},\"sdl_version\":{},\"video_driver\":{},\"adapter\":{},\"logical_size\":[{},{}],\"pixel_size\":[{},{}],\"pixel_density\":{},\"display_scale\":{},\"fullscreen\":{},\"vsync\":{},\"resolution_scale\":{},\"widescreen\":{},\"aspect_mode\":{},\"requested_aspect_mode\":{},\"audio_muted\":{},\"inputs_automated\":{},\"playing\":{},\"frame\":{},\"session_log\":{},\"refresh_rate\":{},\"logic_hz\":60,\"native_scene_steps\":true,\"interpolation\":false}}\n",
        utc,smbd::json(sha),smbd::json(__DATE__ " " __TIME__),smbd::json(SDL_GetPlatform()),SDL_GetVersion(),smbd::json(SDL_GetCurrentVideoDriver()),smbd::json(renderer.adapter()),
        width,height,pixel_width,pixel_height,SDL_GetWindowPixelDensity(window),SDL_GetWindowDisplayScale(window),
        (SDL_GetWindowFlags(window)&SDL_WINDOW_FULLSCREEN)!=0,settings.vsync,renderer.stats().resolution_scale,aspect!=0,aspect,settings.aspect,muted,automated,playing,frames,smbd::json(smbd::path_text(smbd::diagnostic_log())),settings.refresh_rate);
}
struct WriteWatches {
    unsigned count=0;
    ~WriteWatches() {
        for(unsigned slot=0;slot<count;++slot) try {smbd::diagnostic_watch(slot,nullptr);}
        catch(const std::exception& error) {
            smbd::diagnostic_record("{\"type\":\"watch_cleanup_error\",\"message\":"+smbd::json(error.what())+'}');
        }
    }
};
int run(int argc,wchar_t** argv,bool restarting=false) {
    std::uint64_t frame_limit=0,seconds=0,capture_at=1,performance_at=1,performance_frames=3600,motion_at=2600,motion_frames=120,benchmark_ticks=0;
    std::filesystem::path capture,timeline,report,call_trace,frame_capture,storage,performance_output,motion_report;
    const auto root=application_root();
    std::filesystem::path settings_path=root/"data/local/settings.cfg",settings_capture,executable_override,disc_override;
    bool hidden=false,stop_after_input=false,muted=false,capture_at_set=false,performance_limits_set=false,trace_storage=false;
    std::vector<std::uint32_t> watches;
    for(int i=1;i<argc;++i) {
        const auto option=smbd::utf8(argv[i]);
        if(option=="--hidden") hidden=true;
        else if(option=="--trace-storage") trace_storage=true;
        else if(option=="--mute") muted=true;
        else if(option=="--stop-after-input") stop_after_input=true;
        else if(option=="--capture" && i+1<argc) capture=argv[++i];
        else if(option=="--capture-frame" && i+1<argc) frame_capture=argv[++i];
        else if(option=="--input" && i+1<argc) timeline=argv[++i];
        else if(option=="--report" && i+1<argc) report=argv[++i];
        else if(option=="--storage" && i+1<argc) storage=argv[++i];
        else if(option=="--performance" && i+1<argc) performance_output=argv[++i];
        else if(option=="--motion-report" && i+1<argc) motion_report=argv[++i];
        else if(option=="--settings" && i+1<argc) settings_path=argv[++i];
        else if(option=="--settings-capture" && i+1<argc) settings_capture=argv[++i];
        else if(option=="--xbe" && i+1<argc) executable_override=argv[++i];
        else if(option=="--disc" && i+1<argc) disc_override=argv[++i];
        else if(option=="--trace-calls" && i+1<argc) call_trace=argv[++i];
        else if(option=="--watch-write" && i+1<argc) {
            const auto value=smbd::utf8(argv[++i]);
            const bool hex=value.starts_with("0x") || value.starts_with("0X");
            std::uint32_t address=0;
            const auto parsed=std::from_chars(value.data()+(hex?2:0),value.data()+value.size(),address,hex?16:10);
            if(parsed.ec!=std::errc{} || parsed.ptr!=value.data()+value.size() || watches.size()==4)
                throw std::runtime_error("Write watches require an address and at most four slots");
            watches.push_back(address);
        }
        else if((option=="--frames" || option=="--seconds" || option=="--capture-at" || option=="--performance-at" || option=="--performance-frames" || option=="--motion-at" || option=="--motion-frames" || option=="--benchmark-ticks") && i+1<argc) {
            const auto value=smbd::utf8(argv[++i]);
            auto& limit=option=="--benchmark-ticks"?benchmark_ticks:option=="--frames"?frame_limit:option=="--seconds"?seconds:option=="--performance-at"?performance_at:option=="--performance-frames"?performance_frames:option=="--motion-at"?motion_at:option=="--motion-frames"?motion_frames:capture_at;
            if(option=="--capture-at") capture_at_set=true;
            if(option=="--performance-at" || option=="--performance-frames") performance_limits_set=true;
            const auto parsed=std::from_chars(value.data(),value.data()+value.size(),limit);
            if(parsed.ec!=std::errc{} || parsed.ptr!=value.data()+value.size() || !limit || (option=="--seconds" && limit>3600))
                throw std::runtime_error("Limits must be positive; seconds must not exceed 3600");
        } else throw std::runtime_error("Usage: smbd [--hidden] [--mute] [--frames COUNT] [--seconds COUNT] [--input TIMELINE.txt] [--stop-after-input] [--capture FRAME.png] [--capture-frame NEW_DIRECTORY] [--capture-at TICK] [--report OUTPUT.json] [--trace-calls OUTPUT.json] [--performance NEW_DIRECTORY] [--performance-at TICK] [--performance-frames COUNT] [--benchmark-ticks COUNT] [--motion-report NEW.json] [--motion-at TICK] [--motion-frames COUNT] [--settings PATH] [--settings-capture NEW_DIRECTORY] [--storage DIRECTORY] [--xbe FILE] [--disc ISO] [--watch-write ADDRESS (up to 4)]");
    }
    if(motion_frames>4096) throw std::runtime_error("Motion reports are limited to 4096 scene frames");
    if(!motion_report.empty() && std::filesystem::exists(motion_report)) throw std::runtime_error("Motion reports require a new file");
    if(stop_after_input && timeline.empty()) throw std::runtime_error("--stop-after-input requires --input");
    if(benchmark_ticks && (benchmark_ticks>3600 || timeline.empty() || performance_output.empty() || stop_after_input || performance_limits_set))
        throw std::runtime_error("--benchmark-ticks requires --input and --performance, at most 3600 ticks, and no other performance limits or --stop-after-input");
    if(capture_at_set && frame_capture.empty()) throw std::runtime_error("--capture-at requires --capture-frame");
    if(performance_limits_set && performance_output.empty()) throw std::runtime_error("Performance limits require --performance DIRECTORY");
    if(!performance_output.empty() && std::filesystem::exists(performance_output)) throw std::runtime_error("Use a fresh performance capture directory");
    muted=muted || hidden || !timeline.empty();
    smbd::Settings settings(root);std::string settings_error;
    try {settings.load(settings_path);} catch(const std::exception& error) {settings_error=error.what();}
    if(!executable_override.empty()) smbd::copy_text(settings.executable.data(),settings.executable.size(),smbd::path_text(std::filesystem::absolute(executable_override)));
    if(!disc_override.empty()) smbd::copy_text(settings.disc.data(),settings.disc.size(),smbd::path_text(std::filesystem::absolute(disc_override)));
    if(!timeline.empty() && !SDL_SetHintWithPriority(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,"1",SDL_HINT_OVERRIDE))
        throw std::runtime_error("Cannot enable background controller input for automation");
    Sdl sdl;
    const auto flags=SDL_WINDOW_RESIZABLE|(hidden?SDL_WINDOW_HIDDEN:0);
    std::unique_ptr<SDL_Window,decltype(&SDL_DestroyWindow)> window(
        SDL_CreateWindow("Super Monkey Ball Deluxe | F1: settings | F8: frame | F9: performance",static_cast<int>(720*settings.display_aspect()),720,flags),SDL_DestroyWindow);
    if(!window) throw std::runtime_error(SDL_GetError());
    SDL_SetWindowMinimumSize(window.get(),760,520);
    if(settings.fullscreen && !hidden && !SDL_SetWindowFullscreen(window.get(),true)) throw std::runtime_error(SDL_GetError());
    smbd::Renderer renderer(640,480,false,false,static_cast<unsigned>(settings.resolution));
    renderer.aspect_ratio(settings.display_aspect());
    renderer.attach_window(SDL_GetPointerProperty(SDL_GetWindowProperties(window.get()),SDL_PROP_WINDOW_WIN32_HWND_POINTER,nullptr));
    smbd::SettingsUi ui(window.get(),renderer,settings,settings_path);
    if(!settings_error.empty()) ui.message(settings_error);
    // Prepare the UI shaders/font atlas before gameplay or performance recording.
    renderer.clear(0xFF07130C);renderer.display();ui.draw(false,false,0,[]{},[]{});renderer.present(false);
    smbd::FrameClock launcher_clock;
    const auto pace_launcher=[&] {
        const auto now=smbd::FrameClock::Clock::now();launcher_clock.due(presentation_rate(settings,ui),now);
        if(now<launcher_clock.next()) SDL_DelayPrecise(static_cast<Uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(launcher_clock.next()-now).count()));
        const auto finished=smbd::FrameClock::Clock::now();launcher_clock.advance(finished);launcher_clock.skip_missed(finished);
    };
    const bool automated=hidden || frame_limit || seconds || !timeline.empty() || !frame_capture.empty() || !performance_output.empty();
    std::filesystem::path launcher_capture;
    const auto capture_launcher=[&] {
        const auto utc=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        launcher_capture=root/"build/captures"/std::format("frame-{}-{}",utc,SDL_GetTicksNS());
        std::filesystem::create_directories(launcher_capture);
    };
    if(!settings_capture.empty()) {
        if(std::filesystem::exists(settings_capture)) throw std::runtime_error("Use a fresh settings capture directory");
        std::filesystem::create_directories(settings_capture);
        for(int page=0;page<5;++page) {
            ui.category=page;
            for(unsigned frame=0;frame<3;++frame) {
                SDL_Event event;while(SDL_PollEvent(&event)) ui.event(event);
                renderer.clear(0xFF07130C);renderer.display();ui.draw(false,false,0,[]{},[]{});
                if(frame==2) smbd::save_png(renderer.read_display(),settings_capture/std::format("settings-{}.png",page));
                renderer.present(false);
            }
        }
        smbd::write_text(settings_capture/"report.json","{\"format\":\"smbd-settings-capture-v1\",\"pages\":5,\"passed\":true}\n",false);
        return 0;
    }
    if(!automated && !restarting) while(!ui.quit) {
        SDL_Event event;while(SDL_PollEvent(&event)) {
            ui.event(event);
            if(event.type==SDL_EVENT_KEY_DOWN && !event.key.repeat && event.key.scancode==SDL_SCANCODE_F8) capture_launcher();
        }
        renderer.clear(0xFF07130C);if(!renderer.display()) {SDL_Delay(10);continue;}
        ui.draw(false,false,0,capture_launcher,[]{});
        if(!launcher_capture.empty()) {
            smbd::save_png(renderer.read_display(),launcher_capture/"window.png");settings.save(launcher_capture/"settings.cfg");
            smbd::write_text(launcher_capture/"window.json",window_diagnostic(window.get(),renderer,settings,muted,false,0,"",false,settings.aspect),false);
            ui.message("Frame captured: "+smbd::path_text(launcher_capture));launcher_capture.clear();
        }
        pace_launcher();renderer.present(settings.vsync);
        if(ui.launch) {
            ui.launch=false;
            try {settings.validate();break;} catch(const std::exception& error) {ui.message(error.what());ui.category=0;}
        }
    }
    if(ui.quit) return 0;
    if(static_cast<unsigned>(settings.resolution)!=renderer.stats().resolution_scale) return 4;
    renderer.aspect_ratio(settings.display_aspect());
    ui.visible=false;
    smbd::Xbe image(settings.executable_path());
    std::vector<std::byte> ram(64*1024*1024);
    image.load(std::span(ram).subspan(image.base,image.image_size));
    smbd::Memory memory(0,ram);
    WriteWatches active_watches;
    for(unsigned slot=0;slot<watches.size();++slot) {
        smbd::diagnostic_watch(slot,memory.access(watches[slot],1));++active_watches.count;
        smbd::diagnostic_record("{\"type\":\"write_watch\",\"slot\":"+std::to_string(slot)+",\"guest_address\":"+smbd::json(smbd::hex32(watches[slot]))+'}');
    }
    std::unique_ptr<smbd::Automation> automation;
    if(!timeline.empty()) automation=std::make_unique<smbd::Automation>(timeline);
    smbd::Input input(!automation,false,automation?automation->device():0);
    std::uint64_t frames=0,presented_frames=0;
    smbd::FrameClock::Time first_source{},last_source{},first_present{},last_present{};
    smbd::FrameClock display_clock;
    double fps=0;std::uint64_t fps_frames=0;auto fps_started=std::chrono::steady_clock::now();
    const int active_scale=settings.resolution;
    const bool active_widescreen=settings.aspect!=0;
    const int active_aspect=settings.aspect;
    float output_gain=-1;
    const auto deadline=seconds?std::chrono::steady_clock::now()+std::chrono::seconds(seconds):std::chrono::steady_clock::time_point::max();
    smbd::XboxHost host;
    smbd::Performance performance;
    smbd::Xbox* running=nullptr;
    std::filesystem::path pending_window_capture;
    std::string window_capture_error;
    bool automatic_capture_requested=false,capture_was_pending=false;
    bool automatic_performance_requested=false;
    std::uint64_t performance_first=0;
    std::string performance_error;
    std::filesystem::path last_performance;
    std::uint64_t logic_ticks=0;
    std::uint64_t benchmark_first_tick=0,benchmark_mode_changes=0,benchmark_ball_changes=0;
    std::uint32_t benchmark_mode=0;
    std::uint8_t benchmark_ball=0;
    bool benchmark_complete=false;
    std::string benchmark_start;
    const auto benchmark_state=[&] {
        const auto value=[&](std::uint32_t address) {return std::bit_cast<float>(memory.load<std::uint32_t>(address));};
        const auto world=memory.load<std::uint8_t>(0x8B246A);
        return std::format("{{\"mode\":{},\"ball_state\":{},\"character\":{},\"practice_category\":{},\"practice_world\":{},\"practice_stage\":{},\"ball\":[{:.9g},{:.9g},{:.9g}],\"stage_time\":{:.9g}}}",
            memory.load<std::uint32_t>(0x6C9354),memory.load<std::uint8_t>(0x738DF3),memory.load<std::uint32_t>(0x739B70),
            memory.load<std::uint8_t>(0x8B2468),world,world<10?int(memory.load<std::uint8_t>(0x8B2475+world)):-1,
            value(0x738DF4),value(0x738DF8),value(0x738DFC),value(0x72FF84));
    };
    std::vector<std::string> motion_samples;
    if(!motion_report.empty()) motion_samples.reserve(static_cast<std::size_t>(motion_frames));
    const auto stop_performance=[&] {
        if(!performance.active()) return;
        try {
            renderer.finish_performance();performance.stop();
            running->performance_recording(false);
            const auto utc=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            const auto directory=!performance_output.empty()?performance_output:root/"build/captures"/std::format("performance-{}-{}",utc,SDL_GetTicksNS());
            performance.save(directory);last_performance=directory;
            settings.save(directory/"settings.cfg");
            if(benchmark_ticks && automatic_performance_requested)
                smbd::write_text(directory/"benchmark.json",std::format("{{\"format\":\"smbd-benchmark-window-v1\",\"complete\":{},\"requested_ticks\":{},\"first_tick\":{},\"last_tick\":{},\"first_frame\":{},\"last_frame\":{},\"mode_changes\":{},\"ball_state_changes\":{},\"start\":{},\"end\":{}}}\n",
                    benchmark_complete,benchmark_ticks,benchmark_first_tick,logic_ticks,performance_first+1,frames,benchmark_mode_changes,benchmark_ball_changes,benchmark_start,benchmark_state()),false);
            smbd::write_text(directory/"window.json",window_diagnostic(window.get(),renderer,settings,muted,bool(automation),frames,image.sha256,true,active_aspect),false);
            ui.message("Performance saved: "+smbd::path_text(directory));
            SDL_SetWindowTitle(window.get(),("Super Monkey Ball Deluxe | Performance saved: "+smbd::path_text(directory)).c_str());
            smbd::diagnostic_record("{\"type\":\"performance_saved\",\"directory\":"+smbd::json(smbd::path_text(directory))+'}');
        } catch(const std::exception& error) {
            performance.stop();performance_error=error.what();
            SDL_SetWindowTitle(window.get(),("Super Monkey Ball Deluxe | Performance: "+performance_error).c_str());
            ui.message(performance_error);
        }
        renderer.performance(nullptr,frames+1);
    };
    const auto start_performance=[&] {
        if(!running || performance.active()) return;
        performance.start(renderer.adapter(),settings.refresh_rate,active_scale,settings.vsync,running->performance_counters());
        renderer.performance(&performance,frames+1);performance_first=frames;
        if(benchmark_ticks) {
            benchmark_first_tick=logic_ticks;benchmark_start=benchmark_state();
            benchmark_mode=memory.load<std::uint32_t>(0x6C9354);benchmark_ball=memory.load<std::uint8_t>(0x738DF3);
        }
        running->performance_recording(true);
        renderer.performance_resources();
        SDL_SetWindowTitle(window.get(),"Super Monkey Ball Deluxe | Recording performance - F9 saves (60 second limit)");
        ui.message("Recording performance. F9 saves; recording stops after 60 seconds.");
    };
    const auto request_capture=[&](const std::filesystem::path& directory) {
        if(!running || running->capture_status().pending) return;
        smbd::PerfScope scope(&performance,smbd::PerfPhase::capture);
        try {
            running->capture_frame(std::filesystem::absolute(directory));capture_was_pending=true;
            if(performance.active()) performance.save(directory);
            settings.save(directory/"settings.cfg");
            pending_window_capture=running->capture_status().directory/"window.png";window_capture_error.clear();
            smbd::write_text(directory/"window.json",window_diagnostic(window.get(),renderer,settings,muted,bool(automation),frames,image.sha256,true,active_aspect),false);
            SDL_SetWindowTitle(window.get(),"Super Monkey Ball Deluxe | Capturing next complete frame...");
            smbd::diagnostic_record("{\"type\":\"frame_capture_requested\",\"directory\":"+smbd::json(smbd::path_text(running->capture_status().directory))+'}');
            ui.message("Capturing frame: "+smbd::path_text(directory));
        } catch(const std::exception& error) {
            running->cancel_capture(error.what());pending_window_capture.clear();
            window_capture_error=error.what();
            ui.message(window_capture_error);
            SDL_SetWindowTitle(window.get(),("Super Monkey Ball Deluxe | Capture failed: "+window_capture_error).c_str());
            smbd::diagnostic_record("{\"type\":\"frame_capture_request_error\",\"message\":"+smbd::json(error.what())+'}');
        }
    };
    const auto display_frame=[&] {
        bool displayed;
        {smbd::PerfScope scope(&performance,smbd::PerfPhase::display);displayed=renderer.display(true,!pending_window_capture.empty() && running->capture_status().complete);}
        if(displayed) {
            smbd::PerfScope scope(&performance,smbd::PerfPhase::ui);
            ui.draw(true,performance.active(),fps,[&]{
                const auto utc=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                request_capture(root/"build/captures"/std::format("frame-{}-{}",utc,SDL_GetTicksNS()));
            },[&]{if(performance.active()) stop_performance();else start_performance();});
        }
        if(!pending_window_capture.empty() && running->capture_status().complete) {
            try {
                if(!displayed) throw std::runtime_error("The game window has no drawable area");
                smbd::save_png(renderer.read_display(),pending_window_capture);
                const auto& status=running->capture_status();
                SDL_SetWindowTitle(window.get(),("Super Monkey Ball Deluxe | Capture saved"+std::string(status.truncated?" (size limit reached): ":": ")+smbd::path_text(status.directory)).c_str());
            } catch(const std::exception& error) {
                window_capture_error=error.what();
                SDL_SetWindowTitle(window.get(),("Super Monkey Ball Deluxe | Window capture failed: "+window_capture_error).c_str());
                smbd::diagnostic_record("{\"type\":\"window_capture_error\",\"message\":"+smbd::json(error.what())+'}');
                try {smbd::write_text(pending_window_capture.parent_path()/"window-error.json","{\"error\":"+smbd::json(error.what())+"}\n",false);} catch(...) {}
            }
            pending_window_capture.clear();
        }
        if(displayed) {
            smbd::PerfScope scope(&performance,smbd::PerfPhase::present);
            if(renderer.present(settings.vsync,false)) {
                last_present=smbd::FrameClock::Clock::now();if(!presented_frames) first_present=last_present;
                ++presented_frames;++fps_frames;
            }
        }
        const auto now=std::chrono::steady_clock::now();
        if(now-fps_started>=std::chrono::milliseconds(500)) {fps=fps_frames/std::chrono::duration<double>(now-fps_started).count();fps_frames=0;fps_started=now;}
        display_clock.advance(now);display_clock.skip_missed(now);
    };
    host.renderer=&renderer;host.input=&input;host.main_address=0x00014B90;host.capture=capture;
    host.trace_io=!report.empty();
    host.trace_storage=trace_storage;
    host.mute_audio=muted;
    host.storage=storage.empty()?root/"data/user":std::filesystem::absolute(storage);
    host.performance=&performance;
    host.widescreen=active_widescreen;
    host.ultrawide=active_aspect==2;
    host.refresh_rate=[&] {return presentation_rate(settings,ui);};
    host.poll=[&](std::uint64_t ticks) {
        logic_ticks=ticks;
        if(automation) automation->update(logic_ticks,memory);
        SDL_Event event;
        while(SDL_PollEvent(&event)) {
            ui.event(event);input.event(event);
            if(event.type==SDL_EVENT_KEY_DOWN && !event.key.repeat && event.key.scancode==SDL_SCANCODE_F8) {
                const auto utc_ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                request_capture(root/"build/captures"/std::format("frame-{}-{}",utc_ms,SDL_GetTicksNS()));
            }
            if(event.type==SDL_EVENT_KEY_DOWN && !event.key.repeat && event.key.scancode==SDL_SCANCODE_F9) {
                if(performance.active()) stop_performance();else start_performance();
            }
        }
        if(performance.expired()) stop_performance();
        input.capture(ui.visible);
        const auto gain=settings.output_gain((SDL_GetWindowFlags(window.get())&SDL_WINDOW_INPUT_FOCUS)!=0);
        if(gain!=output_gain) {running->output_gain(gain);output_gain=gain;}
        if(capture_was_pending && !running->capture_status().pending) {
            capture_was_pending=false;
            const auto& status=running->capture_status();
            if(!status.error.empty()) {
                pending_window_capture.clear();
                SDL_SetWindowTitle(window.get(),("Super Monkey Ball Deluxe | Capture failed: "+status.error).c_str());
            }
        }
        input.update();
        const auto now=std::chrono::steady_clock::now();
        if(frames && !pending_window_capture.empty() && running->capture_status().complete) display_frame();
        return !input.quit_requested && !ui.quit && !ui.restart && (!stop_after_input || !automation->finished()) &&
            (!benchmark_ticks || !automatic_performance_requested || performance.active()) &&
            (!frame_limit || frames<frame_limit) && now<deadline;
    };
    host.idle=[&](auto wake) {
        const auto now=std::chrono::steady_clock::now();
        // Keep polling events during loading, and keep display pacing out of
        // guest waits, timers, asynchronous I/O and the audio callback.
        wake=std::min(wake,now+std::chrono::milliseconds(8));
        if(wake>now) SDL_DelayPrecise(static_cast<Uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(wake-now).count()));
    };
    host.flip=[&] {
        {smbd::PerfScope scope(&performance,smbd::PerfPhase::publish);renderer.publish_frame();}
        last_source=smbd::FrameClock::Clock::now();if(!frames) first_source=last_source;
        ++frames;
        if(!motion_report.empty() && logic_ticks>=motion_at && motion_samples.size()<motion_frames) {
            const auto value=[&](std::uint32_t address) {return std::bit_cast<float>(memory.load<std::uint32_t>(address));};
            const auto ordinary=[](std::uint32_t address) {return address>=0x10000 && address<=0x03FFF000;};
            const auto ape=memory.load<std::uint32_t>(0x738EF4)&0x7FFFFFFFU;
            const auto animation=ordinary(ape)?memory.load<std::uint32_t>(ape+0x0C)&0x7FFFFFFFU:0;
            const auto sample_frame=ordinary(animation)?value(animation+0x4C):-1.0f;
            const auto sample_speed=ordinary(animation)?value(animation+0x48):-1.0f;
            const auto counters=running->performance_counters();
            motion_samples.push_back(std::format("{{\"frame\":{},\"ticks\":{},\"seconds\":{:.9f},\"delta\":{:.9g},\"scene\":{},\"ball_state\":{},\"ball\":[{:.9g},{:.9g},{:.9g}],\"matrix\":[{:.9g},{:.9g},{:.9g}],\"camera\":[{:.9g},{:.9g},{:.9g}],\"stage_time\":{:.9g},\"animation_frame\":{:.9g},\"animation_speed\":{:.9g},\"draws\":{}}}",
                frames,logic_ticks,std::chrono::duration<double>(last_source-first_source).count(),smbd::frame_step.delta,memory.load<std::uint16_t>(0x6C9288),memory.load<std::uint8_t>(0x738DF3),
                value(0x738DF4),value(0x738DF8),value(0x738DFC),value(0x738E2C),value(0x738E3C),value(0x738E4C),
                value(0x73F1A0),value(0x73F1A4),value(0x73F1A8),value(0x72FF84),std::isfinite(sample_frame)?sample_frame:-1,std::isfinite(sample_speed)?sample_speed:-1,counters[0]));
        }
        const auto rate=presentation_rate(settings,ui);
        if(!rate || rate>=60 || display_clock.due(rate,last_source)) display_frame();
        if(performance.active()) {
            performance.source_frame(frames,running->performance_counters());renderer.performance_frame(frames+1);renderer.performance_resources();
            if(benchmark_ticks) {
                const auto mode=memory.load<std::uint32_t>(0x6C9354);
                const auto ball=memory.load<std::uint8_t>(0x738DF3);
                benchmark_mode_changes+=mode!=benchmark_mode;benchmark_mode=mode;
                benchmark_ball_changes+=ball!=benchmark_ball;benchmark_ball=ball;
                benchmark_complete=logic_ticks-benchmark_first_tick>=benchmark_ticks;
                if(benchmark_complete) stop_performance();
            } else if(!performance_output.empty() && frames-performance_first>=performance_frames) stop_performance();
        }
        if(!performance_output.empty() && !automatic_performance_requested &&
           (benchmark_ticks?automation->finished():logic_ticks>=performance_at)) {
            automatic_performance_requested=true;start_performance();
        }
        if(!frame_capture.empty() && !automatic_capture_requested && logic_ticks>=capture_at) {
            automatic_capture_requested=true;request_capture(frame_capture);
        }
    };
    host.frames=[&] {return frames;};
    host.presented_frames=[&] {return presented_frames;};
    smbd::Xbox xbox(image,memory,ram,settings.disc_path(),std::move(host));
    running=&xbox;
    if(!call_trace.empty()) smbd::enable_call_trace();
    const auto result=xbox.run(0);
    stop_performance();
    if(!motion_report.empty()) {
        std::string samples;
        for(const auto& sample:motion_samples) {if(!samples.empty()) samples+=',';samples+=sample;}
        smbd::write_text(motion_report,std::format("{{\"format\":\"smbd-motion-report-v1\",\"source_sha256\":{},\"refresh_rate\":{},\"interpolation\":false,\"samples\":[{}]}}\n",smbd::json(image.sha256),settings.refresh_rate,samples),false);
    }
    if(ui.restart) return 4;
    smbd::diagnostic_record("{\"type\":\"run_end\",\"boundary\":"+smbd::json(result.boundary)+
        ",\"guest_eip\":"+smbd::json(smbd::hex32(result.cpu.eip))+",\"frames\":"+std::to_string(result.frames)+
        ",\"draws\":"+std::to_string(result.draws)+'}');
    if(!call_trace.empty()) {
        std::ofstream output(call_trace,std::ios::binary|std::ios::trunc);
        output.exceptions(std::ios::failbit|std::ios::badbit);
        output<<smbd::call_trace_report(image.sha256)<<'\n';
    }
    if(!report.empty()) {
        std::ofstream output(report,std::ios::binary|std::ios::trunc);
        output.exceptions(std::ios::failbit|std::ios::badbit);
        output<<smbd::boot_report(image,result)<<'\n';
    }
    if(result.boundary!="Native window closed" && !automated) {
        running->output_gain(0);ui.visible=true;ui.category=4;ui.message("Game stopped: "+result.boundary);
        const auto utc=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        const auto failure=root/"build/logs"/std::format("game-stopped-{}.json",utc);
        smbd::write_text(failure,smbd::boot_report(image,result)+'\n',false);ui.message("Details saved: "+smbd::path_text(failure));
        while(!ui.quit) {
            SDL_Event event;while(SDL_PollEvent(&event)) {
                ui.event(event);
                if(event.type==SDL_EVENT_KEY_DOWN && !event.key.repeat && event.key.scancode==SDL_SCANCODE_F8) capture_launcher();
            }
            if(!renderer.display()) {SDL_Delay(10);continue;}
            ui.draw(false,false,0,capture_launcher,[]{});
            if(!launcher_capture.empty()) {
                smbd::save_png(renderer.read_display(),launcher_capture/"window.png");settings.save(launcher_capture/"settings.cfg");
                smbd::write_text(launcher_capture/"window.json",window_diagnostic(window.get(),renderer,settings,muted,false,frames,image.sha256,false,active_aspect),false);
                ui.message("Frame captured: "+smbd::path_text(launcher_capture));launcher_capture.clear();
            }
            pace_launcher();renderer.present(settings.vsync);
            if(ui.launch || ui.restart) return 4;
        }
        return 0;
    }
    std::cout << "{\"boundary\":" << smbd::json(result.boundary) << ",\"eip\":" << smbd::json(smbd::hex32(result.cpu.eip))
              << ",\"frames\":" << result.frames << ",\"draws\":" << result.draws
              << ",\"presented_frames\":" << presented_frames << ",\"logic_hz\":60,\"native_scene_steps\":true,\"interpolation\":false,\"refresh_rate\":" << settings.refresh_rate
              << ",\"source_seconds\":" << std::chrono::duration<double>(last_source-first_source).count()
              << ",\"presentation_seconds\":" << std::chrono::duration<double>(last_present-first_present).count()
              << ",\"simulation_steps\":" << result.simulation_steps << ",\"simulation_ms\":" << result.simulation_ms
              << ",\"timing_epochs\":" << result.timing_epochs
              << ",\"controller_steps\":" << (automation?automation->applied():0)
              << ",\"controller_waits\":" << (automation?automation->waits_applied():0)
              << ",\"controller_complete\":" << (!automation || automation->finished()?"true":"false")
              << ",\"audio_muted\":" << (muted?"true":"false")
              << ",\"capture_written\":" << (result.capture_written?"true":"false") << ",\"capture_error\":" << smbd::json(result.capture_error)
              << ",\"frame_capture\":{\"directory\":" << smbd::json(smbd::path_text(xbox.capture_status().directory))
              << ",\"complete\":" << (xbox.capture_status().complete?"true":"false") << ",\"truncated\":" << (xbox.capture_status().truncated?"true":"false")
              << ",\"error\":" << smbd::json(xbox.capture_status().error) << ",\"window_error\":" << smbd::json(window_capture_error) << "}}\n";
    return result.boundary=="Native window closed" && result.capture_error.empty() && performance_error.empty() &&
        (!benchmark_ticks || benchmark_complete) &&
        (motion_report.empty() || motion_samples.size()==motion_frames) &&
        (performance_output.empty() || !last_performance.empty()) &&
        (!stop_after_input || automation->finished()) &&
        (frame_capture.empty() || (automatic_capture_requested && xbox.capture_status().directory==std::filesystem::absolute(frame_capture) &&
            xbox.capture_status().complete && window_capture_error.empty()))?0:3;
}
}
int wmain(int argc,wchar_t** argv) {
    smbd::initialize_diagnostics();
    try {bool restarting=false;for(;;) {const int result=run(argc,argv,restarting);if(result!=4) return result;restarting=true;}}
    catch(const std::exception& error) {
        smbd::diagnostic_record("{\"type\":\"error\",\"message\":"+smbd::json(error.what())+'}');
        std::cerr << error.what() << '\n';return 1;
    }
}
