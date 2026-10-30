#pragma once
#include "file.h"
#include <array>
#include <deque>
#include <functional>
#include <vector>

struct SDL_Window;
union SDL_Event;
namespace smbd {
class Renderer;
std::string check_presentation(const std::filesystem::path&);
struct Settings {
    std::array<char,2048> executable{},disc{};
    bool show_fps=false,muted=false,mute_on_focus_loss=true,vsync=false,fullscreen=false;
    int volume=100,aspect=1,resolution=1,refresh_rate=60;
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
    SettingsUi(SDL_Window*,Renderer&,Settings&,std::filesystem::path);
    ~SettingsUi();
    void event(const SDL_Event&);
    void draw(bool playing,bool recording,double fps,const std::function<void()>& capture,const std::function<void()>& performance);
    void message(std::string);
    bool visible=true,launch=false,restart=false,quit=false;
    int category=0;
    const std::deque<std::string>& messages() const {return messages_;}
    float display_refresh() const {return display_refresh_;}
private:
    SDL_Window* window_;
    Settings& settings_;
    std::filesystem::path path_;
    std::deque<std::string> messages_;
    std::vector<int> refresh_rates_;
    float display_refresh_=0;
    void refresh_options();
    bool browse(std::array<char,2048>&,bool);
};
}
