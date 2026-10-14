#pragma once

#include "minidb/storage/clock_replacer.hpp"
#include "minidb/storage/disk_manager.hpp"

#include <cstddef>
#include <optional>
#include <unordered_map>
#include <vector>

namespace minidb {

class BufferPoolManager {
public:
    explicit BufferPoolManager(DiskManager& disk_manager, std::size_t pool_size = 16);

    // Returns nullptr when no frame is available. DiskManager exceptions for
    // invalid IDs or I/O failures propagate to the caller. Success pins page.
    Page* fetchPage(page_id_t page_id);

    // Returns false when the page is not resident or is already unpinned.
    bool unpinPage(page_id_t page_id, bool is_dirty);

    // Flushes are allowed while pinned and do not change pin counts. Returns
    // false if the page is not resident; DiskManager I/O exceptions propagate.
    bool flushPage(page_id_t page_id);
    // Attempts every dirty page, then rethrows the first I/O exception, if any.
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
