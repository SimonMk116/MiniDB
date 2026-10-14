#pragma once

#include "minidb/storage/page.hpp"

#include <optional>
#include <unordered_set>
#include <vector>

namespace minidb {

// Strategy boundary for tracking reusable page IDs. Implementations belong in
// this header/source pair for small strategies, or in a separate
// <strategy>_free_list.hpp/.cpp pair as they grow. This interface does not
// allocate pages; DiskManager grows the database when acquirePage() is empty.
class FreeList {
public:
    virtual ~FreeList() = default;

    // Return a reusable ID, or nullopt when this strategy has none available.
    virtual std::optional<page_id_t> acquirePage() = 0;

    // Make an existing page ID reusable. DiskManager validates the ID range.
    virtual void releasePage(page_id_t page_id) = 0;
};

// A simple, process-local strategy. Its contents are lost when the database
// process exits, so it is useful as a first implementation, not for recovery.
class InMemoryFreeList final : public FreeList {
public:
    std::optional<page_id_t> acquirePage() override;
    void releasePage(page_id_t page_id) override;

private:
    std::vector<page_id_t> free_pages_;
    std::unordered_set<page_id_t> available_pages_;
};

} // namespace minidb
