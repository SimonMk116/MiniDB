#include "minidb/storage/slotted_page.hpp"

#include <cstddef>

namespace minidb {
namespace {

constexpr std::size_t MAGIC_OFFSET = 0;
constexpr std::size_t VERSION_OFFSET = 2;
constexpr std::size_t HEADER_RESERVED_OFFSET = 3;
constexpr std::size_t SLOT_COUNT_OFFSET = 4;
constexpr std::size_t DATA_BOUNDARY_OFFSET = 6;
constexpr std::uint8_t FORMAT_VERSION = 1;
constexpr std::byte MAGIC_FIRST{0x53}; // 'S'
constexpr std::byte MAGIC_SECOND{0x50}; // 'P'

constexpr std::size_t SLOT_OFFSET_FIELD = 0;
constexpr std::size_t SLOT_LENGTH_FIELD = 2;
constexpr std::size_t SLOT_STATE_FIELD = 4;
constexpr std::size_t SLOT_RESERVED_FIELD = 5;

std::uint16_t read_u16_le(const std::byte* bytes, std::size_t offset) noexcept {
    const auto low = std::to_integer<std::uint16_t>(bytes[offset]);
    const auto high = std::to_integer<std::uint16_t>(bytes[offset + 1]);
    return static_cast<std::uint16_t>(low | static_cast<std::uint16_t>(high << 8));
}

void write_u16_le(std::byte* bytes, std::size_t offset, std::uint16_t value) noexcept {
    bytes[offset] = static_cast<std::byte>(value & 0x00FFU);
    bytes[offset + 1] = static_cast<std::byte>((value >> 8) & 0x00FFU);
}

} // namespace

SlottedPage::SlottedPage(Page& page) noexcept : page_(page) {}

void SlottedPage::initialize() noexcept {
    page_.reset();
    std::byte* const bytes = page_.data();
    bytes[MAGIC_OFFSET] = MAGIC_FIRST;
    bytes[MAGIC_OFFSET + 1] = MAGIC_SECOND;
    bytes[VERSION_OFFSET] = static_cast<std::byte>(FORMAT_VERSION);
    bytes[HEADER_RESERVED_OFFSET] = std::byte{0};
    write_u16_le(bytes, SLOT_COUNT_OFFSET, 0);
    write_u16_le(bytes, DATA_BOUNDARY_OFFSET, static_cast<std::uint16_t>(PAGE_SIZE));
}

bool SlottedPage::isValid() const noexcept {
    const std::byte* const bytes = page_.data();
    if (bytes[MAGIC_OFFSET] != MAGIC_FIRST || bytes[MAGIC_OFFSET + 1] != MAGIC_SECOND ||
        std::to_integer<std::uint8_t>(bytes[VERSION_OFFSET]) != FORMAT_VERSION ||
        bytes[HEADER_RESERVED_OFFSET] != std::byte{0}) {
        return false;
    }

    const std::size_t slot_count = read_u16_le(bytes, SLOT_COUNT_OFFSET);
    const std::size_t data_boundary = read_u16_le(bytes, DATA_BOUNDARY_OFFSET);
    const std::size_t maximum_slot_count = (PAGE_SIZE - HEADER_SIZE) / SLOT_ENTRY_SIZE;
    if (slot_count > maximum_slot_count) {
        return false;
    }

    // Check count before multiplication; this also keeps the directory-end
    // calculation bounded by the fixed page size.
    const std::size_t directory_end = HEADER_SIZE + slot_count * SLOT_ENTRY_SIZE;
    if (data_boundary < directory_end || data_boundary > PAGE_SIZE) {
        return false;
    }

    for (std::size_t slot_index = 0; slot_index < slot_count; ++slot_index) {
        const std::size_t entry = HEADER_SIZE + slot_index * SLOT_ENTRY_SIZE;
        const std::size_t offset = read_u16_le(bytes, entry + SLOT_OFFSET_FIELD);
        const std::size_t length = read_u16_le(bytes, entry + SLOT_LENGTH_FIELD);
        const auto state = std::to_integer<std::uint8_t>(bytes[entry + SLOT_STATE_FIELD]);
        if (bytes[entry + SLOT_RESERVED_FIELD] != std::byte{0}) {
            return false;
        }

        if (state == static_cast<std::uint8_t>(SlotState::free)) {
            if (offset != 0 || length != 0) {
                return false;
            }
            continue;
        }
        if (state != static_cast<std::uint8_t>(SlotState::occupied)) {
            return false;
        }

        // offset is bounded before subtracting it from PAGE_SIZE, avoiding
        // overflow in offset + length. Empty records may point at PAGE_SIZE.
        if (offset < data_boundary || offset > PAGE_SIZE || length > PAGE_SIZE - offset) {
            return false;
        }

        if (length == 0) {
            continue;
        }
        const std::size_t record_end = offset + length; // safe after the check above
        for (std::size_t earlier = 0; earlier < slot_index; ++earlier) {
            const std::size_t earlier_entry = HEADER_SIZE + earlier * SLOT_ENTRY_SIZE;
            const auto earlier_state =
                std::to_integer<std::uint8_t>(bytes[earlier_entry + SLOT_STATE_FIELD]);
            if (earlier_state != static_cast<std::uint8_t>(SlotState::occupied)) {
                continue;
            }

            const std::size_t earlier_offset = read_u16_le(bytes, earlier_entry + SLOT_OFFSET_FIELD);
            const std::size_t earlier_length = read_u16_le(bytes, earlier_entry + SLOT_LENGTH_FIELD);
            if (earlier_length == 0) {
                continue;
            }
            const std::size_t earlier_end = earlier_offset + earlier_length;
            if (offset < earlier_end && earlier_offset < record_end) {
                return false;
            }
        }
    }

    return true;
}

} // namespace minidb
