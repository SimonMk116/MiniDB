#include "minidb/storage/heap_table.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::filesystem::path test_path(const std::string& suffix) {
    auto path = std::filesystem::temp_directory_path() / ("minidb_heap_" + suffix + ".db");
    std::filesystem::remove(path);
    return path;
}

void write_u16_le(std::byte* bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::byte>(value & 0xFFU);
    bytes[offset + 1] = static_cast<std::byte>((value >> 8) & 0xFFU);
}

void write_u32_le(std::byte* bytes, std::size_t offset, std::uint32_t value) {
    for (std::size_t i = 0; i < sizeof(value); ++i) {
        bytes[offset + i] = static_cast<std::byte>((value >> (8 * i)) & 0xFFU);
    }
}

void initialize_metadata(minidb::Page& page, std::uint8_t kind, minidb::page_id_t next) {
    page.reset();
    page.data()[0] = std::byte{'H'};
    page.data()[1] = std::byte{'T'};
    page.data()[2] = std::byte{'B'};
    page.data()[3] = std::byte{'L'};
    page.data()[4] = std::byte{1};
    page.data()[5] = static_cast<std::byte>(kind);
    write_u32_le(page.data(), 8, next);
    write_u16_le(page.data(), 12, 0);
    write_u16_le(page.data(), 14, static_cast<std::uint16_t>((minidb::PAGE_SIZE - 16) / 4));
}

void test_buffer_pool_new_page_and_no_frame_behavior() {
    const auto path = test_path("new_page");
    {
        minidb::DiskManager disk(path.string());
        const auto existing_id = disk.allocatePage();
        minidb::BufferPoolManager pool(disk, 1);
        auto* existing = pool.fetchPage(existing_id);
        expect(existing != nullptr, "existing allocated page should fetch");

        minidb::page_id_t untouched_output = 123;
        expect(pool.newPage(untouched_output) == nullptr,
               "newPage should fail when the only frame is pinned");
        expect(untouched_output == 123, "failed newPage should leave its output ID unchanged");
        expect(std::filesystem::file_size(path) == minidb::PAGE_SIZE,
               "newPage should not allocate a disk page when no frame is available");

        existing->data()[7] = std::byte{0x39};
        expect(pool.unpinPage(existing_id, true), "modified existing page should unpin dirty");
        minidb::page_id_t new_id = 0;
        auto* fresh = pool.newPage(new_id);
        expect(fresh != nullptr && new_id == 1, "newPage should allocate and pin the next page ID");
        expect(std::all_of(fresh->data(), fresh->data() + minidb::PAGE_SIZE,
                           [](std::byte byte) { return byte == std::byte{0}; }),
               "newPage should return a zeroed page");
        fresh->data()[0] = std::byte{0x7A};
        expect(pool.unpinPage(new_id, true), "new page should unpin dirty");
        pool.flushAllPages();

        minidb::Page persisted;
        disk.readPage(new_id, persisted);
        expect(persisted.data()[0] == std::byte{0x7A}, "new page dirty bytes should flush to disk");
        disk.readPage(existing_id, persisted);
        expect(persisted.data()[7] == std::byte{0x39},
               "newPage should write a dirty victim before reusing its frame");
    }
    std::filesystem::remove(path);
}

void test_heap_table_insert_get_delete_and_scan() {
    const auto path = test_path("operations");
    {
        minidb::DiskManager disk(path.string());
        minidb::BufferPoolManager pool(disk, 3);
        const auto root_id = minidb::HeapTable::create(pool);
        expect(root_id.has_value(), "table root should be allocated");
        minidb::HeapTable table(pool, *root_id);
        expect(table.scan().empty(), "new table scan should be empty");
        expect(!table.getRecord({999, 0}), "unknown page ID should be a missing record");

        const std::vector<std::byte> first{std::byte{0}, std::byte{1}, std::byte{0xFF}};
        const std::vector<std::byte> second{std::byte{2}, std::byte{3}};
        const std::vector<std::byte> large(minidb::PAGE_SIZE - 22, std::byte{0xA4});
        const auto first_id = table.insertRecord(first);
        const auto second_id = table.insertRecord(second);
        expect(first_id && second_id && first_id->page_id == second_id->page_id,
               "small records should share a data page");
        const auto large_id = table.insertRecord(large);
        expect(large_id && large_id->page_id != first_id->page_id,
               "record that exceeds remaining space should allocate another data page");

        expect(table.getRecord(*first_id) == first, "binary record should retrieve by composite ID");
        expect(table.getRecord(*large_id) == large, "record on another page should retrieve");
        const auto records = table.scan();
        expect(records.size() == 3, "scan should return all live records across pages");
        expect(records[0].id == *first_id && records[0].data == first,
               "scan should preserve the first record ID and bytes");
        expect(records[2].id == *large_id && records[2].data == large,
               "scan should find the record on the second page");

        expect(table.deleteRecord(*second_id), "deleting an existing record should succeed");
        expect(!table.getRecord(*second_id), "deleted record should be reported missing");
        expect(!table.deleteRecord(*second_id), "deleting the same record twice should return false");
        expect(!table.getRecord({first_id->page_id, 65535}), "invalid slot ID should be missing");
        expect(!table.deleteRecord({first_id->page_id, 65535}), "invalid slot deletion should return false");
        expect(table.scan().size() == 2, "scan should skip deleted slots");

        const std::vector<std::byte> oversized(minidb::PAGE_SIZE, std::byte{0x55});
        expect(!table.insertRecord(oversized), "record too large for an empty page should be rejected");
    }
    std::filesystem::remove(path);
}

void test_empty_record_and_lowest_slot_reuse() {
    const auto path = test_path("empty_reuse");
    {
        minidb::DiskManager disk(path.string());
        minidb::BufferPoolManager pool(disk, 2);
        const auto root = minidb::HeapTable::create(pool);
        expect(root.has_value(), "table should create");
        minidb::HeapTable table(pool, *root);
        const std::vector<std::byte> empty;
        const std::array<std::byte, 1> value{std::byte{0x2A}};
        const auto empty_id = table.insertRecord(empty);
        const auto value_id = table.insertRecord(value);
        expect(empty_id && value_id && empty_id->slot_id == 0 && value_id->slot_id == 1,
               "initial slots should be allocated in order");
        expect(table.deleteRecord(*empty_id), "empty record should be deletable");
        const auto reused = table.insertRecord(value);
        expect(reused && reused->page_id == empty_id->page_id && reused->slot_id == empty_id->slot_id,
               "HeapTable should use SlottedPage's lowest-free-slot reuse");
        expect(table.scan().size() == 2, "scan should return reused and existing live records");
    }
    std::filesystem::remove(path);
}

void test_clean_close_and_reopen() {
    const auto path = test_path("reopen");
    minidb::page_id_t root_id{};
    minidb::RecordId first_id;
    minidb::RecordId second_id;
    const std::vector<std::byte> first{std::byte{0x10}, std::byte{0x11}};
    const std::vector<std::byte> second(minidb::PAGE_SIZE - 15, std::byte{0xD1});
    {
        minidb::DiskManager disk(path.string());
        minidb::BufferPoolManager pool(disk, 2);
        const auto created = minidb::HeapTable::create(pool);
        expect(created.has_value(), "table should create before persistence test");
        root_id = *created;
        minidb::HeapTable table(pool, root_id);
        first_id = *table.insertRecord(first);
        second_id = *table.insertRecord(second);
        expect(first_id.page_id != second_id.page_id, "records should span pages for reopen test");
        pool.flushAllPages();
    }

    {
        minidb::DiskManager disk(path.string());
        minidb::BufferPoolManager pool(disk, 2);
        minidb::HeapTable reopened(pool, root_id);
        expect(reopened.getRecord(first_id) == first, "first record should retrieve after reopening");
        expect(reopened.getRecord(second_id) == second, "second-page record should retrieve after reopening");
        const auto records = reopened.scan();
        expect(records.size() == 2, "page membership and records should survive clean reopen");
    }
    std::filesystem::remove(path);
}

void test_invalid_metadata_headers_and_capacity() {
    for (const std::string mode : {"magic", "version", "capacity", "count", "cycle", "bad_next"}) {
        const auto path = test_path("bad_metadata_" + mode);
        minidb::page_id_t root_id{};
        {
            minidb::DiskManager disk(path.string());
            minidb::BufferPoolManager pool(disk, 2);
            const auto root = minidb::HeapTable::create(pool);
            expect(root.has_value(), "metadata corruption setup should create table");
            root_id = *root;
            pool.flushAllPages();
        }
        {
            minidb::DiskManager disk(path.string());
            minidb::Page root;
            disk.readPage(root_id, root);
            if (mode == "magic") {
                root.data()[0] = std::byte{0};
            } else if (mode == "version") {
                root.data()[4] = std::byte{9};
            } else if (mode == "capacity") {
                write_u16_le(root.data(), 14, 1);
            } else if (mode == "count") {
                write_u16_le(root.data(), 12, 0xFFFF);
            } else if (mode == "bad_next") {
                write_u32_le(root.data(), 8, 1000);
            } else {
                write_u32_le(root.data(), 8, root_id);
            }
            disk.writePage(root_id, root);
        }
        {
            minidb::DiskManager disk(path.string());
            minidb::BufferPoolManager pool(disk, 2);
            bool rejected = false;
            try {
                minidb::HeapTable malformed(pool, root_id);
            } catch (const std::runtime_error&) {
                rejected = true;
            }
            expect(rejected, "malformed metadata " + mode + " should be rejected");
        }
        std::filesystem::remove(path);
    }
}

void test_linked_metadata_page_membership() {
    const auto path = test_path("metadata_chain");
    minidb::page_id_t root_id{};
    {
        minidb::DiskManager disk(path.string());
        root_id = disk.allocatePage();
        const auto continuation_id = disk.allocatePage();
        const auto data_id = disk.allocatePage();

        minidb::Page root;
        initialize_metadata(root, 1, continuation_id);
        minidb::Page continuation;
        initialize_metadata(continuation, 2, std::numeric_limits<minidb::page_id_t>::max());
        write_u16_le(continuation.data(), 12, 1);
        write_u32_le(continuation.data(), 16, data_id);
        minidb::Page data;
        minidb::SlottedPage slotted(data);
        slotted.initialize();
        disk.writePage(root_id, root);
        disk.writePage(continuation_id, continuation);
        disk.writePage(data_id, data);
    }
    {
        minidb::DiskManager disk(path.string());
        minidb::BufferPoolManager pool(disk, 1);
        minidb::HeapTable table(pool, root_id);
        const std::array<std::byte, 2> payload{std::byte{0x31}, std::byte{0x32}};
        const auto id = table.insertRecord(payload);
        expect(id && table.scan().size() == 1,
               "table should load data-page membership from linked metadata pages");
        expect(table.getRecord(*id) == std::vector<std::byte>(payload.begin(), payload.end()),
               "record inserted through a linked metadata chain should retrieve");
    }
    std::filesystem::remove(path);
}

void test_multi_page_metadata_cycle_is_rejected() {
    const auto path = test_path("metadata_cycle");
    minidb::page_id_t root_id{};
    {
        minidb::DiskManager disk(path.string());
        root_id = disk.allocatePage();
        const auto continuation_id = disk.allocatePage();
        const auto data_id = disk.allocatePage();

        minidb::Page root;
        initialize_metadata(root, 1, continuation_id);
        minidb::Page continuation;
        initialize_metadata(continuation, 2, root_id);
        minidb::Page data;
        minidb::SlottedPage slotted(data);
        slotted.initialize();
        disk.writePage(root_id, root);
        disk.writePage(continuation_id, continuation);
        disk.writePage(data_id, data);
    }
    {
        minidb::DiskManager disk(path.string());
        minidb::BufferPoolManager pool(disk, 1);
        bool cycle_rejected = false;
        try {
            minidb::HeapTable malformed(pool, root_id);
        } catch (const std::runtime_error&) {
            cycle_rejected = true;
        }
        expect(cycle_rejected, "multi-page metadata chain cycle should be rejected");
    }
    std::filesystem::remove(path);
}

void test_duplicate_data_page_ids_and_corrupt_slotted_page() {
    const auto path = test_path("duplicate_and_corrupt");
    minidb::page_id_t root_id{};
    minidb::page_id_t first_page_id{};
    minidb::page_id_t second_page_id{};
    {
        minidb::DiskManager disk(path.string());
        minidb::BufferPoolManager pool(disk, 2);
        const auto root = minidb::HeapTable::create(pool);
        expect(root.has_value(), "table should create");
        root_id = *root;
        minidb::HeapTable table(pool, root_id);
        const std::vector<std::byte> large(minidb::PAGE_SIZE - 128, std::byte{0x44});
        first_page_id = table.insertRecord(large)->page_id;
        second_page_id = table.insertRecord(large)->page_id;
        expect(first_page_id != second_page_id, "setup should create two data pages");
        pool.flushAllPages();
    }

    {
        minidb::DiskManager disk(path.string());
        minidb::Page root;
        disk.readPage(root_id, root);
        write_u32_le(root.data(), 20, first_page_id); // second entry duplicates first
        disk.writePage(root_id, root);
    }
    {
        minidb::DiskManager disk(path.string());
        minidb::BufferPoolManager pool(disk, 2);
        bool duplicate_rejected = false;
        try {
            minidb::HeapTable malformed(pool, root_id);
        } catch (const std::runtime_error&) {
            duplicate_rejected = true;
        }
        expect(duplicate_rejected, "duplicate data-page IDs should be rejected");
    }

    {
        minidb::DiskManager disk(path.string());
        minidb::Page root;
        disk.readPage(root_id, root);
        write_u32_le(root.data(), 20, second_page_id);
        disk.writePage(root_id, root);
        minidb::Page data;
        disk.readPage(first_page_id, data);
        data.data()[0] = std::byte{0};
        disk.writePage(first_page_id, data);
    }
    {
        minidb::DiskManager disk(path.string());
        minidb::BufferPoolManager pool(disk, 1);
        minidb::HeapTable table(pool, root_id);
        bool corrupt_page_rejected = false;
        try {
            (void)table.scan();
        } catch (const std::runtime_error&) {
            corrupt_page_rejected = true;
        }
        expect(corrupt_page_rejected, "corrupted SlottedPage should not be treated as empty");
        // With a one-frame pool, a second fetch succeeding proves the failed
        // scan released its page pin on the exception path.
        auto* page = pool.fetchPage(first_page_id);
        expect(page != nullptr, "corrupt-page scan should not leave its frame pinned");
        expect(pool.unpinPage(first_page_id, false), "fetched corrupt page should unpin");
        auto* metadata = pool.fetchPage(root_id);
        expect(metadata != nullptr,
               "exception path should release its only frame for a different page");
        expect(pool.unpinPage(root_id, false), "metadata page fetched after exception should unpin");
    }
    std::filesystem::remove(path);
}

void test_oversized_insert_does_not_allocate() {
    const auto path = test_path("oversized");
    {
        minidb::DiskManager disk(path.string());
        minidb::BufferPoolManager pool(disk, 2);
        const auto root = minidb::HeapTable::create(pool);
        expect(root.has_value(), "table should create");
        minidb::HeapTable table(pool, *root);
        const std::vector<std::byte> oversized(minidb::PAGE_SIZE, std::byte{0xE1});
        expect(!table.insertRecord(oversized), "oversized record should be rejected");
        pool.flushAllPages();
        expect(disk.allocatePage() == 1, "rejected oversized record should not allocate a data page");
    }
    std::filesystem::remove(path);
}

} // namespace

int main() {
    try {
        test_buffer_pool_new_page_and_no_frame_behavior();
        test_heap_table_insert_get_delete_and_scan();
        test_empty_record_and_lowest_slot_reuse();
        test_clean_close_and_reopen();
        test_invalid_metadata_headers_and_capacity();
        test_linked_metadata_page_membership();
        test_multi_page_metadata_cycle_is_rejected();
        test_duplicate_data_page_ids_and_corrupt_slotted_page();
        test_oversized_insert_does_not_allocate();
        std::cout << "HeapTable tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "HeapTable test failed: " << error.what() << '\n';
        return 1;
    }
}
