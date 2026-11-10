#pragma once
#include "file.h"
#include "input.h"
#include <array>
#include <deque>
#include <functional>
#include <memory>
#include <vector>
#include "com_ptr.h"

struct SDL_Window;
union SDL_Event;
struct ID3D11ShaderResourceView;
struct ImFont;
namespace smbd {
class DiscordPresence;
class Renderer;
std::string check_presentation(const std::filesystem::path&,const std::filesystem::path& xbe={},const std::filesystem::path& disc={});
struct Settings {
    std::array<char,2048> executable{},disc{};
    bool show_fps=false,muted=false,mute_on_focus_loss=true,vsync=false,fullscreen=false;
    bool unlimited_render_distance=false;
    bool discord_presence=false;
    int volume=100,aspect=1,resolution=1,refresh_rate=60,antialiasing=0,anisotropy=0;
    ControlBindings bindings=default_bindings();
    std::filesystem::path root;
    explicit Settings(std::filesystem::path base={});
    void load(const std::filesystem::path&);
    void save(const std::filesystem::path&) const;
    void validate() const;
    float output_gain(bool focused) const;
    float display_aspect() const;
    std::filesystem::path executable_path() const;
    std::filesystem::path disc_path() const;
};
class SettingsUi {
public:
    SettingsUi(SDL_Window*,Renderer&,Settings&,std::filesystem::path,DiscordPresence* presence=nullptr);
    ~SettingsUi();
    bool event(const SDL_Event&);
    void draw(bool playing,bool recording,double fps,const std::function<void()>& capture,const std::function<void()>& performance);
    void message(std::string);
    void file_message(std::string text) {files_message_=text;message(std::move(text));category=0;}
    bool visible=true,launch=false,restart=false,quit=false;
    int category=0;
    const std::deque<std::string>& messages() const {return messages_;}
    float display_refresh() const {return display_refresh_;}
    bool active() const {return visible || animation_>0;}
    bool animating() const {return visible?animation_<1:animation_>0;}
    void hide() {visible=false;animation_=0;binding_action_=-1;binding_release_=0;}
private:
    SDL_Window* window_;
    Renderer& renderer_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> icon_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> wood_;
    float icon_aspect_=1;
    Settings& settings_;
    DiscordPresence* presence_;
    std::filesystem::path path_;
    std::deque<std::string> messages_;
    std::string files_message_;
    std::vector<int> refresh_rates_;
    float display_refresh_=0;
    float animation_=0;
    bool playing_=false,binding_controller_=false;
    ImFont* italic_font_=nullptr;
    std::uint64_t mouse_activity_=0;
    bool cursor_hidden_=false;
    int binding_action_=-1;
    int binding_release_=0;
    std::uint32_t binding_device_=0;
#ifndef _WIN32
    struct FileDialog;
    std::shared_ptr<FileDialog> dialog_;
#endif
    void rebind(unsigned action,bool controller);
    void refresh_options();
    void show_cursor();
    bool browse(std::array<char,2048>&,bool);
};
}
