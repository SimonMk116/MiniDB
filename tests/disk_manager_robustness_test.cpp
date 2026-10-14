#include "minidb/storage/disk_manager.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {

void expect_true(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

}  // namespace

int main() {
    const std::filesystem::path db_path = "/tmp/minidb_robustness_test.db";
    std::filesystem::remove(db_path);

    try {
        {
            minidb::DiskManager dm(db_path.string());
            expect_true(dm.allocatePage() == 0, "first allocated page should be 0");
            expect_true(dm.allocatePage() == 1, "second allocated page should be 1");
            expect_true(std::filesystem::file_size(db_path) == 2 * minidb::PAGE_SIZE, "allocated pages should extend the file to the correct size");
        }

        {
            minidb::DiskManager reopened(db_path.string());
            expect_true(reopened.allocatePage() == 2, "after restart, next_page_id_ should continue from file size");
            expect_true(std::filesystem::file_size(db_path) == 3 * minidb::PAGE_SIZE, "new allocation should extend file size again");
        }

        {
            minidb::DiskManager dm(db_path.string());
            minidb::Page page;

            try {
                dm.readPage(999999, page);
                throw std::runtime_error("invalid page_id should throw instead of silently reading");
            } catch (const std::out_of_range&) {
            }

            try {
                dm.writePage(999999, page);
                throw std::runtime_error("invalid page_id should throw instead of silently writing");
            } catch (const std::out_of_range&) {
            }
        }

        {
            std::ofstream file(db_path, std::ios::binary | std::ios::trunc);
            std::array<char, 1024> buffer{};
            file.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            file.close();

            bool threw = false;
            try {
                [[maybe_unused]] minidb::DiskManager invalid_db(db_path.string());
            } catch (const std::runtime_error&) {
                threw = true;
            }
            expect_true(threw, "non-page-aligned database file should be rejected");
        }

        std::filesystem::remove(db_path);
        std::cout << "DiskManager robustness checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "DiskManager robustness check failed: " << e.what() << "\n";
        std::filesystem::remove(db_path);
        return 1;
    }
}
