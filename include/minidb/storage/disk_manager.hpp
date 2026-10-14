#pragma once

#include "minidb/storage/free_list.hpp"
#include "minidb/storage/page.hpp"

#include <fstream>
#include <memory>
#include <string>

namespace minidb {

class DiskManager {
public:
    // Pass a FreeList implementation to select a strategy. If omitted, use
    // the process-local InMemoryFreeList (see docs/free_list.md).
    explicit DiskManager(
        const std::string& filename,
        std::unique_ptr<FreeList> free_list = std::make_unique<InMemoryFreeList>());

    virtual ~DiskManager();

    virtual void readPage(page_id_t page_id, Page& page);

    virtual void writePage(page_id_t page_id, const Page& page);

    page_id_t allocatePage();

    void freePage(page_id_t page_id);

private:
    std::fstream file_;
    std::unique_ptr<FreeList> free_list_;
    page_id_t next_page_id_{0};
};

} // namespace minidb
