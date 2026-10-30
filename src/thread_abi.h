#pragma once
#include <cstdint>

namespace smbd {
// Native thread records are cleared before publication. This host registers no
// optional callback record; original accesses to that word are audited separately.
struct NativeThreadAbi {
    static constexpr std::uint32_t current=0x20, tls_current=0x28;
    static constexpr std::uint32_t tib=0x400, optional_callbacks=0x250;
};
}
