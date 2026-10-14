#include "minidb/storage/buffer_pool_manager.hpp"
#include "minidb/storage/disk_manager.hpp"

#include <filesystem>
#include <iostream>
#include <string>

int main() {
    const std::string db_path = "/tmp/minidb_demo.db";
    std::filesystem::remove(db_path);

    try {
        minidb::DiskManager disk(db_path);

        const auto page0 = disk.allocatePage();
        const auto page1 = disk.allocatePage();

        minidb::Page first_page;
        first_page.data()[0] = std::byte{0x11};
        first_page.data()[1] = std::byte{0x22};
        first_page.data()[2] = std::byte{0x33};
        disk.writePage(page0, first_page);

        minidb::Page read_back;
        disk.readPage(page0, read_back);

        std::cout << "Allocated pages: " << page0 << ", " << page1 << '\n';
        std::cout << "Page 0 bytes: "
                  << static_cast<int>(read_back.data()[0]) << ' '
                  << static_cast<int>(read_back.data()[1]) << ' '
                  << static_cast<int>(read_back.data()[2]) << '\n';

        minidb::BufferPoolManager pool(disk, 2);
        auto* cached = pool.fetchPage(page0);
        if (cached == nullptr) {
            throw std::runtime_error("Failed to fetch page 0 into the buffer pool");
        }

        cached->data()[3] = std::byte{0x44};
        cached->data()[4] = std::byte{0x55};
        if (!pool.unpinPage(page0, true)) {
            throw std::runtime_error("Failed to unpin dirty page 0");
        }

        if (!pool.flushPage(page0)) {
            throw std::runtime_error("Failed to flush dirty page 0");
        }

        minidb::Page persisted;
        disk.readPage(page0, persisted);
        std::cout << "Persisted bytes after flush: "
                  << static_cast<int>(persisted.data()[0]) << ' '
                  << static_cast<int>(persisted.data()[1]) << ' '
                  << static_cast<int>(persisted.data()[2]) << ' '
                  << static_cast<int>(persisted.data()[3]) << ' '
                  << static_cast<int>(persisted.data()[4]) << '\n';

        std::filesystem::remove(db_path);
        std::cout << "MiniDB storage demo completed successfully\n";
        return 0;
    } catch (const std::exception& ex) {
        std::filesystem::remove(db_path);
        std::cerr << "MiniDB demo failed: " << ex.what() << '\n';
        return 1;
    }
}