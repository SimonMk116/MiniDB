#pragma once

#include "minidb/storage/clock_replacer.hpp"
#include "minidb/storage/disk_manager.hpp"

#include <cstddef>
#include <optional>
#include <unordered_map>
#include <vector>

namespace minidb {

// In-memory cache for database pages.
//
// The buffer pool keeps a limited number of pages resident in memory and uses a
// clock-based replacement strategy to evict unpinned frames when the cache is
// full. Pages can be pinned to prevent eviction while they are being used.
class BufferPoolManager {
public:
    explicit BufferPoolManager(DiskManager& disk_manager, std::size_t pool_size = 16);

    // Loads a page into the buffer pool and pins it.
    // Returns nullptr when no frame is available for a new page, and propagates
    // any DiskManager read or write errors to the caller.
    Page* fetchPage(page_id_t page_id);

    // Decrements the pin count for a page and marks the frame dirty if requested.
    // Returns false if the page is not resident or already unpinned.
    bool unpinPage(page_id_t page_id, bool is_dirty);

    // Flushes a dirty page to disk while keeping its pin state unchanged.
    // Returns false if the page is not resident; DiskManager I/O exceptions
    // propagate to the caller.
    bool flushPage(page_id_t page_id);

    // Writes every dirty resident page to disk and rethrows the first I/O
    // exception encountered if any write fails.
    void flushAllPages();

    // TODO: Page guards could unpin automatically at scope exit and reduce
    // page-lifecycle mistakes.
    // TODO: This manager is single-threaded. If concurrency is added, protect
    // the page table, frame metadata, pin counts, clock hand, and disk I/O.

private:
    struct Frame {
        Page page;
        std::optional<page_id_t> page_id;
        std::size_t pin_count{0};
        bool dirty{false};
    };

    DiskManager& disk_manager_;
    std::vector<Frame> frames_;
    std::unordered_map<page_id_t, frame_id_t> page_table_;
    std::vector<frame_id_t> free_frames_;
    ClockReplacer replacer_;
};

} // namespace minidb
