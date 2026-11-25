#pragma once
#include <array>
#include <cstdint>

namespace smbd {
inline constexpr std::uint32_t game_dispatcher=0x00014B90;
// Original routines entered directly by the native bridge, outside guest calls.
// Keep the bridge's accepted entries and whole-game reachability in agreement.
inline constexpr std::array native_graphics_helpers{
    0x00320410U,0x00322830U,0x00322B40U,0x0031D5C0U,0x0031DC00U,
    0x00322940U,0x0031AF10U,0x0031D990U,0x0031D9E0U,0x00321BA0U};
inline constexpr std::array native_goal_queries{0x00012F30U,0x00013180U};
// EFFECT update/draw tables 4B2DB0/4B2ED0, slots 1 (HUD) and 3 (world).
inline constexpr std::uint32_t banana_draw_update(std::uint32_t draw) {
    if(draw==0x4E900) return 0x4E7F0;
    if(draw==0x4F210) return 0x4F050;
    return 0;
}
struct FramePresentation {
    std::uint32_t entry,prologue_end,draw,frame_bytes,argument_bytes;
};
// Verified controller/drawing boundaries and complete presentation routines.
// These entries rebuild original draws; they retain no vertices or GPU packets.
inline constexpr std::array frame_presentations{
    FramePresentation{0x2A13B0,0x2A13D0,0x2A22D2,0xB4,0},
    FramePresentation{0x2A5D50,0x2B71AA,0x2B7233,0x58,0},
    FramePresentation{0x2A5D80,0x2B7872,0x2B7E15,0x308,0},
    FramePresentation{0x2A9970,0,0x2A9970,0,4},
    FramePresentation{0x1D3890,0,0x1D3890,0,4},
    FramePresentation{0x1E0470,0,0x1E0470,0,0}};
inline constexpr auto native_menu_helpers=[] {
    std::array<std::uint32_t,frame_presentations.size()> entries{};
    for(unsigned i=0;i<entries.size();++i) entries[i]=frame_presentations[i].entry;
    return entries;
}();
}
