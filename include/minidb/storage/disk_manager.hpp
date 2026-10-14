#pragma once

#include "minidb/storage/free_list.hpp"
#include "minidb/storage/page.hpp"

#include <fstream>
#include <memory>
#include <string>

namespace minidb {

// File-backed page store.
//
// A DiskManager treats the database file as a sparse array of fixed-size pages.
// Page IDs are sequential and correspond to byte offsets equal to page_id *
// PAGE_SIZE. The file is extended as pages are allocated, and reusable page IDs
// may be recycled through the configured FreeList strategy.
class DiskManager {
public:
    // Pass a FreeList implementation to select a strategy. If omitted, use
    // the process-local InMemoryFreeList (see docs/free_list.md).
    explicit DiskManager(
        const std::string& filename,
        std::unique_ptr<FreeList> free_list = std::make_unique<InMemoryFreeList>());

    virtual ~DiskManager();

    // Reads a page from disk into the caller-owned Page buffer.
    // Throws std::out_of_range if the page ID is outside the current file.
    virtual void readPage(page_id_t page_id, Page& page);

    // Writes a page to disk at the given page ID.
    // Throws std::out_of_range if the page ID is outside the current file.
    virtual void writePage(page_id_t page_id, const Page& page);

    // Allocates a page ID and extends the file if necessary.
    // Reuses IDs from the configured free list when available.
    page_id_t allocatePage();

    // Releases a page ID back to the free-list strategy for future reuse.
    void freePage(page_id_t page_id);

private:
    std::fstream file_;
    std::unique_ptr<FreeList> free_list_;
    page_id_t next_page_id_{0};
};

} // namespace minidb
