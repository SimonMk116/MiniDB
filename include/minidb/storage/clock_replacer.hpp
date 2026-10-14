#pragma once

#include <cstddef>
#include <optional>
#include <vector>

namespace minidb {

using frame_id_t = std::size_t;

// Single-threaded second-chance replacer. Only unpinned frames are eligible.
class ClockReplacer {
public:
    explicit ClockReplacer(std::size_t frame_count);

    void pin(frame_id_t frame_id);
    void unpin(frame_id_t frame_id);
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
