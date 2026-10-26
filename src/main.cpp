#include "xbox.h"
#include "render.h"
#include "input.h"
#include "automation.h"
#include <SDL3/SDL.h>
#include <charconv>
#include <chrono>
#include <fstream>
#include <iostream>

namespace {
struct Sdl {
    Sdl() {if(!SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMEPAD)) throw std::runtime_error(SDL_GetError());}
    ~Sdl() {SDL_Quit();}
};
int run(int argc,wchar_t** argv) {
    std::uint64_t frame_limit=0,seconds=0;
    std::filesystem::path capture,timeline,report;
    bool hidden=false;
    for(int i=1;i<argc;++i) {
        const auto option=smbd::utf8(argv[i]);
        if(option=="--hidden") hidden=true;
        else if(option=="--capture" && i+1<argc) capture=argv[++i];
        else if(option=="--input" && i+1<argc) timeline=argv[++i];
        else if(option=="--report" && i+1<argc) report=argv[++i];
        else if((option=="--frames" || option=="--seconds") && i+1<argc) {
            const auto value=smbd::utf8(argv[++i]);
            auto& limit=option=="--frames"?frame_limit:seconds;
            const auto parsed=std::from_chars(value.data(),value.data()+value.size(),limit);
            if(parsed.ec!=std::errc{} || parsed.ptr!=value.data()+value.size() || !limit || (option=="--seconds" && limit>3600))
                throw std::runtime_error("Limits must be positive; seconds must not exceed 3600");
        } else throw std::runtime_error("Usage: smbd [--hidden] [--frames COUNT] [--seconds COUNT] [--input TIMELINE.txt] [--capture FRAME.png] [--report OUTPUT.json]");
    }
    smbd::Xbe image("data/local/default.xbe");
    std::vector<std::byte> ram(64*1024*1024);
    image.load(std::span(ram).subspan(image.base,image.image_size));
    smbd::Memory memory(0,ram);
    if(!timeline.empty() && !SDL_SetHintWithPriority(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,"1",SDL_HINT_OVERRIDE))
        throw std::runtime_error("Cannot enable background controller input for automation");
    Sdl sdl;
    const auto flags=SDL_WINDOW_RESIZABLE|(hidden?SDL_WINDOW_HIDDEN:0);
    std::unique_ptr<SDL_Window,decltype(&SDL_DestroyWindow)> window(
        SDL_CreateWindow("Super Monkey Ball Deluxe",960,720,flags),SDL_DestroyWindow);
    if(!window) throw std::runtime_error(SDL_GetError());
    smbd::Renderer renderer(640,480);
    renderer.attach_window(SDL_GetPointerProperty(SDL_GetWindowProperties(window.get()),SDL_PROP_WINDOW_WIN32_HWND_POINTER,nullptr));
    std::unique_ptr<smbd::Automation> automation;
    if(!timeline.empty()) automation=std::make_unique<smbd::Automation>(timeline);
    smbd::Input input(!automation,false,automation?automation->device():0);
    std::uint64_t frames=0;
    const auto deadline=seconds?std::chrono::steady_clock::now()+std::chrono::seconds(seconds):std::chrono::steady_clock::time_point::max();
    smbd::XboxHost host;
    host.renderer=&renderer;host.input=&input;host.main_address=0x00014B90;host.capture=capture;
    host.trace_io=!report.empty();
    host.poll=[&] {
        if(automation) automation->update(frames);
        SDL_Event event;
        while(SDL_PollEvent(&event)) input.event(event);
        input.update();
        return !input.quit_requested && (!frame_limit || frames<frame_limit) && std::chrono::steady_clock::now()<deadline;
    };
    host.flip=[&] {renderer.display();renderer.present(false);++frames;};
    host.frames=[&] {return frames;};
    smbd::Xbox xbox(image,memory,ram,"Super Monkey Ball Deluxe/Super Monkey Ball Deluxe (USA).xiso.iso",std::move(host));
    const auto result=xbox.run(0);
    if(!report.empty()) {
        std::ofstream output(report,std::ios::binary|std::ios::trunc);
        output.exceptions(std::ios::failbit|std::ios::badbit);
        output<<smbd::boot_report(image,result)<<'\n';
    }
    std::cout << "{\"boundary\":" << smbd::json(result.boundary) << ",\"eip\":" << smbd::json(smbd::hex32(result.cpu.eip))
              << ",\"frames\":" << result.frames << ",\"draws\":" << result.draws
              << ",\"controller_steps\":" << (automation?automation->applied():0)
              << ",\"capture_written\":" << (result.capture_written?"true":"false") << ",\"capture_error\":" << smbd::json(result.capture_error) << "}\n";
    return result.boundary=="Native window closed" && result.capture_error.empty()?0:3;
}
}
int wmain(int argc,wchar_t** argv) {
    try {return run(argc,argv);}
    catch(const std::exception& error) {std::cerr << error.what() << '\n';return 1;}
}
