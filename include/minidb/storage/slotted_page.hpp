#pragma once

#include "minidb/storage/page.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

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

    // Returns the directory slot count for a valid page, including deleted
    // slots; returns no value for uninitialized or corrupted page bytes.
    std::optional<std::size_t> slotCount() const noexcept;

    // Inserts a record, reusing the lowest free slot or appending a new slot.
    // If total space is sufficient but fragmented, compacts before insertion.
    // Returns no value for an invalid page or insufficient total space.
    std::optional<slot_id_t> insertRecord(std::span<const std::byte> record);

    // Returns an owned copy of a live record, or no value for an invalid page,
    // out-of-range slot ID, or free slot.
    std::optional<std::vector<std::byte>> getRecord(slot_id_t slot_id) const;

    // Validates once and returns live records in slot-ID order, each as an
    // owned byte vector. Returns no value for an invalid page.
    std::optional<std::vector<std::pair<slot_id_t, std::vector<std::byte>>>> getLiveRecords() const;

    // Marks a live slot free without removing it from the directory. Returns
    // false for an invalid page, out-of-range ID, or already-free slot.
    bool deleteRecord(slot_id_t slot_id);

    // Packs live record bytes toward the page end while preserving slot IDs.
    // Returns false without modifying the page if its current format is invalid.
    bool compact();

private:
    Page& page_;
};

} // namespace minidb
