// SPDX-License-Identifier: MIT
// core 0.1 for smbd-us: the API mods use on top of osmod::Ctx.
#pragma once
#include <cstdint>

namespace core {

// XINPUT_STATE as the Xbox SDK lays it out (22 bytes, no padding).
#pragma pack(push, 1)
struct Pad {
    std::uint32_t packet;
    std::uint16_t buttons;          // digital: d-pad, start, back, thumbs
    std::uint8_t analog[8];         // A B X Y black white LT RT, 0..255
    std::int16_t lx, ly, rx, ry;    // sticks, -32768..32767, +y is up
};
#pragma pack(pop)
static_assert(sizeof(Pad) == 22);

// pad_read: the state the game is about to see. nullptr if the call failed.
inline Pad* pad(osmod::Ctx& ctx) {
    if (ctx.result() != 0) return nullptr;  // ERROR_SUCCESS
    return static_cast<Pad*>(ctx.memory.access(ctx.arg(1), sizeof(Pad)));
}

// Clamp helper for stick edits: -32768 has no positive mirror.
inline std::int16_t negate(std::int16_t v) { return v == -32768 ? 32767 : static_cast<std::int16_t>(-v); }

// Frames presented since boot (counted on the frame event, before mods run).
std::uint64_t frame_count();

}  // namespace core
