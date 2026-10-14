#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace minidb {

constexpr std::size_t PAGE_SIZE = 8192;

using page_id_t = std::uint32_t;

// A fixed-size in-memory page buffer.
//
// Each page contains exactly PAGE_SIZE bytes and is used as the storage unit for
// the file-backed database. Higher-level code can treat a Page as a raw byte
// array and write records or metadata into its contents.
class Page {
public:
    Page();

    // Returns the writable payload buffer.
    std::byte* data();

    // Returns the read-only payload buffer.
    const std::byte* data() const;

    // Fills the page with zero bytes.
    void reset();

private:
    std::array<std::byte, PAGE_SIZE> data_;
};

} // namespace minidb
