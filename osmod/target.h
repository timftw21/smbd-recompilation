// SPDX-License-Identifier: MIT
// OSMod glue for x86 guests recompiled by smbd-recompilation.
// Defines osmod::Ctx, the one argument every handler receives.
#pragma once
#include "cpu.h"
#include <cstdint>

namespace osmod {

struct Ctx {
    smbd::Cpu& cpu;
    smbd::Memory& memory;

    // Guest esp when the site was entered: [esp] is the return address and
    // the stack arguments start at esp+4.
    std::uint32_t entry_esp = 0;

    // A pre handler sets skip to stop the stock function from running. The
    // bus then returns to the caller with eax = skip_result and pops
    // skip_pop bytes of arguments (stdcall), as the stock function would.
    bool skip = false;
    std::uint32_t skip_result = 0;
    std::uint32_t skip_pop = 0;

    // The i-th 32-bit stack argument at entry.
    std::uint32_t arg(unsigned i) const { return memory.load<std::uint32_t>(entry_esp + 4 + 4 * i); }
    // eax: the return value, once the stock function has run (post handlers).
    std::uint32_t result() const { return cpu.registers[smbd::eax]; }
    void set_result(std::uint32_t v) { cpu.registers[smbd::eax] = v; }

    template<class T> T load(std::uint32_t address) const { return memory.load<T>(address); }
    template<class T> void store(std::uint32_t address, T value) { memory.store<T>(address, value); }
    // A host pointer to guest memory. The guest is little-endian x86, so a
    // struct with the guest's layout can be used directly.
    template<class T> T* at(std::uint32_t address) const {
        return static_cast<T*>(memory.access(address, sizeof(T)));
    }
};

inline void enter(Ctx& ctx) { ctx.entry_esp = ctx.cpu.registers[smbd::esp]; }

inline void skip_return(Ctx& ctx) {
    const auto ret = ctx.memory.load<std::uint32_t>(ctx.entry_esp);
    ctx.cpu.registers[smbd::esp] = ctx.entry_esp + 4 + ctx.skip_pop;
    ctx.cpu.registers[smbd::eax] = ctx.skip_result;
    ctx.cpu.eip = ret;
}

}  // namespace osmod
