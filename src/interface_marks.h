#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace smbd {
// The texture rectangle each UV set of one frontend quad spans, as
// {minimum u, minimum v, maximum u, maximum v} in normalized coordinates.
struct InterfaceSampleBounds {std::array<std::array<float,4>,2> sets{};std::uint32_t mask=0;};
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
