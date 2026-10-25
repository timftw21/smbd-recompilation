#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <span>

namespace smbd {
// Immutable disc reads, backed by the actual RAM reserved through FscSetCacheSize.
class DiscCache {
public:
    void storage(std::span<std::byte> bytes) { bytes_=bytes; entries_.clear(); cursor_=0; }
    bool read(std::uint64_t offset,std::span<std::byte> output) const {
        for (const auto& entry:entries_) if (offset>=entry.offset && offset-entry.offset<=entry.size &&
            output.size()<=entry.size-(offset-entry.offset)) {
            std::memmove(output.data(),bytes_.data()+entry.at+std::size_t(offset-entry.offset),output.size());
            return true;
        }
        return false;
    }
    void insert(std::uint64_t offset,std::span<const std::byte> input) {
        if (input.empty() || input.size()>bytes_.size()) return;
        if (input.size()>bytes_.size()-cursor_) cursor_=0;
        const auto end=cursor_+input.size();
        std::erase_if(entries_,[&](const Entry& entry) { return cursor_<entry.at+entry.size && entry.at<end; });
        if (entries_.size()==4096) entries_.pop_front();
        std::memmove(bytes_.data()+cursor_,input.data(),input.size());
        entries_.push_back({offset,cursor_,input.size()});
        cursor_=end;
    }
private:
    struct Entry { std::uint64_t offset; std::size_t at,size; };
    std::span<std::byte> bytes_;
    std::deque<Entry> entries_;
    std::size_t cursor_=0;
};
}
