#pragma once
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace smbd {
// Opt-in game automation uses SDL's normal controller path. Each line is
// FRAME BUTTON [LEFT_X LEFT_Y]; "none" releases buttons, positive Y is up.
class Automation {
    struct Step {std::uint64_t frame;int button;std::array<Sint16,2> stick;};
    std::vector<Step> steps_;
    std::size_t next_=0;
    SDL_JoystickID id_=0;
    SDL_Joystick* joystick_=nullptr;
public:
    explicit Automation(const std::filesystem::path& path) {
        if(std::filesystem::file_size(path)>1024*1024) throw std::runtime_error("Controller timeline exceeds 1 MiB");
        std::ifstream input(path);
        if(!input) throw std::runtime_error("Cannot open controller timeline");
        std::string line;
        constexpr std::array names{"a","b","x","y","start","back","up","down","left","right","none"};
        constexpr std::array buttons{SDL_GAMEPAD_BUTTON_SOUTH,SDL_GAMEPAD_BUTTON_EAST,SDL_GAMEPAD_BUTTON_WEST,
            SDL_GAMEPAD_BUTTON_NORTH,SDL_GAMEPAD_BUTTON_START,SDL_GAMEPAD_BUTTON_BACK,SDL_GAMEPAD_BUTTON_DPAD_UP,
            SDL_GAMEPAD_BUTTON_DPAD_DOWN,SDL_GAMEPAD_BUTTON_DPAD_LEFT,SDL_GAMEPAD_BUTTON_DPAD_RIGHT,SDL_GAMEPAD_BUTTON_INVALID};
        while(std::getline(input,line)) {
            if(line.empty() || line[0]=='#') continue;
            std::istringstream fields(line);std::uint64_t frame;std::string name,extra;float x=0,y=0;
            if(!(fields>>frame>>name)) throw std::runtime_error("Invalid controller timeline line");
            fields>>std::ws;
            if(!fields.eof() && !(fields>>x>>y)) throw std::runtime_error("Controller timeline sticks require X and Y");
            if(fields>>extra || frame>UINT32_MAX || !std::isfinite(x) || !std::isfinite(y) || std::abs(x)>1 || std::abs(y)>1 ||
                steps_.size()==4096 || (!steps_.empty() && frame<=steps_.back().frame))
                throw std::runtime_error("Invalid controller timeline order, size or stick values");
            const auto found=std::ranges::find_if(names,[&](const char* value){return name==value;});
            if(found==names.end()) throw std::runtime_error("Unknown controller timeline button");
            steps_.push_back({frame,buttons[found-names.begin()],{static_cast<Sint16>(std::lround(x*32767)),static_cast<Sint16>(std::lround(-y*32767))}});
        }
        if(!input.eof() || steps_.empty()) throw std::runtime_error("Controller timeline could not be read");
        SDL_VirtualJoystickDesc description{};SDL_INIT_INTERFACE(&description);
        description.type=SDL_JOYSTICK_TYPE_GAMEPAD;description.naxes=SDL_GAMEPAD_AXIS_COUNT;
        description.nbuttons=SDL_GAMEPAD_BUTTON_COUNT;description.axis_mask=(1U<<SDL_GAMEPAD_AXIS_COUNT)-1;
        description.button_mask=(1U<<SDL_GAMEPAD_BUTTON_COUNT)-1;description.name="Super Monkey Ball Deluxe automation";
        id_=SDL_AttachVirtualJoystick(&description);
        if(id_) joystick_=SDL_OpenJoystick(id_);
        if(!joystick_) {const std::string error=SDL_GetError();if(id_)SDL_DetachVirtualJoystick(id_);throw std::runtime_error(error);}
        if(!SDL_SetJoystickVirtualAxis(joystick_,SDL_GAMEPAD_AXIS_LEFT_TRIGGER,-32768) ||
           !SDL_SetJoystickVirtualAxis(joystick_,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,-32768)) {
            const std::string error=SDL_GetError();SDL_CloseJoystick(joystick_);SDL_DetachVirtualJoystick(id_);throw std::runtime_error(error);
        }
    }
    ~Automation(){SDL_CloseJoystick(joystick_);SDL_DetachVirtualJoystick(id_);}
    Automation(const Automation&)=delete;
    Automation& operator=(const Automation&)=delete;
    SDL_JoystickID device() const {return id_;}
    std::size_t applied() const {return next_;}
    void update(std::uint64_t frame) {
        bool changed=false;
        while(next_<steps_.size() && steps_[next_].frame<=frame) {
            const auto& step=steps_[next_++];
            for(int i=0;i<SDL_GAMEPAD_BUTTON_COUNT;++i)
                if(!SDL_SetJoystickVirtualButton(joystick_,i,i==step.button)) throw std::runtime_error(SDL_GetError());
            for(int i=0;i<2;++i) if(!SDL_SetJoystickVirtualAxis(joystick_,i,step.stick[i])) throw std::runtime_error(SDL_GetError());
            changed=true;
        }
        if(changed) SDL_UpdateJoysticks();
    }
};
}
