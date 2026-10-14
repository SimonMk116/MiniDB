#include "minidb/storage/disk_manager.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

namespace {

bool check_page_bytes_equal(const std::byte* actual, const std::vector<std::byte>& expected) {
    for (std::size_t i = 0; i < expected.size(); ++i) {
        if (actual[i] != expected[i]) {
            return false;
        }
    }
    return true;
}

std::vector<std::byte> make_pattern(std::byte seed) {
    std::vector<std::byte> pattern(static_cast<std::size_t>(minidb::PAGE_SIZE));
    const auto seed_value = static_cast<unsigned char>(seed);
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        const auto value = static_cast<unsigned char>((seed_value + static_cast<unsigned char>(i % 251)) % 256);
        pattern[i] = static_cast<std::byte>(value);
    }
    return pattern;
}

}  // namespace

int main() {
    const std::filesystem::path db_path = "/tmp/minidb_persistence_test.db";
    std::filesystem::remove(db_path);

    {
        minidb::DiskManager disk_manager(db_path.string());

        std::vector<minidb::page_id_t> page_ids;
        std::vector<std::vector<std::byte>> expected_pages;

        for (int page_index = 0; page_index < 3; ++page_index) {
            const auto page_id = disk_manager.allocatePage();
            page_ids.push_back(page_id);

            minidb::Page page;
            auto* raw = reinterpret_cast<std::byte*>(page.data());
            const auto expected = make_pattern(static_cast<std::byte>(page_index + 1));

            for (std::size_t i = 0; i < expected.size(); ++i) {
                raw[i] = expected[i];
            }

            disk_manager.writePage(page_id, page);
            expected_pages.push_back(expected);
        }

        if (std::filesystem::file_size(db_path) != 3 * minidb::PAGE_SIZE) {
            std::cerr << "DiskManager persistence check failed: file size does not match allocated page count\n";
            return 1;
        }
    }

    {
        minidb::DiskManager reopened_manager(db_path.string());

        for (int page_index = 0; page_index < 3; ++page_index) {
            minidb::Page read_back_page;
            reopened_manager.readPage(static_cast<minidb::page_id_t>(page_index), read_back_page);

            const auto expected = make_pattern(static_cast<std::byte>(page_index + 1));
            const auto* actual = read_back_page.data();

            if (!check_page_bytes_equal(actual, expected)) {
                std::cerr << "DiskManager persistence check failed: page " << page_index << " differs after reopen\n";
                return 1;
            }
        }
    }

    std::filesystem::remove(db_path);
    std::cout << "DiskManager persistence test passed\n";
    return 0;
}
