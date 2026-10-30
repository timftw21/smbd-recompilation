#pragma once
#include <array>
#include <cstdint>

namespace smbd {
// Original routines entered directly by the native bridge, outside guest calls.
// Keep the bridge's accepted entries and whole-game reachability in agreement.
inline constexpr std::array native_graphics_helpers{
    0x00320410U,0x00322830U,0x00322B40U,0x0031D5C0U,0x0031DC00U,
    0x00322940U,0x0031AF10U,0x0031D990U,0x0031D9E0U,0x00321BA0U};
inline constexpr std::array native_goal_queries{0x00012F30U,0x00013180U};
}
