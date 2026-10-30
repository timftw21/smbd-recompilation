#pragma once
#include "decoder.h"

namespace smbd {
std::string prepare_disc(const std::filesystem::path& image,
                         const std::filesystem::path& output, bool extract_all);
std::string verify_analysis(Xbe& image, const std::filesystem::path& instructions);
#ifdef SMBD_HAS_BATCH
std::string check_cpu(Xbe& image);
std::string probe_cpu(Xbe& image,const std::filesystem::path& capture,const std::filesystem::path& recipe);
#endif
}
