// SPDX-License-Identifier: MIT
#include "osmod_host.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <mutex>
#include <vector>

// The OSMod builder generates osmod_registry(), which calls
// register_mod() for every built-in mod. A stock build has no generated
// registry; the linker then falls back to the empty one below.
extern "C" void osmod_registry_none() {}
#if defined(_MSC_VER)
extern "C" void osmod_registry();
#pragma comment(linker, "/alternatename:osmod_registry=osmod_registry_none")
#else
extern "C" __attribute__((weak, alias("osmod_registry_none"))) void osmod_registry();
#endif

namespace osmod::host {
namespace {
std::vector<const ModInfo*>& list() {
    static std::vector<const ModInfo*> mods;
    return mods;
}
// register_mod() fills list() while this runs, so it must not call registry().
std::vector<const ModInfo*>& registry() {
    static const bool filled = (osmod_registry(), true);
    (void)filled;
    return list();
}
std::mutex files_lock;
std::map<std::string, std::string> active;  // folded from -> to
std::map<std::string, std::filesystem::path> replaced;  // folded from -> host file
std::filesystem::path root;
std::atomic<std::size_t> served{0};

std::string folded(std::string_view path) {
    std::string out(path);
    for (auto& c : out) c = c == '/' ? '\\' : static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}
std::string native(std::string_view path) {
    std::string out(path);
    std::ranges::replace(out, '/', '\\');
    return out;
}
}  // namespace

void register_mod(const ModInfo& mod) { list().push_back(&mod); }
std::span<const ModInfo* const> mods() { return registry(); }

void load(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const auto id = line.substr(0, eq);
        const bool on = line.substr(eq + 1) != "0";
        for (const auto* mod : registry())
            if (mod->enabled && id == mod->id) mod->enabled->store(on, std::memory_order_relaxed);
    }
}

void save(const std::filesystem::path& path) {
    std::ofstream out(path, std::ios::trunc);
    out << "# OSMod switches: mod id = 1 (on) or 0 (off)\n";
    for (const auto* mod : registry())
        if (mod->enabled) out << mod->id << '=' << (mod->enabled->load() ? 1 : 0) << '\n';
}

void set_enabled(const ModInfo& mod, bool on) {
    if (mod.enabled) mod.enabled->store(on, std::memory_order_relaxed);
}

void set_root(const std::filesystem::path& path) {
    std::scoped_lock guard(files_lock);
    root = path;
}

void apply_files() {
    std::map<std::string, std::string> next;
    std::map<std::string, std::filesystem::path> files;
    std::scoped_lock guard(files_lock);
    for (const auto* mod : registry())
        if (enabled(*mod)) {
            for (std::size_t i = 0; i < mod->redirect_count; ++i)
                next[folded(mod->redirects[i].from)] = native(mod->redirects[i].to);
            for (std::size_t i = 0; i < mod->replace_count; ++i)
                files[folded(mod->replaces[i].from)] = root / std::filesystem::path(mod->replaces[i].to);
        }
    active = std::move(next);
    replaced = std::move(files);
}

std::optional<std::filesystem::path> replacement(const std::string& relative) {
    std::scoped_lock guard(files_lock);
    if (replaced.empty()) return std::nullopt;
    const auto found = replaced.find(folded(relative));
    if (found == replaced.end()) return std::nullopt;
    served.fetch_add(1, std::memory_order_relaxed);
    return found->second;
}

std::string redirect(std::string relative) {
    std::scoped_lock guard(files_lock);
    if (active.empty()) return relative;
    const auto found = active.find(folded(relative));
    if (found == active.end()) return relative;
    served.fetch_add(1, std::memory_order_relaxed);
    return found->second;
}

std::size_t redirects_served() { return served.load(); }
std::size_t redirects_active() {
    std::scoped_lock guard(files_lock);
    return active.size() + replaced.size();
}

}  // namespace osmod::host
