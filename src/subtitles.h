#pragma once
#include "file.h"

namespace smbd {
// Corrections for the US English story and ending subtitles (media/dlg/*_e.bin).
// The original loader 6B26F reads each script whole into one buffer; a
// correction applies only to such a complete read whose SHA-256 matches the
// verified original, so any other data passes through unchanged.
bool correct_subtitles(std::string_view path,std::uint64_t offset,std::span<std::byte> data);
std::string check_subtitles(const std::filesystem::path& disc);
}
