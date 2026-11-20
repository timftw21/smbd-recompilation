#pragma once
#include "audio.h"
#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace smbd {
class Disc;
class Xbe;
// Hermite curve evaluation shared with the original helper at ADBB0.
// Points are 16 bytes: integer x, then float y and two tangents.
double rolling_curve(std::span<const std::byte> points,float x);

// The GameCube rolling sound, rebuilt from the three looping recordings on the
// Deluxe disc. The original controller at E9180 still computes every layer
// volume; these native voices replace only its non-looping ADX playback.
class ClassicRolling {
public:
    static constexpr unsigned players=8,layers=3;
    static constexpr std::uint32_t rate=44100;
    explicit ClassicRolling(Disc&);
    // volume is the Deluxe controller's layer volume; request is the caller's
    // volume argument; doppler is the 14-bit MusyX doppler value from 58174C.
    void start(Audio&,unsigned player,unsigned layer,unsigned volume,int request,double doppler);
    void update(Audio&,unsigned player,unsigned layer,unsigned volume,int request,double doppler);
    void release(Audio&,unsigned player);
    bool active(unsigned player) const {return player<players && active_[player];}
    static float gain(unsigned volume,int request);
    std::span<const std::int16_t> samples(unsigned layer) const {return pcm_.at(layer);}
private:
    struct Layer {std::array<std::uint32_t,2> voices{};unsigned current=0;};
    std::array<std::vector<std::int16_t>,layers> pcm_;
    std::array<std::array<Layer,layers>,players> voices_{};
    std::array<bool,players> active_{};
    std::uint32_t voice(Audio&,unsigned layer,std::uint32_t& id);
};
std::string check_rolling_sound(Xbe&,const std::filesystem::path& disc,const std::filesystem::path& output);
}
