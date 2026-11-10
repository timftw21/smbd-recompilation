#include "settings.h"
#include "discord.h"
#include "render.h"
#include "xbe.h"
#include "disc.h"
#include "diagnostics.h"
#include "xbox.h"
#include "ui_theme.h"
#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_dx11.h>
#ifdef _WIN32
#include <Windows.h>
#include <commdlg.h>
#else
#include <mutex>
#endif
#include <d3d11.h>
#include <algorithm>
#include <charconv>
#include <bit>
#include <cmath>
#include <format>
#include <fstream>

namespace smbd {
Settings::Settings(std::filesystem::path base):root(std::move(base)) {
    copy_text(executable.data(),executable.size(),"data/local/default.xbe");
    copy_text(disc.data(),disc.size(),"Super Monkey Ball Deluxe/Super Monkey Ball Deluxe (USA).xiso.iso");
}
void Settings::load(const std::filesystem::path& path) {
    if(!std::filesystem::exists(path)) return;
    if(std::filesystem::file_size(path)>16384) throw std::runtime_error("Settings file exceeds 16 KiB");
    std::ifstream input(path);std::string line;
    while(std::getline(input,line)) {
        if(line.empty() || line[0]=='#') continue;
        if(line.back()=='\r') line.pop_back();const auto split=line.find('=');
        if(split==std::string::npos) continue;
        const auto name=line.substr(0,split),value=line.substr(split+1);
        if(name=="executable") {copy_text(executable.data(),executable.size(),value);continue;}
        if(name=="disc") {copy_text(disc.data(),disc.size(),value);continue;}
        int number=0;const auto parsed=std::from_chars(value.data(),value.data()+value.size(),number);
        if(parsed.ec!=std::errc{} || parsed.ptr!=value.data()+value.size()) continue;
        if(name=="show_fps") show_fps=number!=0;
        else if(name=="muted") muted=number!=0;
        else if(name=="mute_on_focus_loss") mute_on_focus_loss=number!=0;
        else if(name=="vsync") vsync=number!=0;
        else if(name=="fullscreen") fullscreen=number!=0;
        else if(name=="unlimited_render_distance") unlimited_render_distance=number!=0;
        else if(name=="discord_presence") discord_presence=number!=0;
        else if(name=="volume") volume=std::clamp(number,0,100);
        else if(name=="aspect") aspect=std::clamp(number,0,2);
        else if(name=="resolution") resolution=std::clamp(number,1,4);
        else if(name=="refresh_rate" && (!number || (number>=30 && number<=1000))) refresh_rate=number;
        else if(name=="antialiasing" && number>=0 && number<=6) antialiasing=number;
        else if(name=="anisotropy" && number>=0 && number<=16 && (!number || std::has_single_bit(unsigned(number)))) anisotropy=number;
        else for(unsigned i=0;i<control_actions.size();++i) {
            if(name=="key_"+std::string(control_actions[i].id) && valid_key_binding(number)) bindings[i].keyboard=number;
            else if(name=="pad_"+std::string(control_actions[i].id) && valid_pad_binding(number)) bindings[i].gamepad=number;
        }
    }
}
void Settings::save(const std::filesystem::path& path) const {
    auto text=std::format("executable={}\ndisc={}\nshow_fps={}\nvolume={}\nmuted={}\nmute_on_focus_loss={}\naspect={}\nvsync={}\nfullscreen={}\nresolution={}\nrefresh_rate={}\nantialiasing={}\nanisotropy={}\nunlimited_render_distance={}\ndiscord_presence={}\n",
        executable.data(),disc.data(),int(show_fps),volume,int(muted),int(mute_on_focus_loss),aspect,int(vsync),int(fullscreen),resolution,refresh_rate,antialiasing,anisotropy,int(unlimited_render_distance),int(discord_presence));
    for(unsigned i=0;i<control_actions.size();++i) text+=std::format("key_{}={}\npad_{}={}\n",control_actions[i].id,bindings[i].keyboard,control_actions[i].id,bindings[i].gamepad);
    if(text.find('\r')!=std::string::npos || std::string_view(executable.data()).find('\n')!=std::string_view::npos ||
       std::string_view(disc.data()).find('\n')!=std::string_view::npos) throw std::runtime_error("Game paths cannot contain line breaks");
    if(path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    auto temporary=path;temporary+=".tmp";write_text(temporary,text,true);
#ifdef _WIN32
    if(!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Cannot save settings");
#else
    std::filesystem::rename(temporary,path);
#endif
}
void Settings::validate() const {
    Xbe image(executable_path());
    if(!image.supported()) throw std::runtime_error("Select the supported US Xbox release of Super Monkey Ball Deluxe");
    Disc source(disc_path());const auto entry=source.find("default.xbe");
    if(source.sha256(entry)!=image.sha256) throw std::runtime_error("The disc image and game executable do not match");
}
float Settings::output_gain(bool focused) const {return muted || (mute_on_focus_loss && !focused)?0.0f:volume/100.0f;}
float Settings::display_aspect() const {return aspect==2?21.0f/9:aspect==1?16.0f/9:4.0f/3;}
std::filesystem::path Settings::executable_path() const {return root/utf8_path(executable.data());}
std::filesystem::path Settings::disc_path() const {return root/utf8_path(disc.data());}
std::string check_presentation(const std::filesystem::path& directory,const std::filesystem::path& xbe,const std::filesystem::path& disc) {
    if(std::filesystem::exists(directory)) throw std::runtime_error("Use a fresh presentation diagnostic directory");
    std::filesystem::create_directories(directory);
    unsigned checks=0;
    const auto require=[&](bool condition) {++checks;if(!condition) throw std::runtime_error(std::format("Completed-frame presentation check {} failed",checks));};
    Settings settings;require(settings.refresh_rate==60 && !settings.unlimited_render_distance);
    for(bool unlimited:{true,false}) {
        settings.unlimited_render_distance=unlimited;settings.save(directory/"settings.cfg");
        Settings restored;restored.load(directory/"settings.cfg");require(restored.unlimited_render_distance==unlimited);
    }
    for(int rate:{0,30,144,360,1000}) {
        settings.refresh_rate=rate;settings.save(directory/"settings.cfg");
        Settings restored;restored.load(directory/"settings.cfg");require(restored.refresh_rate==rate);
    }
    for(int quality=0;quality<=6;++quality) for(int filter:{0,1,2,4,8,16}) {
        settings.antialiasing=quality;settings.anisotropy=filter;settings.save(directory/"settings.cfg");
        Settings restored;restored.load(directory/"settings.cfg");require(restored.antialiasing==quality && restored.anisotropy==filter);
    }
    write_text(directory/"invalid.cfg","refresh_rate=1\nrefresh_rate=-1\nrefresh_rate=1001\nantialiasing=7\nanisotropy=3\n",false);
    Settings invalid;invalid.load(directory/"invalid.cfg");require(invalid.refresh_rate==60 && invalid.antialiasing==0 && invalid.anisotropy==0);
    settings.bindings[8].keyboard=SDL_SCANCODE_J;settings.bindings[8].gamepad=SDL_GAMEPAD_BUTTON_NORTH+1;
    settings.bindings[17].gamepad=pad_axis_binding(SDL_GAMEPAD_AXIS_RIGHTX,false);settings.bindings[9]={};settings.save(directory/"settings.cfg");
    Settings restored;restored.load(directory/"settings.cfg");require(restored.bindings==settings.bindings);
    write_text(directory/"invalid-bindings.cfg","key_a="+std::to_string(SDL_SCANCODE_F8)+"\npad_a=-999999\nkey_b=-1\npad_b=99999\n",false);
    invalid.load(directory/"invalid-bindings.cfg");require(invalid.bindings==default_bindings());
    if(!SDL_InitSubSystem(SDL_INIT_VIDEO)) throw std::runtime_error(SDL_GetError());
    struct Video {~Video(){SDL_QuitSubSystem(SDL_INIT_VIDEO);}} video;
    SDL_WindowFlags flags=SDL_WINDOW_HIDDEN;
#ifndef _WIN32
    flags|=SDL_WINDOW_VULKAN;
#endif
    std::unique_ptr<SDL_Window,decltype(&SDL_DestroyWindow)> window(SDL_CreateWindow("Presentation diagnostic",96,72,flags),SDL_DestroyWindow);
    if(!window) throw std::runtime_error(SDL_GetError());
    std::string adapter;
    for(unsigned scale:{1U,4U}) {
        Renderer renderer(32,24,false,Renderer::debug_layer_available,scale);adapter=renderer.adapter();
        renderer.attach_window(window.get());
        const auto auxiliary=renderer.create_target(32,24);
        for(int aspect=0;aspect<3;++aspect) {
            settings.aspect=aspect;renderer.aspect_ratio(settings.display_aspect());
            renderer.set_target(0);renderer.clear(0xFF204060);renderer.publish_frame();
            require(renderer.display());const auto expected=renderer.read_display();renderer.present(false);
            renderer.clear(0xFFCC8844);const auto changed=renderer.readback();
            renderer.set_target(auxiliary);renderer.clear(0xFF00FF00);const auto offscreen=renderer.readback();
            for(unsigned frame=0;frame<6;++frame) {
                require(renderer.display(true));const auto displayed=renderer.read_display();
                require(displayed.rgba==expected.rgba);renderer.present(false);
            }
            require(renderer.readback().rgba==offscreen.rgba);
            renderer.set_target(0);require(renderer.readback().rgba==changed.rgba);
            renderer.publish_frame();require(renderer.display(true));const auto next=renderer.read_display();renderer.present(false);
            require(next.rgba!=expected.rgba);
            save_png(expected,directory/std::format("complete-{}x-aspect-{}.png",scale,aspect));
        }
        // Different colours at each mip make live LOD changes observable,
        // including restoring a pipeline prepared before the option changed.
        const std::array<std::uint32_t,5> mip_pixels{0xFFFF0000,0xFFFF0000,0xFFFF0000,0xFFFF0000,0xFF0000FF};
        const auto texture=renderer.upload({2,2,TextureFormat::argb8,std::as_bytes(std::span(mip_pixels)),2});
        RenderState state;state.textures[0]=texture;state.texture_modes[0]=1;
        state.texture_filter[0]=0x02060000;state.texture_control[0]=0x04004000;
        state.final_inputs={0x00000008,0x00001800};
        std::array<Vertex,6> quad;
        constexpr Float4 positions[]={{-1,-1,0,1},{1,-1,0,1},{1,1,0,1},{-1,-1,0,1},{1,1,0,1},{-1,1,0,1}};
        for(unsigned i=0;i<quad.size();++i) {quad[i].position=positions[i];quad[i].uv[0]={.5f,.5f,0,1};}
        for(bool unlimited:{false,true,false}) {
            renderer.image_quality(scale,0,0,unlimited);renderer.set_target(0);renderer.prepare(state);
            renderer.begin();renderer.draw(state,quad);renderer.end();const auto image=renderer.readback();
            require(renderer.stats().unlimited_render_distance==unlimited);
            require(image.rgba[0]==std::byte(unlimited?255:0) && image.rgba[2]==std::byte(unlimited?0:255));
        }
        renderer.release_texture(texture);
        require(SDL_SetWindowSize(window.get(),120,90) && SDL_SyncWindow(window.get()));
        require(renderer.display(true));const auto resized=renderer.read_display();renderer.present(false);
        require(resized.width==120 && resized.height==90);
        renderer.resize(40,30);require(!renderer.display(true));
        renderer.clear(0xFF204060);renderer.publish_frame();require(renderer.display(true));renderer.present(false);
        require(renderer.errors().empty());
        require(SDL_SetWindowSize(window.get(),5120,72) && SDL_SyncWindow(window.get()));
        require(renderer.display(true));const auto wide=renderer.read_display();renderer.present(false);
        require(wide.width==5120 && wide.height==72 && renderer.errors().empty());
        require(SDL_SetWindowSize(window.get(),96,72) && SDL_SyncWindow(window.get()));
    }
    unsigned live_changes=0;
    if(!xbe.empty()) {
        Xbe image(xbe);if(!image.supported()) throw std::runtime_error("Live presentation checks require the verified game executable");
        std::vector<std::byte> ram(64*1024*1024);
        image.load(std::span(ram).subspan(image.base,image.image_size));Memory memory(0,ram);
        Renderer renderer(640,480,false,Renderer::debug_layer_available);
        renderer.attach_window(window.get());
        Xbox* running=nullptr;std::uint64_t frames=0;
        const std::array<std::array<unsigned,4>,7> options{{{2,1,16,2},{1,4,2,0},{2,5,4,1},{1,6,8,2},{4,0,1,0},{1,3,0,1},{1,0,0,2}}};
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
        XboxHost host;host.renderer=&renderer;host.main_address=0x14B90;host.storage=directory/"storage";host.mute_audio=true;
        host.refresh_rate=[] {return 60U;};host.widescreen=true;
        host.poll=[&](std::uint64_t) {return frames<(options.size()+1)*24 && std::chrono::steady_clock::now()<deadline;};
        host.frames=[&] {return frames;};
        host.flip=[&] {
            ++frames;
            if(frames%24==0 && frames/24<=options.size()) {
                const auto& option=options[frames/24-1];
                if(option[1]<4 || renderer.supports_msaa(1U<<(option[1]-3))) {
                    const bool unlimited=(frames/24)%2!=0;
                    renderer.image_quality(option[0],option[1],option[2],unlimited);running->aspect_ratio(option[3]);
                    running->unlimited_render_distance(unlimited);
                    renderer.aspect_ratio(option[3]==2?21.0f/9:option[3]==1?16.0f/9:4.0f/3);
                    const auto stats=renderer.stats();
                    require(stats.resolution_scale==option[0] && stats.antialiasing==option[1] && stats.anisotropy==option[2]);
                    require(stats.unlimited_render_distance==unlimited);
                    require(memory.load<std::uint8_t>(0x6C934C)==(option[3]!=0));
                    require(memory.load<std::uint32_t>(0x402428)==(option[3]==2?std::bit_cast<std::uint32_t>(21.0f/9):0x3FE38E39U));
                    const auto layout=std::bit_cast<std::int8_t>(memory.load<std::uint8_t>(0x73F190));
                    if(!option[3] || (layout>=0 && layout<=3)) {
                        const auto aspect=option[3] && layout!=3?(option[3]==2?21.0f/9:16.0f/9):4.0f/3;
                        require(memory.load<std::uint32_t>(0x73F0EC)==std::bit_cast<std::uint32_t>(aspect));
                        for(unsigned i=0;i<5;++i) require(memory.load<std::uint32_t>(0x73F1D4+i*0x28C)==std::bit_cast<std::uint32_t>(aspect));
                    }
                    ++live_changes;
                }
            }
            renderer.publish_frame();
            require(renderer.display(true));renderer.present(false);
        };
        Xbox xbox(image,memory,ram,disc,std::move(host));running=&xbox;
        const auto result=xbox.run(0);
        write_text(directory/"live-boot.json",boot_report(image,result)+'\n',false);
        require(result.boundary=="Native window closed" && frames>options.size()*24 && live_changes>=4);
        require(renderer.errors().empty());save_png(renderer.readback(),directory/"live-final.png");
    }
    const auto report=std::format("{{\"format\":\"smbd-presentation-check-v1\",\"passed\":true,\"checks\":{},\"adapter\":{},\"hidden_window\":true,\"game_booted\":{},\"audio_output_muted\":true,\"live_graphics_changes\":{},\"scales\":[1,4],\"aspect_ratios\":[\"4:3\",\"16:9\",\"21:9\"]}}",checks,json(adapter),!xbe.empty(),live_changes);
    write_text(directory/"report.json",report+'\n',false);return report;
}
namespace {
void help(const char* text) {
    if(ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {ImGui::BeginTooltip();ImGui::PushTextWrapPos(370);ImGui::TextUnformatted(text);ImGui::PopTextWrapPos();ImGui::EndTooltip();}
}
void label(const char* text) {ImGui::Spacing();ImGui::TextUnformatted(text);ImGui::SetNextItemWidth(-1);}
Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> ui_texture(Renderer& renderer,const Image& image) {
    D3D11_TEXTURE2D_DESC description{};description.Width=image.width;description.Height=image.height;
    description.MipLevels=description.ArraySize=description.SampleDesc.Count=1;
    description.Format=DXGI_FORMAT_R8G8B8A8_UNORM;description.Usage=D3D11_USAGE_IMMUTABLE;description.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA initial{image.rgba.data(),image.width*4};
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
    if(FAILED(renderer.native_device()->CreateTexture2D(&description,&initial,&texture)) ||
       FAILED(renderer.native_device()->CreateShaderResourceView(texture.Get(),nullptr,&view))) throw std::runtime_error("Cannot create the settings artwork");
    return view;
}
Image wood_grain() {
    Image image{512,256,std::vector<std::byte>(512*256*4)};
    for(unsigned y=0;y<image.height;++y) for(unsigned x=0;x<image.width;++x) {
        const auto color=theme::wood_pixel(x,y);
        auto* pixel=image.rgba.data()+(y*image.width+x)*4;
        for(unsigned i=0;i<4;++i) pixel[i]=static_cast<std::byte>(color[i]);
    }
    return image;
}
void wooden_panel(ID3D11ShaderResourceView* wood,bool inset) {
    const auto pos=ImGui::GetWindowPos(),size=ImGui::GetWindowSize();const ImVec2 end{pos.x+size.x,pos.y+size.y};
    auto* draw=ImGui::GetWindowDrawList();const float radius=inset?8.0f:12.0f;
    draw->PushClipRect({pos.x-5,pos.y-5},{end.x+8,end.y+8},false);
    draw->AddRectFilled({pos.x+3,pos.y+4},{end.x+3,end.y+4},IM_COL32(12,7,2,150),radius);
    draw->AddImageRounded(reinterpret_cast<ImTextureID>(wood),pos,end,{0,0},{1,1},inset?IM_COL32(153,139,118,255):IM_COL32_WHITE,radius);
    draw->AddRect(pos,end,IM_COL32(183,129,51,255),radius,2.0f);
    draw->AddRect({pos.x+3,pos.y+3},{end.x-3,end.y-3},IM_COL32(61,36,13,255),radius-2,2.0f);
    draw->AddLine({pos.x+8,pos.y+2},{end.x-8,pos.y+2},IM_COL32(236,188,100,255));
    draw->PopClipRect();
}
}
SettingsUi::SettingsUi(SDL_Window* window,Renderer& renderer,Settings& settings,std::filesystem::path path,DiscordPresence* presence):
    window_(window),renderer_(renderer),settings_(settings),presence_(presence),path_(std::move(path)) {
    refresh_options();
    IMGUI_CHECKVERSION();ImGui::CreateContext();auto& io=ImGui::GetIO();
    io.ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard|ImGuiConfigFlags_NavEnableGamepad;
    io.IniFilename=nullptr;io.LogFilename=nullptr;
#ifdef _WIN32
    std::array<wchar_t,MAX_PATH> windows{};
    const auto length=GetWindowsDirectoryW(windows.data(),static_cast<UINT>(windows.size()));
    if(length && length<windows.size()) {
        const auto fonts=std::filesystem::path(windows.data())/"Fonts";
        const auto regular=fonts/"segoeui.ttf",italic=fonts/"segoeuii.ttf";
        if(std::filesystem::exists(regular)) io.Fonts->AddFontFromFileTTF(path_text(regular).c_str(),18);
        else io.Fonts->AddFontDefault();
        if(std::filesystem::exists(italic)) italic_font_=io.Fonts->AddFontFromFileTTF(path_text(italic).c_str(),18);
    }
#else
    io.Fonts->AddFontDefault();
#endif
    auto& style=ImGui::GetStyle();style.FontSizeBase=18;style.WindowPadding={24,20};style.FramePadding={12,8};style.ItemSpacing={12,10};
    style.WindowRounding=12;style.ChildRounding=9;style.FrameRounding=5;style.GrabRounding=5;style.WindowBorderSize=0;
    ImGui::StyleColorsDark();auto* c=style.Colors;
    c[ImGuiCol_WindowBg]={.19f,.105f,.04f,.98f};c[ImGuiCol_ChildBg]={.17f,.09f,.035f,1};c[ImGuiCol_PopupBg]={.20f,.115f,.045f,1};
    c[ImGuiCol_FrameBg]={.22f,.13f,.055f,1};c[ImGuiCol_FrameBgHovered]={.34f,.21f,.08f,1};c[ImGuiCol_FrameBgActive]={.43f,.28f,.10f,1};
    c[ImGuiCol_Text]={1,.95f,.79f,1};c[ImGuiCol_TextDisabled]={.76f,.65f,.46f,1};
    c[ImGuiCol_Button]={.40f,.24f,.08f,1};c[ImGuiCol_ButtonHovered]={.58f,.37f,.12f,1};c[ImGuiCol_ButtonActive]={.69f,.46f,.16f,1};
    c[ImGuiCol_Header]={.53f,.33f,.09f,1};c[ImGuiCol_HeaderHovered]={.64f,.43f,.14f,1};c[ImGuiCol_HeaderActive]={.72f,.51f,.19f,1};
    c[ImGuiCol_CheckMark]={1,.83f,.12f,1};c[ImGuiCol_SliderGrab]={1,.78f,.08f,1};c[ImGuiCol_SliderGrabActive]={1,.9f,.3f,1};
    c[ImGuiCol_Separator]={.66f,.44f,.16f,1};c[ImGuiCol_Border]={.49f,.31f,.11f,1};c[ImGuiCol_NavCursor]={1,.85f,.15f,1};
    c[ImGuiCol_ScrollbarBg]={.12f,.065f,.02f,1};c[ImGuiCol_ScrollbarGrab]={.48f,.31f,.12f,1};
    c[ImGuiCol_ScrollbarGrabHovered]={.66f,.45f,.18f,1};c[ImGuiCol_ScrollbarGrabActive]={.79f,.57f,.23f,1};
    c[ImGuiCol_TableHeaderBg]={.25f,.15f,.06f,1};c[ImGuiCol_TableBorderStrong]={.50f,.33f,.13f,1};c[ImGuiCol_TableBorderLight]={.38f,.25f,.10f,1};
#ifdef _WIN32
    const bool platform_ready=ImGui_ImplSDL3_InitForD3D(window);
#else
    const bool platform_ready=ImGui_ImplSDL3_InitForVulkan(window);
#endif
    if(!platform_ready || !ImGui_ImplDX11_Init(renderer.native_device(),renderer.native_context()))
        throw std::runtime_error("Cannot initialize the settings menu");
    wood_=ui_texture(renderer,wood_grain());
    try {
        const auto icon=load_image(settings_.root/"resources/smbd.png");
        icon_=ui_texture(renderer,icon);
        icon_aspect_=float(icon.width)/icon.height;
        auto* surface=SDL_CreateSurfaceFrom(static_cast<int>(icon.width),static_cast<int>(icon.height),SDL_PIXELFORMAT_RGBA32,
            const_cast<std::byte*>(icon.rgba.data()),static_cast<int>(icon.width*4));
        if(surface) {SDL_SetWindowIcon(window,surface);SDL_DestroySurface(surface);}
    } catch(const std::exception& error) {message(error.what());}
}
SettingsUi::~SettingsUi() {show_cursor();ImGui_ImplDX11_Shutdown();ImGui_ImplSDL3_Shutdown();ImGui::DestroyContext();}
void SettingsUi::show_cursor() {
    mouse_activity_=SDL_GetTicks();
    if(cursor_hidden_) {
        ImGui::GetIO().ConfigFlags&=~ImGuiConfigFlags_NoMouseCursorChange;
        SDL_ShowCursor();cursor_hidden_=false;
    }
}
void SettingsUi::refresh_options() {
    refresh_rates_={0,30,60,75,90,120,144,165,180,240,360};
    const auto display=SDL_GetDisplayForWindow(window_);
    const auto* current=SDL_GetCurrentDisplayMode(display);
    display_refresh_=current?current->refresh_rate:0;
    int count=0;auto** modes=SDL_GetFullscreenDisplayModes(display,&count);
    if(modes) {
        for(int i=0;i<count;++i) {
            const auto rate=static_cast<int>(std::round(modes[i]->refresh_rate));
            if(rate>=30 && rate<=1000) refresh_rates_.push_back(rate);
        }
        SDL_free(modes);
    }
    refresh_rates_.push_back(settings_.refresh_rate);
    std::ranges::sort(refresh_rates_);
    refresh_rates_.erase(std::unique(refresh_rates_.begin(),refresh_rates_.end()),refresh_rates_.end());
}
void SettingsUi::rebind(unsigned action,bool controller) {
    if(action>=control_actions.size()) return;
    binding_action_=int(action);binding_controller_=controller;category=5;
}
bool SettingsUi::event(const SDL_Event& event) {
    switch(event.type) {
    case SDL_EVENT_MOUSE_MOTION:case SDL_EVENT_MOUSE_BUTTON_DOWN:case SDL_EVENT_MOUSE_BUTTON_UP:case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_WINDOW_MOUSE_ENTER:case SDL_EVENT_WINDOW_MOUSE_LEAVE:case SDL_EVENT_WINDOW_FOCUS_GAINED:case SDL_EVENT_WINDOW_FOCUS_LOST:
    case SDL_EVENT_WINDOW_HIDDEN:case SDL_EVENT_WINDOW_MINIMIZED:show_cursor();break;
    }
    if(event.type==SDL_EVENT_GAMEPAD_REMOVED && event.gdevice.which==binding_device_) binding_release_=0;
    if(binding_release_ && ((event.type==SDL_EVENT_GAMEPAD_BUTTON_UP && event.gbutton.which==binding_device_ && int(event.gbutton.button)+1==binding_release_) ||
        (event.type==SDL_EVENT_GAMEPAD_AXIS_MOTION && event.gaxis.which==binding_device_ && binding_release_<0 && int(event.gaxis.axis)==(-binding_release_-1)/2 && std::abs(int(event.gaxis.value))<8000))) {
        binding_release_=0;return true;
    }
    if(active() && binding_action_>=0) {
        int code=0;bool captured=false,cancel=false;
        if(event.type==SDL_EVENT_KEY_DOWN) {
            cancel=event.key.scancode==SDL_SCANCODE_ESCAPE;
            if(!binding_controller_ && !event.key.repeat && event.key.scancode && valid_key_binding(event.key.scancode)) {code=event.key.scancode;captured=true;}
        } else if(event.type==SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
            cancel=event.gbutton.button==SDL_GAMEPAD_BUTTON_BACK;
            if(binding_controller_ && !cancel) {code=int(event.gbutton.button)+1;captured=valid_pad_binding(code);}
        } else if(event.type==SDL_EVENT_GAMEPAD_AXIS_MOTION && binding_controller_ && std::abs(int(event.gaxis.value))>18000) {
            code=pad_axis_binding(event.gaxis.axis,event.gaxis.value>0);captured=valid_pad_binding(code);
        }
        if(captured) {
            auto& binding=settings_.bindings[binding_action_];(binding_controller_?binding.gamepad:binding.keyboard)=code;binding_action_=-1;
            if(binding_controller_) {binding_release_=code;binding_device_=event.type==SDL_EVENT_GAMEPAD_AXIS_MOTION?event.gaxis.which:event.gbutton.which;}
            try {settings_.save(path_);} catch(const std::exception& error) {message(error.what());}
        }
        if(cancel) {
            binding_action_=-1;
            if(event.type==SDL_EVENT_GAMEPAD_BUTTON_DOWN) {binding_release_=int(event.gbutton.button)+1;binding_device_=event.gbutton.which;}
        }
        if(event.type==SDL_EVENT_KEY_DOWN || event.type==SDL_EVENT_GAMEPAD_BUTTON_DOWN || event.type==SDL_EVENT_GAMEPAD_AXIS_MOTION) return true;
    }
    ImGui_ImplSDL3_ProcessEvent(&event);
    if(event.type==SDL_EVENT_WINDOW_DISPLAY_CHANGED || event.type==SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED) refresh_options();
    if(event.type==SDL_EVENT_QUIT || event.type==SDL_EVENT_WINDOW_CLOSE_REQUESTED) quit=true;
    if(event.type==SDL_EVENT_KEY_DOWN && !event.key.repeat && (event.key.scancode==SDL_SCANCODE_F1 || event.key.scancode==SDL_SCANCODE_ESCAPE)) {
        if(playing_) visible=!visible;
        if(!visible) binding_action_=-1;
        return true;
    }
    if(event.type==SDL_EVENT_GAMEPAD_BUTTON_DOWN && event.gbutton.button==SDL_GAMEPAD_BUTTON_BACK) {
        if(playing_) visible=!visible;
        if(!visible) binding_action_=-1;
        return true;
    }
    return false;
}
void SettingsUi::message(std::string text) {
    diagnostic_record("{\"type\":\"settings_activity\",\"message\":"+json(text)+'}');
    if(messages_.size()==24) messages_.pop_front();messages_.push_back(std::move(text));
}
#ifndef _WIN32
struct SettingsUi::FileDialog {
    std::mutex mutex;
    std::string path,error;
    bool disc=false,done=false;
};
#endif
bool SettingsUi::browse(std::array<char,2048>& value,bool disc) {
    try {
#ifdef _WIN32
    std::array<wchar_t,32768> path{};const auto initial=wide(value.data());
    if(initial.size()<path.size()) std::copy(initial.begin(),initial.end(),path.begin());
    OPENFILENAMEW dialog{};dialog.lStructSize=sizeof(dialog);
    dialog.hwndOwner=static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(window_),SDL_PROP_WINDOW_WIN32_HWND_POINTER,nullptr));
    dialog.lpstrFile=path.data();dialog.nMaxFile=static_cast<DWORD>(path.size());
    dialog.lpstrFilter=disc?L"Xbox disc image (*.iso)\0*.iso\0All files\0*.*\0":L"Xbox executable (*.xbe)\0*.xbe\0All files\0*.*\0";
    dialog.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
    if(!GetOpenFileNameW(&dialog)) {if(CommDlgExtendedError()) throw std::runtime_error("Could not open the file picker");return false;}
    copy_text(value.data(),value.size(),utf8(path.data()));return true;
#else
    if(dialog_) return false;
    static constexpr SDL_DialogFileFilter executable_filters[]={{"Xbox executable","xbe"},{"All files","*"}};
    static constexpr SDL_DialogFileFilter disc_filters[]={{"Xbox disc image","iso"},{"All files","*"}};
    const auto location=path_text(settings_.root/utf8_path(value.data()));
    auto pending=std::make_unique<std::shared_ptr<FileDialog>>(std::make_shared<FileDialog>());
    (*pending)->disc=disc;
    dialog_=*pending;
    SDL_ShowOpenFileDialog([](void* data,const char* const* files,int) {
        const std::unique_ptr<std::shared_ptr<FileDialog>> owner(static_cast<std::shared_ptr<FileDialog>*>(data));
        auto& result=**owner;
        std::lock_guard lock(result.mutex);
        try {
            if(!files) result.error=SDL_GetError();
            else if(*files) result.path=*files;
        } catch(...) {}
        result.done=true;
    },pending.release(),window_,disc?disc_filters:executable_filters,2,location.c_str(),false);
    return false;
#endif
    } catch(const std::exception& error) {file_message(error.what());return false;}
}
void SettingsUi::draw(bool playing,bool recording,double fps,const std::function<void()>& capture,const std::function<void()>& performance) {
    auto& io=ImGui::GetIO();
    constexpr auto focus=SDL_WINDOW_INPUT_FOCUS|SDL_WINDOW_MOUSE_FOCUS;
    if(!playing || !playing_ || active() || (SDL_GetWindowFlags(window_)&focus)!=focus) show_cursor();
    else if(!cursor_hidden_ && SDL_GetTicks()-mouse_activity_>=2000 && SDL_HideCursor()) {
        // The SDL backend otherwise shows the cursor every frame, even with settings closed.
        io.ConfigFlags|=ImGuiConfigFlags_NoMouseCursorChange;cursor_hidden_=true;
    }
    // SDL polls controller navigation every frame. Keep a captured confirm
    // button from activating the binding again until it has been released.
    if(binding_action_>=0 || binding_release_) io.ConfigFlags&=~ImGuiConfigFlags_NavEnableGamepad;
    else io.ConfigFlags|=ImGuiConfigFlags_NavEnableGamepad;
    ImGui_ImplDX11_NewFrame();ImGui_ImplSDL3_NewFrame();ImGui::NewFrame();
    playing_=playing;if(!playing) visible=true;
    const bool show=visible;
    const float step=ImGui::GetIO().DeltaTime/(show?.12f:.09f);
    animation_=std::clamp(animation_+(show?step:-step),0.0f,1.0f);
    ImDrawList* status_draw=nullptr;ImVec2 center{};
    bool changed=false;
#ifndef _WIN32
    if(auto pending=dialog_) {
        std::lock_guard lock(pending->mutex);
        if(pending->done) {
            auto& destination=pending->disc?settings_.disc:settings_.executable;
            if(!pending->error.empty()) file_message(pending->error);
            else if(pending->path.size()>=destination.size() || pending->path.find_first_of("\r\n")!=std::string::npos)
                file_message("The selected path is too long or contains a line break.");
            else if(!pending->path.empty()) {
                copy_text(destination.data(),destination.size(),pending->path);changed=true;
            }
            dialog_.reset();
        }
    }
#endif
    const bool was_fullscreen=settings_.fullscreen;
    if(animation_>0) {
        const auto* viewport=ImGui::GetMainViewport();const auto width=std::min(1000.0f,viewport->WorkSize.x-24),height=std::min(690.0f,viewport->WorkSize.y-24);
        center={viewport->WorkPos.x+viewport->WorkSize.x/2,viewport->WorkPos.y+viewport->WorkSize.y/2};
        ImGui::SetNextWindowPos({viewport->WorkPos.x+(viewport->WorkSize.x-width)/2,viewport->WorkPos.y+(viewport->WorkSize.y-height)/2});
        ImGui::SetNextWindowSize({width,height});
        ImGui::Begin("Super Monkey Ball Deluxe settings",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoSavedSettings);
        wooden_panel(wood_.Get(),false);ImGui::PushStyleVar(ImGuiStyleVar_DisabledAlpha,1.0f);
        ImGui::BeginDisabled(!show || animation_<1);ImGui::PopStyleVar();
        constexpr float icon_height=48;
        const float header_bottom=ImGui::GetCursorPosY()+(icon_?icon_height:ImGui::GetTextLineHeight());
        if(icon_) {
            ImGui::Image(reinterpret_cast<ImTextureID>(icon_.Get()),{icon_height*icon_aspect_,icon_height});ImGui::SameLine();
        }
        constexpr const char* save_hint="Changes save automatically";
        ImGui::PushFont(italic_font_,0);
        ImGui::SetCursorPosY(header_bottom-ImGui::GetTextLineHeight());
        ImGui::SetCursorPosX(ImGui::GetCursorPosX()+std::max(0.0f,ImGui::GetContentRegionAvail().x-ImGui::CalcTextSize(save_hint).x));
        ImGui::TextDisabled("%s",save_hint);
        ImGui::PopFont();
        ImGui::Separator();
        constexpr const char* pages[]={"Game Files","General","Audio","Graphics","Enhancements","Controls","Diagnostics"};
        const auto content=ImGui::GetContentRegionAvail();
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2{12,16});
        ImGui::BeginChild("Navigation",{170,content.y-56},ImGuiChildFlags_Borders);ImGui::PopStyleVar();
        wooden_panel(wood_.Get(),true);
        for(int i=0;i<7;++i) if(ImGui::Selectable(pages[i],category==i,0,{0,40})) {category=i;binding_action_=-1;}
        ImGui::EndChild();ImGui::SameLine();
        ImGui::BeginChild("Page",{0,content.y-56},ImGuiChildFlags_Borders);
        wooden_panel(wood_.Get(),true);
        ImGui::PushFont(nullptr,26);ImGui::TextColored({1,.83f,.32f,1},"%s",pages[category]);ImGui::PopFont();
        ImGui::Separator();ImGui::Spacing();
        try {
            switch(category) {
            case 0:
                ImGui::BeginDisabled(playing);
                label("Game executable");ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x-106);
                changed|=ImGui::InputText("##Executable",settings_.executable.data(),settings_.executable.size());
                help("The supported US Xbox game executable (default.xbe).");
                ImGui::SameLine();if(ImGui::Button("Browse##executable",{94,0})) changed|=browse(settings_.executable,false);
                label("Game disc image");ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x-106);
                changed|=ImGui::InputText("##Disc",settings_.disc.data(),settings_.disc.size());
                help("Your US Xbox XISO disc image. Game files are read directly from it.");
                ImGui::SameLine();if(ImGui::Button("Browse##disc",{94,0})) changed|=browse(settings_.disc,true);
                if(changed) files_message_.clear();
                ImGui::EndDisabled();ImGui::Spacing();
                ImGui::TextWrapped("Use the US Xbox release of Super Monkey Ball Deluxe. Select default.xbe and its XISO disc image.");
                if(playing) ImGui::TextWrapped("Close and reopen the app to change game files.");
                if(ImGui::Button("Validate game files")) {settings_.validate();file_message("Game files verified. Ready to play.");}
                if(!files_message_.empty()) ImGui::TextWrapped("%s",files_message_.c_str());
                break;
            case 1: {
                ImGui::SeparatorText("MOTION & PERFORMANCE");label("Refresh rate");
                const auto selected=settings_.refresh_rate?std::format("{} Hz",settings_.refresh_rate):"Unlimited";
                if(ImGui::BeginCombo("##Refresh rate",selected.c_str())) {
                    for(const auto rate:refresh_rates_) {
                        const auto name=rate?std::format("{} Hz",rate):"Unlimited";
                        const bool active=rate==settings_.refresh_rate;
                        if(ImGui::Selectable(name.c_str(),active)) {settings_.refresh_rate=rate;changed=true;}
                        if(active) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                help("Sets the rendering rate while keeping game timing unchanged. Unlimited removes the cap; V-sync follows your display.");
                ImGui::SeparatorText("ON-SCREEN INFO");changed|=ImGui::Checkbox("Show frame rate",&settings_.show_fps);
                help("Displays the presentation frame rate in the upper-right corner.");
                ImGui::SeparatorText("DISCORD");
                changed|=ImGui::Checkbox("Discord Rich Presence",&settings_.discord_presence);
                help("Shares your game activity and elapsed time with Discord desktop. Turning this off clears the activity.");
                if(settings_.discord_presence && presence_) ImGui::TextDisabled("%s",presence_->status().data());
                break;
            }
            case 2:
                ImGui::SeparatorText("OUTPUT");label("Master volume");changed|=ImGui::SliderInt("##Volume",&settings_.volume,0,100,"%d%%");
                help("Applies to both music and sound effects.");changed|=ImGui::Checkbox("Mute",&settings_.muted);
                help("Silences game audio while keeping your volume setting.");
                changed|=ImGui::Checkbox("Mute on focus loss",&settings_.mute_on_focus_loss);
                help("Silences game audio while another window has focus and restores it when you return.");break;
            case 3: {
                ImGui::SeparatorText("DISPLAY");constexpr const char* aspects[]={"Standard (4:3)","Widescreen (16:9)","Ultrawide (21:9)"};
                label("Aspect ratio");changed|=ImGui::Combo("##Aspect",&settings_.aspect,aspects,3);
                help("Uses the original game's Xbox display setting for the correct camera and widescreen layout. Applies immediately.");
                changed|=ImGui::Checkbox("V-sync",&settings_.vsync);help("Synchronizes presentation with your display to prevent tearing.");
                ImGui::SameLine();
                changed|=ImGui::Checkbox("Fullscreen",&settings_.fullscreen);
                break;
            }
            case 4: {
                ImGui::SeparatorText("RESOLUTION");label("Internal resolution");
                const auto current=std::format("{}x ({} x {})",settings_.resolution,640*settings_.resolution,480*settings_.resolution);
                if(ImGui::BeginCombo("##Resolution",current.c_str())) {
                    for(int scale=1;scale<=4;++scale) if(ImGui::Selectable(std::format("{}x ({} x {})",scale,640*scale,480*scale).c_str(),settings_.resolution==scale)) {settings_.resolution=scale;changed=true;}
                    ImGui::EndCombo();
                }
                help("Higher resolutions sharpen geometry and use more GPU memory. Applies immediately.");
                ImGui::SeparatorText("IMAGE QUALITY");label("Antialiasing");
                constexpr const char* quality[]={"Off (original)","FXAA Low","FXAA Medium","FXAA High","MSAA 2x","MSAA 4x","MSAA 8x"};
                if(ImGui::BeginCombo("##Antialiasing",quality[settings_.antialiasing])) {
                    for(int i=0;i<7;++i) {
                        const bool supported=i<4 || renderer_.supports_msaa(1U<<(i-3));
                        ImGui::BeginDisabled(!supported);
                        if(ImGui::Selectable(quality[i],settings_.antialiasing==i)) {settings_.antialiasing=i;changed=true;}
                        if(!supported) help("This MSAA level is unavailable on your graphics device.");
                        ImGui::EndDisabled();
                    }
                    ImGui::EndCombo();
                }
                help("MSAA smooths geometry edges while keeping textures sharp. FXAA smooths the whole image and can soften fine detail and in-game text. Higher settings use more GPU power and memory. Applies immediately.");
                constexpr int values[]={0,1,2,4,8,16};
                constexpr const char* filters[]={"Original (game setting)","Off (1x)","2x","4x","8x","16x"};
                int filter=0;for(int i=0;i<6;++i) if(settings_.anisotropy==values[i]) filter=i;
                label("Anisotropic filtering");
                if(ImGui::Combo("##Anisotropic filtering",&filter,filters,6)) {settings_.anisotropy=values[filter];changed=true;}
                help("Keeps textures sharper when viewed at an angle. Original preserves the game's filtering settings. Higher values use more GPU power. Applies immediately.");
                ImGui::SeparatorText("RENDER DISTANCE");
                changed|=ImGui::Checkbox("Render distance override",&settings_.unlimited_render_distance);
                help("Removes the original far cutoff and keeps full-resolution textures at every distance. Recommended at higher internal resolutions. May reduce performance and increase texture shimmer. Applies immediately.");break;
            }
            case 5: {
                bool controller=binding_controller_;
                if(ImGui::RadioButton("Keyboard",!controller)) controller=false;
                ImGui::SameLine();if(ImGui::RadioButton("Controller",controller)) controller=true;
                if(controller!=binding_controller_) {binding_controller_=controller;binding_action_=-1;}
                ImGui::TextWrapped("Select a binding, then press a key, button, or move a stick. Bindings apply to the game; Settings navigation uses standard controls.");
                if(binding_action_>=0) {
                    ImGui::TextColored({1,.83f,.25f,1},"Waiting for input: %s",control_actions[binding_action_].label.data());
                    ImGui::SameLine();if(ImGui::SmallButton("Cancel")) binding_action_=-1;
                }
                if(ImGui::Button("Restore controls")) {settings_.bindings=default_bindings();binding_action_=-1;changed=true;}
                const auto& style=ImGui::GetStyle();
                ImGui::PushStyleVar(ImGuiStyleVar_CellPadding,ImVec2{style.FramePadding.x,style.CellPadding.y});
                constexpr auto table_flags=ImGuiTableFlags_RowBg|ImGuiTableFlags_BordersInnerH|ImGuiTableFlags_PadOuterX;
                const auto columns=[] {
                    ImGui::TableSetupColumn("Action",ImGuiTableColumnFlags_WidthStretch,.9f);
                    ImGui::TableSetupColumn("Binding",ImGuiTableColumnFlags_WidthStretch,1.1f);
                    ImGui::TableSetupColumn("",ImGuiTableColumnFlags_WidthFixed,64);
                };
                // Only the rows scroll; reserve the same scrollbar width in the fixed header.
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,ImVec2{style.ItemSpacing.x,0});
                if(ImGui::BeginTable("Binding headers",3,table_flags,{ImGui::GetContentRegionAvail().x-style.ScrollbarSize,0})) {
                    columns();ImGui::TableHeadersRow();ImGui::EndTable();
                }
                ImGui::PopStyleVar();
                if(ImGui::BeginChild("Binding rows",{0,0},ImGuiChildFlags_None,ImGuiWindowFlags_AlwaysVerticalScrollbar)) {
                    if(ImGui::BeginTable("Bindings",3,table_flags)) {
                        columns();
                        for(unsigned i=0;i<control_actions.size();++i) {
                            ImGui::PushID(int(i));ImGui::TableNextRow();ImGui::TableNextColumn();ImGui::AlignTextToFramePadding();ImGui::TextUnformatted(control_actions[i].label.data());
                            ImGui::TableNextColumn();auto& binding=settings_.bindings[i];auto& code=binding_controller_?binding.gamepad:binding.keyboard;
                            const auto name=binding_action_==int(i)?"Press input...":binding_name(code,binding_controller_);
                            if(ImGui::Button(name.c_str(),{-1,0})) rebind(i,binding_controller_);
                            ImGui::TableNextColumn();if(ImGui::Button("Clear",{-1,0})) {code=0;binding_action_=-1;changed=true;}
                            ImGui::PopID();
                        }
                        ImGui::EndTable();
                    }
                }
                ImGui::EndChild();ImGui::PopStyleVar();
                break;
            }
            case 6:
                ImGui::SeparatorText("CAPTURE & PERFORMANCE");if(ImGui::Button("Capture frame")) capture();
                help("Saves images, game state and rendering resources in build/captures.");
                ImGui::BeginDisabled(!playing);if(ImGui::Button(recording?"Save performance":"Record performance")) performance();ImGui::EndDisabled();
                ImGui::TextWrapped("Record a slowdown, then save the recording. Recording stops after 60 seconds. Capturing a frame also saves an active recording.");
                ImGui::SeparatorText("ACTIVITY");
                if(messages_.empty()) ImGui::TextDisabled("No messages yet.");
                for(auto item=messages_.rbegin();item!=messages_.rend();++item) {ImGui::TextWrapped("%s",item->c_str());ImGui::Spacing();}
                break;
            }
        } catch(const std::exception& error) {if(category==0) file_message(error.what());else message(error.what());}
        ImGui::EndChild();ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text,ImVec4{.12f,.12f,.035f,1});ImGui::PushStyleColor(ImGuiCol_Button,ImVec4{1,.8f,.12f,1});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ImVec4{1,.88f,.25f,1});ImGui::PushStyleColor(ImGuiCol_ButtonActive,ImVec4{.95f,.7f,.05f,1});
        if(ImGui::Button(playing?"Close settings":"Play game",{170,0})) {if(playing) {visible=false;binding_action_=-1;} else launch=true;}
        ImGui::PopStyleColor(4);ImGui::SameLine();
        if(playing) {if(ImGui::Button("Restart game",{170,0})) restart=true;ImGui::SameLine();}
        if(ImGui::Button("Restore defaults",{170,0})) {settings_=Settings(settings_.root);files_message_.clear();binding_action_=-1;changed=true;message("Default settings restored.");}
        ImGui::SameLine();if(ImGui::Button("Exit game",{170,0})) quit=true;
        ImGui::EndDisabled();ImGui::End();
    }
    if(settings_.show_fps || recording) {
        const auto* viewport=ImGui::GetMainViewport();ImGui::SetNextWindowPos({viewport->WorkPos.x+viewport->WorkSize.x-12,viewport->WorkPos.y+12},ImGuiCond_Always,{1,0});
        ImGui::SetNextWindowBgAlpha(.8f);ImGui::Begin("Status",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoNav|ImGuiWindowFlags_NoInputs);
        status_draw=ImGui::GetWindowDrawList();
        if(settings_.show_fps) ImGui::Text("%.1f FPS",playing?fps:ImGui::GetIO().Framerate);
        if(recording) ImGui::TextColored({1,.8f,.12f,1},"Recording performance");ImGui::End();
    }
    if(settings_.fullscreen!=was_fullscreen && !SDL_SetWindowFullscreen(window_,settings_.fullscreen)) {
        settings_.fullscreen=was_fullscreen;message(SDL_GetError());
    }
    if(changed) try {settings_.save(path_);} catch(const std::exception& error) {message(error.what());}
    if(presence_) presence_->update(settings_.discord_presence && !quit && !restart,playing,visible);
    ImGui::Render();
    if(animation_>0 && animation_<1) {
        const float ease=animation_*animation_*(3-2*animation_),scale=.65f+.35f*ease;
        const auto transform=[&](ImVec2 p) {return ImVec2{center.x+(p.x-center.x)*scale,center.y+(p.y-center.y)*scale};};
        for(auto* list:ImGui::GetDrawData()->CmdLists) if(list!=status_draw) {
            for(auto& vertex:list->VtxBuffer) {
                vertex.pos=transform(vertex.pos);const auto alpha=(vertex.col>>IM_COL32_A_SHIFT)&255;
                vertex.col=(vertex.col&~IM_COL32_A_MASK)|(static_cast<ImU32>(alpha*ease)<<IM_COL32_A_SHIFT);
            }
            for(auto& command:list->CmdBuffer) {
                const auto min=transform({command.ClipRect.x,command.ClipRect.y}),max=transform({command.ClipRect.z,command.ClipRect.w});
                command.ClipRect={min.x,min.y,max.x,max.y};
            }
        }
    }
    // Advance input even while hidden, so old clicks cannot activate controls
    // when settings next opens. Empty frames need no D3D11 submission.
    if(ImGui::GetDrawData()->CmdListsCount) ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}
}
