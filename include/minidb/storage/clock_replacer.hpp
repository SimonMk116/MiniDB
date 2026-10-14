#pragma once

#include <cstddef>
#include <optional>
#include <vector>

namespace minidb {

using frame_id_t = std::size_t;

// Single-threaded page-frame replacement policy using the clock algorithm.
//
// A frame becomes eligible only after it is unpinned. When the clock hand
// reaches a candidate, a referenced frame is given a second chance and its
// reference bit is cleared. The first unreferenced eligible frame is selected as
// the eviction victim.
class ClockReplacer {
public:
    explicit ClockReplacer(std::size_t frame_count);

    // Marks a frame as pinned so it cannot be evicted.
    void pin(frame_id_t frame_id);

    // Marks a frame as unpinned and eligible for reuse.
    void unpin(frame_id_t frame_id);

    // Returns the next victim frame, or std::nullopt if every frame is pinned.
    std::optional<frame_id_t> victim();

    // TODO: Alternative replacement policies can be added behind a policy
    // interface if the project needs them. This implementation is not
    // thread-safe.

private:
    struct Entry {
        bool eligible{false};
        bool referenced{false};
    };

    std::vector<Entry> entries_;
    frame_id_t hand_{0};
};

} // namespace minidb
