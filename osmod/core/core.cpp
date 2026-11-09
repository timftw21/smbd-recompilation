// SPDX-License-Identifier: MIT
#include "osmod/osmod.h"

namespace {
std::uint64_t frames = 0;
}

std::uint64_t core::frame_count() { return frames; }

// The core subscribes to its own frame event with order 0 and a lower load
// index than any mod, so its count is current when mods run.
void core_frame(osmod::Ctx&) { ++frames; }
