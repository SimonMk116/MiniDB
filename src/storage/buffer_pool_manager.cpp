#include "minidb/storage/buffer_pool_manager.hpp"

#include <exception>
#include <stdexcept>

namespace minidb {

BufferPoolManager::BufferPoolManager(DiskManager& disk_manager, std::size_t pool_size)
    : disk_manager_(disk_manager),
      frames_(pool_size),
      replacer_(pool_size) {
    if (pool_size == 0) {
        throw std::invalid_argument("BufferPoolManager requires at least one frame");
    }

    free_frames_.reserve(pool_size);
    for (frame_id_t frame_id = 0; frame_id < pool_size; ++frame_id) {
        free_frames_.push_back(pool_size - frame_id - 1);
    }
}

Page* BufferPoolManager::fetchPage(page_id_t page_id) {
    if (const auto found = page_table_.find(page_id); found != page_table_.end()) {
        Frame& frame = frames_[found->second];
        ++frame.pin_count;
        replacer_.pin(found->second);
        return &frame.page;
    }

    Page loaded_page;
    // Read into a temporary so a failed disk read cannot expose a partially
    // initialized frame as resident. Disk I/O exceptions are caller-visible.
    disk_manager_.readPage(page_id, loaded_page);

    const bool using_free_frame = !free_frames_.empty();
    frame_id_t frame_id;
    if (using_free_frame) {
        frame_id = free_frames_.back();
    } else {
        const auto selected_victim = replacer_.victim();
        if (!selected_victim) {
            return nullptr;
        }
        frame_id = *selected_victim;

        Frame& victim_frame = frames_[frame_id];
        const page_id_t victim_page_id = *victim_frame.page_id;
        if (victim_frame.dirty) {
            try {
                disk_manager_.writePage(victim_page_id, victim_frame.page);
            } catch (...) {
                // Restore eligibility before propagating the I/O error. The
                // victim remains resident and dirty.
                replacer_.unpin(frame_id);
                throw;
            }
            victim_frame.dirty = false;
        }
        page_table_.erase(victim_page_id);
    }

    Frame& frame = frames_[frame_id];
    frame.page = loaded_page;
    frame.page_id = page_id;
    frame.pin_count = 1;
    frame.dirty = false;
    page_table_.emplace(page_id, frame_id);
    if (using_free_frame) {
        free_frames_.pop_back();
    }
    replacer_.pin(frame_id);
    return &frame.page;
}

Page* BufferPoolManager::newPage(page_id_t& page_id) {
    const bool using_free_frame = !free_frames_.empty();
    frame_id_t frame_id{};
    std::optional<page_id_t> victim_page_id;

    if (using_free_frame) {
        frame_id = free_frames_.back();
    } else {
        const auto victim = replacer_.victim();
        if (!victim) {
            return nullptr;
        }
        frame_id = *victim;
        Frame& frame = frames_[frame_id];
        victim_page_id = frame.page_id;

        if (!victim_page_id) {
            replacer_.unpin(frame_id);
            throw std::logic_error("Clock selected a frame without a resident page");
        }

        if (frame.dirty) {
            try {
                disk_manager_.writePage(*victim_page_id, frame.page);
            } catch (...) {
                replacer_.unpin(frame_id);
                throw;
            }
            frame.dirty = false;
        }
    }

    page_id_t allocated_page_id{};
    try {
        allocated_page_id = disk_manager_.allocatePage();
    } catch (...) {
        if (!using_free_frame) {
            replacer_.unpin(frame_id);
        }
        throw;
    }

    bool inserted = false;
    try {
        inserted = page_table_.emplace(allocated_page_id, frame_id).second;
    } catch (...) {
        // A failed page-table insertion must not consume a database page or
        // make the selected victim disappear from the pool.
        if (!using_free_frame) {
            replacer_.unpin(frame_id);
        }
        try {
            disk_manager_.freePage(allocated_page_id);
        } catch (...) {
            // Preserve the original failure; the allocated page is otherwise
            // unreachable and can be reclaimed by a later allocator policy.
        }
        throw;
    }
    if (!inserted) {
        if (!using_free_frame) {
            replacer_.unpin(frame_id);
        }
        throw std::logic_error("DiskManager allocated a page already resident in the buffer pool");
    }

    if (victim_page_id) {
        page_table_.erase(*victim_page_id);
    }

    Frame& frame = frames_[frame_id];
    frame.page.reset();
    frame.page_id = allocated_page_id;
    frame.pin_count = 1;
    frame.dirty = false;
    if (using_free_frame) {
        free_frames_.pop_back();
    }
    replacer_.pin(frame_id);
    page_id = allocated_page_id;
    return &frame.page;
}

bool BufferPoolManager::unpinPage(page_id_t page_id, bool is_dirty) {
    const auto found = page_table_.find(page_id);
    if (found == page_table_.end()) {
        return false;
    }

    Frame& frame = frames_[found->second];
    if (frame.pin_count == 0) {
        return false;
    }

    --frame.pin_count;
    frame.dirty = frame.dirty || is_dirty;
    if (frame.pin_count == 0) {
        replacer_.unpin(found->second);
    }
    return true;
}

bool BufferPoolManager::flushPage(page_id_t page_id) {
    const auto found = page_table_.find(page_id);
    if (found == page_table_.end()) {
        return false;
    }

    Frame& frame = frames_[found->second];
    if (!frame.dirty) {
        return true;
    }

    disk_manager_.writePage(page_id, frame.page);
    frame.dirty = false;
    return true;
}

void BufferPoolManager::flushAllPages() {
    std::exception_ptr first_error;
    for (const auto& [page_id, frame_id] : page_table_) {
        Frame& frame = frames_[frame_id];
        if (!frame.dirty) {
            continue;
        }

        try {
            disk_manager_.writePage(page_id, frame.page);
            frame.dirty = false;
        } catch (...) {
            if (!first_error) {
                first_error = std::current_exception();
            }
        }
    }
    if (first_error) {
        std::rethrow_exception(first_error);
    }
}

} // namespace minidb
