#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace smbd {
using InstallProgress=std::function<void(unsigned,std::string_view)>;
std::string verify_install_disc(const std::filesystem::path&,const InstallProgress& progress={});
std::string install_game_files(const std::filesystem::path& iso,const std::filesystem::path& directory,bool copy_disc,
                               const InstallProgress& progress={},const std::atomic_bool* cancel=nullptr);

// Release installers share one payload manifest and installation record format.
inline constexpr std::string_view install_marker="SMBD-INSTALL-V1";
struct PayloadEntry {std::filesystem::path path;std::uint64_t size;std::string digest;};
// Validates a relative path from a payload or installation manifest. Only the
// prepared executable and copied disc image may name data/ (with data=true).
std::filesystem::path install_relative_path(std::string_view value,bool data=false);
std::vector<PayloadEntry> payload_manifest(std::string_view text);
void finish_install_manifest(const std::filesystem::path& root,const std::filesystem::path& staging,
                             const std::set<std::filesystem::path>& previous,const std::set<std::filesystem::path>& installed);
}
