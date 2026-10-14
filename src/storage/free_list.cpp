#include "minidb/storage/free_list.hpp"

#include <stdexcept>

namespace minidb {

// In-memory LIFO stack of reusable page IDs; free-page state is lost on restart.
std::optional<page_id_t> InMemoryFreeList::acquirePage() {
    if (free_pages_.empty()) {
        return std::nullopt;
    }

    const page_id_t page_id = free_pages_.back();
    free_pages_.pop_back();
    available_pages_.erase(page_id);
    return page_id;
}

void InMemoryFreeList::releasePage(page_id_t page_id) {
    const auto [entry, inserted] = available_pages_.insert(page_id);
    if (!inserted) {
        throw std::invalid_argument("page is already in the free list");
    }

    try {
        free_pages_.push_back(page_id);
    } catch (...) {
        available_pages_.erase(entry);
        throw;
    }
}

} // namespace minidb
