#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace smbd {
// Original guest fields that identify the player's activity. Addresses and
// meanings are recorded in knowledge/discord_presence.txt.
struct GameActivity {
    std::int32_t main_mode=-1,sub_mode=-1,game_type=-1,difficulty=0,stage_id=-1;
    std::uint32_t course_flags=0;
    std::int16_t floor=0,world=0,stage=0,scene=0;
    bool operator==(const GameActivity&) const=default;
};
inline constexpr std::size_t game_activity_bytes=0x6C9368; // Guest RAM prefix that holds every field.
GameActivity read_game_activity(std::span<const std::byte> ram);

struct PresenceText {
    std::string details,state;
    bool operator==(const PresenceText&) const=default;
};
// The original stage names, indexed by stage id like media/stgname/usa.str.
using StageNames=std::vector<std::string>;
StageNames load_stage_names(const std::filesystem::path& disc);
PresenceText describe_activity(const GameActivity&,const StageNames&);

class DiscordPresence {
public:
    explicit DiscordPresence(std::string_view application);
    ~DiscordPresence();
    // Starting to play restarts the elapsed time. Before the game starts, only
    // the title and elapsed time are shown.
    void update(bool enabled,bool playing,const PresenceText& activity);
    std::string_view status() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
