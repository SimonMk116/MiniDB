#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace minidb {

constexpr std::size_t PAGE_SIZE = 8192;

using page_id_t = std::uint32_t;

class Page {
public:
    Page();

    std::byte* data();
    const std::byte* data() const;

    void reset();

private:
    std::array<std::byte, PAGE_SIZE> data_;
};

} // namespace minidb
