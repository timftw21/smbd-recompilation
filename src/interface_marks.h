#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace smbd {
// The texture rectangle each UV set of one frontend quad spans, as
// {minimum u, minimum v, maximum u, maximum v} in normalized coordinates.
// Per UV set {minimum u, minimum v, maximum u, maximum v}: the quad's texture
// rectangle and, when the original 640x480 raster covers a pixel centre on
// both axes, the UVs its outermost centres sample. Those can lie up to half
// a texel outside the rectangle, where glyph art may continue.
// For sampled sets, bit set*2+c of leading_high marks UV component c whose
// leading (top/left) edge holds the maximum, and of follows_y one that
// varies along screen y rather than x. leading_edge is, per screen axis, the
// vertex-space coordinate that rasterizes at the boundary of the original's
// first covered pixel; bit axis of leading_max marks it replacing the
// vertex-space maximum. uv_word is the first UV's DWORD within a vertex.
struct InterfaceSampleBounds {
    std::array<std::array<float,4>,2> sets{},sampled{};
    std::array<float,2> leading_edge{};
    std::uint32_t mask=0,sampled_mask=0,leading_high=0,follows_y=0,leading_max=0,uv_word=0;
};
// Identity of one quad's vertex bytes. The CPU completion and the NV097
// inline payload that carries the same quad to the GPU compute it alike.
inline std::uint64_t interface_quad_identity(std::span<const std::byte> bytes) {
    std::uint64_t value=0x9E3779B97F4A7C15ULL^bytes.size();
    for(std::size_t i=0;i+4<=bytes.size();i+=4) {
        std::uint32_t word;std::memcpy(&word,bytes.data()+i,4);
        value=(value^word)*0xFF51AFD7ED558CCDULL;value^=value>>31;
    }
    return value;
}
// complete_screen_primitives (the original immediate frontend's 313B0
// completion) reports the data address of every texture its HUD, menu and
// text quads sample, and the texture rectangle of each of those quads. The
// host registers one set of receivers; nullptr members disable them.
struct InterfaceHooks {
    void(*texture)(void* context,std::uint32_t data)=nullptr;
    void(*bounds)(void* context,std::uint64_t quad,const InterfaceSampleBounds&)=nullptr;
    void* context=nullptr;
};
void set_interface_hooks(const InterfaceHooks&);
}
