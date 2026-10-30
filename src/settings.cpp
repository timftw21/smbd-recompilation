#include "settings.h"
#include "render.h"
#include "xbe.h"
#include "disc.h"
#include "diagnostics.h"
#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_dx11.h>
#include <Windows.h>
#include <commdlg.h>
#include <algorithm>
#include <charconv>
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
        else if(name=="volume") volume=std::clamp(number,0,100);
        else if(name=="aspect") aspect=std::clamp(number,0,2);
        else if(name=="resolution") resolution=std::clamp(number,1,4);
        else if(name=="refresh_rate" && (!number || (number>=30 && number<=1000))) refresh_rate=number;
    }
}
void Settings::save(const std::filesystem::path& path) const {
    const auto text=std::format("executable={}\ndisc={}\nshow_fps={}\nvolume={}\nmuted={}\nmute_on_focus_loss={}\naspect={}\nvsync={}\nfullscreen={}\nresolution={}\nrefresh_rate={}\n",
        executable.data(),disc.data(),int(show_fps),volume,int(muted),int(mute_on_focus_loss),aspect,int(vsync),int(fullscreen),resolution,refresh_rate);
    if(text.find('\r')!=std::string::npos || std::string_view(executable.data()).find('\n')!=std::string_view::npos ||
       std::string_view(disc.data()).find('\n')!=std::string_view::npos) throw std::runtime_error("Game paths cannot contain line breaks");
    if(path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    auto temporary=path;temporary+=".tmp";write_text(temporary,text,true);
    if(!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Cannot save settings");
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
std::string check_presentation(const std::filesystem::path& directory) {
    if(std::filesystem::exists(directory)) throw std::runtime_error("Use a fresh presentation diagnostic directory");
    std::filesystem::create_directories(directory);
    unsigned checks=0;
    const auto require=[&](bool condition) {++checks;if(!condition) throw std::runtime_error("Completed-frame presentation check failed");};
    Settings settings;require(settings.refresh_rate==60);
    for(int rate:{0,30,144,360,1000}) {
        settings.refresh_rate=rate;settings.save(directory/"settings.cfg");
        Settings restored;restored.load(directory/"settings.cfg");require(restored.refresh_rate==rate);
    }
    write_text(directory/"invalid.cfg","refresh_rate=1\nrefresh_rate=-1\nrefresh_rate=1001\n",false);
    Settings invalid;invalid.load(directory/"invalid.cfg");require(invalid.refresh_rate==60);
    if(!SDL_InitSubSystem(SDL_INIT_VIDEO)) throw std::runtime_error(SDL_GetError());
    struct Video {~Video(){SDL_QuitSubSystem(SDL_INIT_VIDEO);}} video;
    std::unique_ptr<SDL_Window,decltype(&SDL_DestroyWindow)> window(SDL_CreateWindow("Presentation diagnostic",96,72,SDL_WINDOW_HIDDEN),SDL_DestroyWindow);
    if(!window) throw std::runtime_error(SDL_GetError());
    std::string adapter;
    for(unsigned scale:{1U,4U}) {
        Renderer renderer(32,24,false,true,scale);adapter=renderer.adapter();
        renderer.attach_window(SDL_GetPointerProperty(SDL_GetWindowProperties(window.get()),SDL_PROP_WINDOW_WIN32_HWND_POINTER,nullptr));
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
        require(SDL_SetWindowSize(window.get(),120,90));SDL_PumpEvents();
        require(renderer.display(true));const auto resized=renderer.read_display();renderer.present(false);
        require(resized.width==120 && resized.height==90);
        renderer.resize(40,30);require(!renderer.display(true));
        renderer.clear(0xFF204060);renderer.publish_frame();require(renderer.display(true));renderer.present(false);
        require(renderer.errors().empty());
        require(SDL_SetWindowSize(window.get(),96,72));SDL_PumpEvents();
    }
    const auto report=std::format("{{\"format\":\"smbd-presentation-check-v1\",\"passed\":true,\"checks\":{},\"adapter\":{},\"hidden_window\":true,\"audio_opened\":false,\"scales\":[1,4],\"aspect_ratios\":[\"4:3\",\"16:9\",\"21:9\"]}}",checks,json(adapter));
    write_text(directory/"report.json",report+'\n',false);return report;
}
namespace {
void help(const char* text) {
    if(ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {ImGui::BeginTooltip();ImGui::PushTextWrapPos(370);ImGui::TextUnformatted(text);ImGui::PopTextWrapPos();ImGui::EndTooltip();}
}
void label(const char* text) {ImGui::Spacing();ImGui::TextUnformatted(text);ImGui::SetNextItemWidth(-1);}
}
SettingsUi::SettingsUi(SDL_Window* window,Renderer& renderer,Settings& settings,std::filesystem::path path):
    window_(window),settings_(settings),path_(std::move(path)) {
    refresh_options();
    IMGUI_CHECKVERSION();ImGui::CreateContext();auto& io=ImGui::GetIO();
    io.ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard|ImGuiConfigFlags_NavEnableGamepad;
    io.IniFilename=nullptr;io.LogFilename=nullptr;
    if(std::filesystem::exists("C:/Windows/Fonts/segoeui.ttf")) io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/segoeui.ttf",18);
    auto& style=ImGui::GetStyle();style.FontSizeBase=18;style.WindowPadding={24,20};style.FramePadding={12,8};style.ItemSpacing={12,10};
    style.WindowRounding=12;style.ChildRounding=9;style.FrameRounding=5;style.GrabRounding=5;style.WindowBorderSize=0;
    ImGui::StyleColorsDark();auto* c=style.Colors;
    c[ImGuiCol_WindowBg]={.045f,.085f,.065f,.98f};c[ImGuiCol_ChildBg]={.075f,.13f,.095f,1};
    c[ImGuiCol_FrameBg]={.15f,.22f,.13f,1};c[ImGuiCol_FrameBgHovered]={.22f,.32f,.16f,1};c[ImGuiCol_FrameBgActive]={.27f,.39f,.18f,1};
    c[ImGuiCol_Text]={1,.97f,.83f,1};c[ImGuiCol_TextDisabled]={.63f,.71f,.59f,1};
    c[ImGuiCol_Button]={.28f,.34f,.13f,1};c[ImGuiCol_ButtonHovered]={.43f,.49f,.16f,1};c[ImGuiCol_ButtonActive]={.55f,.58f,.17f,1};
    c[ImGuiCol_Header]={.33f,.38f,.12f,1};c[ImGuiCol_HeaderHovered]={.43f,.48f,.15f,1};c[ImGuiCol_HeaderActive]={.5f,.55f,.16f,1};
    c[ImGuiCol_CheckMark]={1,.83f,.12f,1};c[ImGuiCol_SliderGrab]={1,.78f,.08f,1};c[ImGuiCol_SliderGrabActive]={1,.9f,.3f,1};
    c[ImGuiCol_Separator]={.43f,.34f,.13f,1};c[ImGuiCol_Border]={.25f,.32f,.19f,1};c[ImGuiCol_NavCursor]={1,.85f,.15f,1};
    if(!ImGui_ImplSDL3_InitForD3D(window) || !ImGui_ImplDX11_Init(renderer.native_device(),renderer.native_context()))
        throw std::runtime_error("Cannot initialize the settings menu");
    try {
        const auto icon=load_image(settings_.root/"resources/smbd.png");
        auto* surface=SDL_CreateSurfaceFrom(static_cast<int>(icon.width),static_cast<int>(icon.height),SDL_PIXELFORMAT_RGBA32,
            const_cast<std::byte*>(icon.rgba.data()),static_cast<int>(icon.width*4));
        if(surface) {SDL_SetWindowIcon(window,surface);SDL_DestroySurface(surface);}
    } catch(const std::exception& error) {message(error.what());}
}
SettingsUi::~SettingsUi() {ImGui_ImplDX11_Shutdown();ImGui_ImplSDL3_Shutdown();ImGui::DestroyContext();}
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
void SettingsUi::event(const SDL_Event& event) {
    ImGui_ImplSDL3_ProcessEvent(&event);
    if(event.type==SDL_EVENT_WINDOW_DISPLAY_CHANGED || event.type==SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED) refresh_options();
    if(event.type==SDL_EVENT_QUIT || event.type==SDL_EVENT_WINDOW_CLOSE_REQUESTED) quit=true;
    if(event.type==SDL_EVENT_KEY_DOWN && !event.key.repeat && (event.key.scancode==SDL_SCANCODE_F1 || event.key.scancode==SDL_SCANCODE_ESCAPE)) {
        visible=!visible;message(visible?"Settings opened.":"Settings closed.");
    }
    if(event.type==SDL_EVENT_GAMEPAD_BUTTON_DOWN && event.gbutton.button==SDL_GAMEPAD_BUTTON_BACK) visible=!visible;
}
void SettingsUi::message(std::string text) {
    diagnostic_record("{\"type\":\"settings_activity\",\"message\":"+json(text)+'}');
    if(messages_.size()==24) messages_.pop_front();messages_.push_back(std::move(text));
}
bool SettingsUi::browse(std::array<char,2048>& value,bool disc) {
    try {
    std::array<wchar_t,32768> path{};const auto initial=wide(value.data());
    if(initial.size()<path.size()) std::copy(initial.begin(),initial.end(),path.begin());
    OPENFILENAMEW dialog{};dialog.lStructSize=sizeof(dialog);
    dialog.hwndOwner=static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(window_),SDL_PROP_WINDOW_WIN32_HWND_POINTER,nullptr));
    dialog.lpstrFile=path.data();dialog.nMaxFile=static_cast<DWORD>(path.size());
    dialog.lpstrFilter=disc?L"Xbox disc image (*.iso)\0*.iso\0All files\0*.*\0":L"Xbox executable (*.xbe)\0*.xbe\0All files\0*.*\0";
    dialog.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
    if(!GetOpenFileNameW(&dialog)) {if(CommDlgExtendedError()) throw std::runtime_error("Could not open the file picker");return false;}
    copy_text(value.data(),value.size(),utf8(path.data()));return true;
    } catch(const std::exception& error) {message(error.what());return false;}
}
void SettingsUi::draw(bool playing,bool recording,double fps,const std::function<void()>& capture,const std::function<void()>& performance) {
    ImGui_ImplDX11_NewFrame();ImGui_ImplSDL3_NewFrame();ImGui::NewFrame();
    bool changed=false;
    const bool was_fullscreen=settings_.fullscreen;
    if(visible || !playing) {
        const auto* viewport=ImGui::GetMainViewport();const auto width=std::min(1000.0f,viewport->WorkSize.x-24),height=std::min(690.0f,viewport->WorkSize.y-24);
        ImGui::SetNextWindowPos({viewport->WorkPos.x+(viewport->WorkSize.x-width)/2,viewport->WorkPos.y+(viewport->WorkSize.y-height)/2});
        ImGui::SetNextWindowSize({width,height});
        ImGui::Begin("Super Monkey Ball Deluxe settings",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoSavedSettings);
        ImGui::PushStyleColor(ImGuiCol_Text,ImVec4{1,.84f,.13f,1});ImGui::PushFont(nullptr,24);
        ImGui::TextUnformatted("SUPER MONKEY BALL DELUXE");ImGui::PopFont();ImGui::PopStyleColor();
        ImGui::SameLine();ImGui::TextDisabled(" / Settings");ImGui::Separator();
        constexpr const char* pages[]={"Game Files","General","Audio","Graphics","Diagnostics"};
        const auto content=ImGui::GetContentRegionAvail();
        ImGui::BeginChild("Navigation",{170,content.y-56});
        for(int i=0;i<5;++i) if(ImGui::Selectable(pages[i],category==i,0,{0,40})) category=i;
        ImGui::Spacing();ImGui::TextDisabled("F1 / Esc: settings\nF8: capture frame\nF9: performance");
        ImGui::EndChild();ImGui::SameLine();
        ImGui::BeginChild("Page",{0,content.y-56},ImGuiChildFlags_Borders);
        ImGui::SeparatorText(pages[category]);
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
                ImGui::EndDisabled();ImGui::Spacing();
                ImGui::TextWrapped("Use the US Xbox release of Super Monkey Ball Deluxe. Select default.xbe and its XISO disc image.");
                if(playing) ImGui::TextWrapped("Close and reopen the app to change game files.");
                if(ImGui::Button("Validate game files")) {settings_.validate();message("Game files verified. Ready to play.");}
                if(!messages_.empty()) ImGui::TextWrapped("%s",messages_.back().c_str());
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
                help("Displays the presentation frame rate in the upper-right corner.");break;
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
                help("Uses the original game's Xbox display setting for the correct camera and widescreen layout. Applies when you restart the game.");
                changed|=ImGui::Checkbox("V-sync",&settings_.vsync);help("Synchronizes presentation with your display to prevent tearing.");
                changed|=ImGui::Checkbox("Fullscreen",&settings_.fullscreen);
                ImGui::SeparatorText("RESOLUTION");label("Internal resolution");
                const auto current=std::format("{}x ({} x {})",settings_.resolution,640*settings_.resolution,480*settings_.resolution);
                if(ImGui::BeginCombo("##Resolution",current.c_str())) {
                    for(int scale=1;scale<=4;++scale) if(ImGui::Selectable(std::format("{}x ({} x {})",scale,640*scale,480*scale).c_str(),settings_.resolution==scale)) {settings_.resolution=scale;changed=true;}
                    ImGui::EndCombo();
                }
                help("Higher resolutions sharpen geometry and use more GPU memory. Applies when you start or restart the game.");
                ImGui::Spacing();ImGui::TextWrapped("Resolution and aspect ratio changes apply when you start or restart the game.");break;
            }
            case 4:
                ImGui::SeparatorText("CAPTURE & PERFORMANCE");if(ImGui::Button("Capture frame (F8)")) capture();
                help("Saves images, game state and rendering resources in build/captures.");
                ImGui::BeginDisabled(!playing);if(ImGui::Button(recording?"Save performance (F9)":"Record performance (F9)")) performance();ImGui::EndDisabled();
                ImGui::TextWrapped("Record a slowdown, then press F9 to save. Recording stops after 60 seconds. F8 also saves performance while recording.");
                ImGui::SeparatorText("ACTIVITY");
                if(messages_.empty()) ImGui::TextDisabled("No messages yet.");
                for(auto item=messages_.rbegin();item!=messages_.rend();++item) {ImGui::TextWrapped("%s",item->c_str());ImGui::Spacing();}
                break;
            }
        } catch(const std::exception& error) {message(error.what());}
        ImGui::EndChild();ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text,ImVec4{.12f,.12f,.035f,1});ImGui::PushStyleColor(ImGuiCol_Button,ImVec4{1,.8f,.12f,1});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ImVec4{1,.88f,.25f,1});ImGui::PushStyleColor(ImGuiCol_ButtonActive,ImVec4{.95f,.7f,.05f,1});
        if(ImGui::Button(playing?"Close settings":"Play game",{170,0})) {if(playing) visible=false;else launch=true;}
        ImGui::PopStyleColor(4);ImGui::SameLine();
        if(playing) {if(ImGui::Button("Restart game",{170,0})) restart=true;ImGui::SameLine();}
        if(ImGui::Button("Restore defaults",{170,0})) {settings_=Settings(settings_.root);changed=true;message("Default settings restored.");}
        ImGui::SameLine();ImGui::TextDisabled("Saved automatically");
        ImGui::End();
    }
    if(settings_.show_fps || recording) {
        const auto* viewport=ImGui::GetMainViewport();ImGui::SetNextWindowPos({viewport->WorkPos.x+viewport->WorkSize.x-12,viewport->WorkPos.y+12},ImGuiCond_Always,{1,0});
        ImGui::SetNextWindowBgAlpha(.8f);ImGui::Begin("Status",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoNav|ImGuiWindowFlags_NoInputs);
        if(settings_.show_fps) ImGui::Text("%.1f FPS",playing?fps:ImGui::GetIO().Framerate);
        if(recording) ImGui::TextColored({1,.8f,.12f,1},"Recording performance - F9 saves");ImGui::End();
    }
    if(settings_.fullscreen!=was_fullscreen && !SDL_SetWindowFullscreen(window_,settings_.fullscreen)) {
        settings_.fullscreen=was_fullscreen;message(SDL_GetError());
    }
    if(changed) try {settings_.save(path_);} catch(const std::exception& error) {message(error.what());}
    ImGui::Render();
    // Advance input even while hidden, so old clicks cannot activate controls
    // when settings next opens. Empty frames need no D3D11 submission.
    if(ImGui::GetDrawData()->CmdListsCount) ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}
}
