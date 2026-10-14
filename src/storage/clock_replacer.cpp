#include "minidb/storage/clock_replacer.hpp"

#include <stdexcept>

namespace minidb {

ClockReplacer::ClockReplacer(std::size_t frame_count) : entries_(frame_count) {}

void ClockReplacer::pin(frame_id_t frame_id) {
    if (frame_id >= entries_.size()) {
        throw std::out_of_range("frame_id is out of range");
    }
    entries_[frame_id] = Entry{};
}

void ClockReplacer::unpin(frame_id_t frame_id) {
    if (frame_id >= entries_.size()) {
        throw std::out_of_range("frame_id is out of range");
    }
    entries_[frame_id] = Entry{true, true};
}

std::optional<frame_id_t> ClockReplacer::victim() {
    if (entries_.empty()) {
        return std::nullopt;
    }

    // One complete pass clears second-chance bits; a following pass can pick
    // an eligible frame. If none is eligible, every frame is pinned.
    for (std::size_t examined = 0; examined < entries_.size() * 2; ++examined) {
        const frame_id_t candidate = hand_;
        hand_ = (hand_ + 1) % entries_.size();

        auto& entry = entries_[candidate];
        if (!entry.eligible) {
            continue;
        }
        if (entry.referenced) {
            entry.referenced = false;
            continue;
        }

        entry.eligible = false;
        return candidate;
    }

    return std::nullopt;
}

} // namespace minidb
