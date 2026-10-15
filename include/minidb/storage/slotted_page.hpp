#pragma once

#include "minidb/storage/page.hpp"

#include <cstddef>
#include <cstdint>

namespace minidb {

using slot_id_t = std::uint16_t;

// A non-owning view over a Page's bytes. The referenced Page must outlive this
// view. On-page integers are explicitly encoded as little-endian bytes.
class SlottedPage {
public:
    static constexpr std::size_t HEADER_SIZE = 8;
    static constexpr std::size_t SLOT_ENTRY_SIZE = 6;

    enum class SlotState : std::uint8_t {
        free = 0,
        occupied = 1,
    };

    explicit SlottedPage(Page& page) noexcept;

    // Explicitly replaces all page contents with an empty slotted-page format.
    void initialize() noexcept;

    // Returns false for uninitialized, malformed, or unsupported page data;
    // validation never changes the page.
    bool isValid() const noexcept;

private:
    Page& page_;
};

} // namespace minidb
