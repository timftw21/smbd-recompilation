#pragma once
#include "cpu.h"
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
// wait32 HEX_ADDRESS HEX_MASK HEX_VALUE [BUTTON] waits for an original game
// state, optionally pulsing BUTTON until it matches. Later frames are relative
// to that match. This only reads guest state and sends ordinary SDL inputs.
class Automation {
    struct Step {
        std::uint64_t frame=0;
        int button=SDL_GAMEPAD_BUTTON_INVALID;
        std::array<Sint16,2> stick{};
        bool wait=false;
        std::uint32_t address=0,mask=0,value=0;
    };
    std::vector<Step> steps_;
    std::size_t next_=0;
    std::uint64_t origin_=0,wait_start_=0;
    std::size_t waits_=0;
    bool waiting_=false;
    int current_button_=SDL_GAMEPAD_BUTTON_INVALID;
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
            fields>>name;
            if(name=="wait32") {
                Step step;step.wait=true;name="none";
                if(!(fields>>std::hex>>step.address>>step.mask>>step.value) || !step.mask || (step.value&~step.mask))
                    throw std::runtime_error("Invalid controller wait address, mask or value");
                fields>>std::ws;
                if(!fields.eof() && !(fields>>name)) throw std::runtime_error("Invalid controller wait button");
                const auto found=std::ranges::find_if(names,[&](const char* value){return name==value;});
                if(found==names.end() || fields>>extra || steps_.size()==4096)
                    throw std::runtime_error("Invalid controller wait button or size");
                step.button=buttons[found-names.begin()];steps_.push_back(step);continue;
            }
            fields.clear();fields.str(line);
            if(!(fields>>frame>>name)) throw std::runtime_error("Invalid controller timeline line");
            fields>>std::ws;
            if(!fields.eof() && !(fields>>x>>y)) throw std::runtime_error("Controller timeline sticks require X and Y");
            if(fields>>extra || frame>UINT32_MAX || !std::isfinite(x) || !std::isfinite(y) || std::abs(x)>1 || std::abs(y)>1 ||
                steps_.size()==4096 || (!steps_.empty() && !steps_.back().wait && frame<=steps_.back().frame))
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
    std::size_t waits_applied() const {return waits_;}
    bool finished() const {return next_==steps_.size();}
    void update(std::uint64_t frame,Memory& memory) {
        bool changed=false;
        const auto button=[&](int value) {
            if(value==current_button_) return;
            for(int i=0;i<SDL_GAMEPAD_BUTTON_COUNT;++i)
                if(!SDL_SetJoystickVirtualButton(joystick_,i,i==value)) throw std::runtime_error(SDL_GetError());
            current_button_=value;changed=true;
        };
        while(next_<steps_.size()) {
            const auto& step=steps_[next_];
            if(step.wait) {
                if(!waiting_) {wait_start_=frame;waiting_=true;}
                if((memory.load<std::uint32_t>(step.address)&step.mask)!=step.value) {
                    button((frame-wait_start_)%60<12?step.button:SDL_GAMEPAD_BUTTON_INVALID);
                    break;
                }
                button(SDL_GAMEPAD_BUTTON_INVALID);
                origin_=frame;waiting_=false;++waits_;++next_;continue;
            }
            if(step.frame>frame-origin_) break;
            ++next_;button(step.button);
            for(int i=0;i<2;++i) if(!SDL_SetJoystickVirtualAxis(joystick_,i,step.stick[i])) throw std::runtime_error(SDL_GetError());
            changed=true;
        }
        if(changed) SDL_UpdateJoysticks();
    }
};
}
