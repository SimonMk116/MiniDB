#pragma once

#include "minidb/storage/buffer_pool_manager.hpp"
#include "minidb/storage/slotted_page.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace minidb {

struct RecordId {
    page_id_t page_id{};
    slot_id_t slot_id{};

    bool operator==(const RecordId&) const = default;
};

struct HeapRecord {
    RecordId id;
    std::vector<std::byte> data;
};

// A single-threaded heap table. Its root page ID is the table's durable
// identity; callers retain that ID to reopen the table after a clean shutdown.
class HeapTable {
public:
    // Creates and initializes a table root. Returns nullopt if the buffer pool
    // cannot provide a frame; DiskManager I/O failures propagate.
    static std::optional<page_id_t> create(BufferPoolManager& buffer_pool);

    // Opens a table by its previously returned root page ID. Malformed or
    // unreadable metadata is reported as an exception.
    HeapTable(BufferPoolManager& buffer_pool, page_id_t metadata_root_page_id);

    page_id_t metadataRootPageId() const noexcept;

    // Returns nullopt when a record cannot fit on a fresh SlottedPage.
    // Buffer exhaustion, corrupt pages, and I/O failures throw.
    std::optional<RecordId> insertRecord(std::span<const std::byte> record);

    // Returns an owned copy, or nullopt for a page/slot ID not naming a live
    // record in this table. Corruption and I/O failures throw.
    std::optional<std::vector<std::byte>> getRecord(RecordId id) const;

    // Returns false for a missing or already-deleted record; corruption and I/O
    // failures throw.
    bool deleteRecord(RecordId id);

    // Materializes all live records in page-list and slot-ID order.
    std::vector<HeapRecord> scan() const;

private:
    std::vector<page_id_t> loadDataPageIds() const;
    void appendDataPageId(page_id_t data_page_id);

    BufferPoolManager& buffer_pool_;
    page_id_t metadata_root_page_id_;
};

} // namespace minidb
