#pragma once

#include "minidb/storage/page.hpp"

#include <optional>
#include <unordered_set>
#include <vector>

namespace minidb {

// Strategy interface for tracking reusable page IDs.
//
// A FreeList does not allocate database pages itself. Instead, it acts as a
// small bookkeeping layer that accepts released page IDs and returns those IDs
// for reuse when DiskManager needs to allocate a new page.
class FreeList {
public:
    virtual ~FreeList() = default;

    // Returns a reusable ID, or std::nullopt when no free page is currently
    // available. The caller is responsible for zeroing the page if it must be
    // reinitialized before reuse.
    virtual std::optional<page_id_t> acquirePage() = 0;

    // Adds an existing page ID back to the reusable set.
    // Implementations may reject duplicate frees by throwing an exception.
    virtual void releasePage(page_id_t page_id) = 0;
};

// A simple, process-local strategy.
//
// This is suitable for an in-memory prototype, but its contents disappear when
// the process exits. It therefore does not provide durable recovery of freed
// page IDs after a restart.
class InMemoryFreeList final : public FreeList {
public:
    std::optional<page_id_t> acquirePage() override;
    void releasePage(page_id_t page_id) override;

private:
    std::vector<page_id_t> free_pages_;
    std::unordered_set<page_id_t> available_pages_;
};

} // namespace minidb
