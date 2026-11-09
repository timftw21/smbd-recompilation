// SPDX-License-Identifier: MIT
#include "osmod/osmod.h"
#include <cstdio>

namespace {
std::uint64_t pad_reads = 0, pad_moves = 0;
}

void hello_pad(osmod::Ctx& ctx) {
    ++pad_reads;
    // order 100: runs after the tilt mods, so it sees their edits.
    if (auto* p = core::pad(ctx); p && (p->lx || p->ly)) ++pad_moves;
}

void hello_frame(osmod::Ctx&) {
    const auto n = core::frame_count();
    if (n % 60) return;
    if (auto* f = std::fopen("osmod-hello.txt", "w")) {
        std::fprintf(f, "frames %llu\npad_reads %llu\npad_reads_with_stick %llu\n",
                     static_cast<unsigned long long>(n), static_cast<unsigned long long>(pad_reads),
                     static_cast<unsigned long long>(pad_moves));
        std::fclose(f);
    }
}
