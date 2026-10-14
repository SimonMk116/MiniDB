#include "minidb/storage/buffer_pool_manager.hpp"

#include <cstddef>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::filesystem::path test_path(const std::string& name) {
    auto path = std::filesystem::temp_directory_path() / ("minidb_" + name + ".db");
    std::filesystem::remove(path);
    return path;
}

void allocate_pages(minidb::DiskManager& disk, minidb::page_id_t count) {
    for (minidb::page_id_t expected = 0; expected < count; ++expected) {
        expect(disk.allocatePage() == expected, "page IDs should be allocated sequentially");
    }
}

void test_fetch_pins_and_unpin_validation() {
    const auto path = test_path("buffer_pool_pins");
    {
        minidb::DiskManager disk(path.string());
        allocate_pages(disk, 2);
        minidb::BufferPoolManager pool(disk, 1);

        auto* first = pool.fetchPage(0);
        expect(first != nullptr, "fetch should load an allocated page");
        expect(pool.fetchPage(0) == first, "cache hit should return the resident frame");
        expect(pool.fetchPage(1) == nullptr, "pinned frame must not be evicted");
        expect(pool.unpinPage(0, false), "first unpin should succeed");
        expect(pool.fetchPage(1) == nullptr, "one remaining pin must prevent eviction");
        expect(pool.unpinPage(0, false), "second unpin should succeed");
        expect(!pool.unpinPage(0, false), "extra unpin should be rejected");
        expect(!pool.unpinPage(99, false), "unpin of a nonresident page should fail");
        expect(pool.fetchPage(1) != nullptr, "unpinned frame should be reusable");
        expect(pool.unpinPage(1, false), "new resident page should unpin");

        bool invalid_fetch_threw = false;
        try {
            (void)pool.fetchPage(99);
        } catch (const std::out_of_range&) {
            invalid_fetch_threw = true;
        }
        expect(invalid_fetch_threw, "invalid page ID should propagate DiskManager error");
    }
    std::filesystem::remove(path);
}

void test_clock_second_chance_and_clean_eviction() {
    const auto path = test_path("buffer_pool_clock");
    {
        minidb::DiskManager disk(path.string());
        allocate_pages(disk, 3);
        minidb::Page stored;
        stored.data()[0] = std::byte{42};
        disk.writePage(0, stored);
        stored.data()[0] = std::byte{84};
        disk.writePage(1, stored);

        minidb::BufferPoolManager pool(disk, 2);
        auto* page0_frame = pool.fetchPage(0);
        auto* page1_frame = pool.fetchPage(1);
        expect(page0_frame != nullptr, "first page should load");
        expect(page1_frame != nullptr, "second page should load");
        expect(pool.unpinPage(0, false), "page 0 should unpin");
        expect(pool.unpinPage(1, false), "page 1 should unpin");

        auto* page2_frame = pool.fetchPage(2);
        expect(page2_frame == page0_frame, "Clock should select eligible page 0 as victim");
        expect(pool.unpinPage(2, false), "page 2 should unpin");

        // The first clock sweep clears both reference bits, then selects page
        // 0. Page 1 therefore remains resident for this cache hit.
        auto* still_resident_page_1 = pool.fetchPage(1);
        expect(still_resident_page_1 == page1_frame && still_resident_page_1->data()[0] == std::byte{84},
               "second-chance sweep should leave page 1 resident");
        expect(pool.unpinPage(1, false), "page 1 should unpin after its cache hit");

        auto* reloaded_page_0 = pool.fetchPage(0);
        expect(reloaded_page_0 == page1_frame && reloaded_page_0->data()[0] == std::byte{42},
               "evicted clean page should reload from disk");
        expect(pool.unpinPage(0, false), "reloaded page should unpin");
    }
    std::filesystem::remove(path);
}

class FailingWriteDiskManager final : public minidb::DiskManager {
public:
    using DiskManager::DiskManager;

    bool fail_writes{true};

    void writePage(minidb::page_id_t page_id, const minidb::Page& page) override {
        if (fail_writes) {
            throw std::runtime_error("injected write failure");
        }
        DiskManager::writePage(page_id, page);
    }
};

void test_dirty_write_failure_preserves_victim() {
    const auto path = test_path("buffer_pool_write_failure");
    {
        FailingWriteDiskManager disk(path.string());
        allocate_pages(disk, 2);
        minidb::BufferPoolManager pool(disk, 1);
        auto* page0 = pool.fetchPage(0);
        expect(page0 != nullptr, "page 0 should load");
        page0->data()[0] = std::byte{123};
        expect(pool.unpinPage(0, true), "modified page should unpin dirty");

        bool flush_failed = false;
        try {
            (void)pool.flushPage(0);
        } catch (const std::runtime_error&) {
            flush_failed = true;
        }
        expect(flush_failed, "flushPage should propagate write failures");

        bool flush_all_failed = false;
        try {
            pool.flushAllPages();
        } catch (const std::runtime_error&) {
            flush_all_failed = true;
        }
        expect(flush_all_failed, "flushAllPages should propagate write failures");

        bool write_failed = false;
        try {
            (void)pool.fetchPage(1);
        } catch (const std::runtime_error&) {
            write_failed = true;
        }
        expect(write_failed, "dirty victim write failure should reach caller");

        auto* still_resident = pool.fetchPage(0);
        expect(still_resident != nullptr && still_resident->data()[0] == std::byte{123},
               "failed write must preserve the dirty victim in memory");
        expect(pool.unpinPage(0, false), "preserved victim should remain pinnable");

        disk.fail_writes = false;
        expect(pool.fetchPage(1) != nullptr, "victim should be evictable after write succeeds");
        minidb::Page persisted;
        disk.DiskManager::readPage(0, persisted);
        expect(persisted.data()[0] == std::byte{123}, "successful retry should persist dirty bytes");
    }
    std::filesystem::remove(path);
}

void test_flush_and_reopen_persistence() {
    const auto path = test_path("buffer_pool_flush");
    {
        minidb::DiskManager disk(path.string());
        allocate_pages(disk, 2);
        minidb::BufferPoolManager pool(disk, 2);

        auto* page0 = pool.fetchPage(0);
        expect(page0 != nullptr, "page 0 should load");
        page0->data()[7] = std::byte{17};
        expect(pool.unpinPage(0, true), "page 0 should unpin dirty");
        expect(pool.flushPage(0), "resident dirty page should flush");

        auto* page1 = pool.fetchPage(1);
        expect(page1 != nullptr, "page 1 should load");
        page1->data()[9] = std::byte{29};
        expect(pool.unpinPage(1, true), "page 1 should unpin dirty");
        expect(!pool.flushPage(99), "flush of nonresident page should return false");

        // Flushing a pinned page is permitted and must not implicitly unpin it.
        auto* pinned_page0 = pool.fetchPage(0);
        expect(pinned_page0 != nullptr, "page 0 should still be resident");
        pinned_page0->data()[8] = std::byte{18};
        expect(pool.unpinPage(0, true), "page 0 modification should be marked dirty");
        expect(pool.fetchPage(0) != nullptr, "page 0 should be fetchable again");
        expect(pool.flushPage(0), "page 0 should flush while pinned");
        expect(pool.unpinPage(0, false), "flush must preserve the page pin count");

        auto* page1_again = pool.fetchPage(1);
        expect(page1_again != nullptr, "page 1 should remain resident");
        page1_again->data()[10] = std::byte{30};
        expect(pool.unpinPage(1, true), "page 1 should remain dirty");
        pool.flushAllPages();
    }

    {
        minidb::DiskManager reopened(path.string());
        minidb::Page page0;
        minidb::Page page1;
        reopened.readPage(0, page0);
        reopened.readPage(1, page1);
        expect(page0.data()[7] == std::byte{17} && page0.data()[8] == std::byte{18},
               "page 0 changes should persist after reopening");
        expect(page1.data()[9] == std::byte{29} && page1.data()[10] == std::byte{30},
               "flush-all changes should persist after reopening");
    }
    std::filesystem::remove(path);
}

} // namespace

int main() {
    try {
        test_fetch_pins_and_unpin_validation();
        test_clock_second_chance_and_clean_eviction();
        test_dirty_write_failure_preserves_victim();
        test_flush_and_reopen_persistence();
        std::cout << "BufferPoolManager tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "BufferPoolManager test failed: " << error.what() << '\n';
        return 1;
    }
}
