#include "minidb/storage/slotted_page.hpp"

#include <cstddef>
#include <cstring>
#include <utility>
#include <vector>

namespace minidb
{
    namespace
    {

        constexpr std::size_t MAGIC_OFFSET = 0;
        constexpr std::size_t VERSION_OFFSET = 2;
        constexpr std::size_t HEADER_RESERVED_OFFSET = 3;
        constexpr std::size_t SLOT_COUNT_OFFSET = 4;
        constexpr std::size_t DATA_BOUNDARY_OFFSET = 6;
        constexpr std::uint8_t FORMAT_VERSION = 1;
        constexpr std::byte MAGIC_FIRST{0x53};  // 'S'
        constexpr std::byte MAGIC_SECOND{0x50}; // 'P'

        constexpr std::size_t SLOT_OFFSET_FIELD = 0;
        constexpr std::size_t SLOT_LENGTH_FIELD = 2;
        constexpr std::size_t SLOT_STATE_FIELD = 4;
        constexpr std::size_t SLOT_RESERVED_FIELD = 5;

        std::uint16_t read_u16_le(const std::byte *bytes, std::size_t offset) noexcept
        {
            const auto low = std::to_integer<std::uint16_t>(bytes[offset]);
            const auto high = std::to_integer<std::uint16_t>(bytes[offset + 1]);
            return static_cast<std::uint16_t>(low | static_cast<std::uint16_t>(high << 8));
        }

        void write_u16_le(std::byte *bytes, std::size_t offset, std::uint16_t value) noexcept
        {
            bytes[offset] = static_cast<std::byte>(value & 0x00FFU);
            bytes[offset + 1] = static_cast<std::byte>((value >> 8) & 0x00FFU);
        }

    } // namespace

    SlottedPage::SlottedPage(Page &page) noexcept : page_(page) {}

    void SlottedPage::initialize() noexcept
    {
        page_.reset();
        std::byte *const bytes = page_.data();
        bytes[MAGIC_OFFSET] = MAGIC_FIRST;
        bytes[MAGIC_OFFSET + 1] = MAGIC_SECOND;
        bytes[VERSION_OFFSET] = static_cast<std::byte>(FORMAT_VERSION);
        bytes[HEADER_RESERVED_OFFSET] = std::byte{0};
        write_u16_le(bytes, SLOT_COUNT_OFFSET, 0);
        write_u16_le(bytes, DATA_BOUNDARY_OFFSET, static_cast<std::uint16_t>(PAGE_SIZE));
    }

    bool SlottedPage::isValid() const noexcept
    {
        const std::byte *const bytes = page_.data();
        if (bytes[MAGIC_OFFSET] != MAGIC_FIRST || bytes[MAGIC_OFFSET + 1] != MAGIC_SECOND ||
            std::to_integer<std::uint8_t>(bytes[VERSION_OFFSET]) != FORMAT_VERSION ||
            bytes[HEADER_RESERVED_OFFSET] != std::byte{0})
        {
            return false;
        }

        const std::size_t slot_count = read_u16_le(bytes, SLOT_COUNT_OFFSET);
        const std::size_t data_boundary = read_u16_le(bytes, DATA_BOUNDARY_OFFSET);
        const std::size_t maximum_slot_count = (PAGE_SIZE - HEADER_SIZE) / SLOT_ENTRY_SIZE;
        if (slot_count > maximum_slot_count)
        {
            return false;
        }

        // The maximum count check bounds this multiplication by PAGE_SIZE.
        const std::size_t directory_end = HEADER_SIZE + slot_count * SLOT_ENTRY_SIZE;
        if (data_boundary < directory_end || data_boundary > PAGE_SIZE)
        {
            return false;
        }

        std::size_t lowest_record_offset = PAGE_SIZE;
        bool has_nonempty_record = false;

        // Pass one validates every slot before any record ranges are compared.
        for (std::size_t slot_index = 0; slot_index < slot_count; ++slot_index)
        {
            const std::size_t entry = HEADER_SIZE + slot_index * SLOT_ENTRY_SIZE;
            const std::size_t offset = read_u16_le(bytes, entry + SLOT_OFFSET_FIELD);
            const std::size_t length = read_u16_le(bytes, entry + SLOT_LENGTH_FIELD);
            const auto state = std::to_integer<std::uint8_t>(bytes[entry + SLOT_STATE_FIELD]);
            if (bytes[entry + SLOT_RESERVED_FIELD] != std::byte{0})
            {
                return false;
            }

            if (state == static_cast<std::uint8_t>(SlotState::free))
            {
                if (offset != 0 || length != 0)
                {
                    return false;
                }
                continue;
            }
            if (state != static_cast<std::uint8_t>(SlotState::occupied))
            {
                return false;
            }

            // Empty records have a canonical representation and occupy no
            // bytes, so they do not affect the data boundary.
            if (length == 0)
            {
                if (offset != PAGE_SIZE)
                {
                    return false;
                }
                continue;
            }

            // The record must begin beyond the directory and at/above the
            // declared data boundary. Check offset before subtracting it from
            // PAGE_SIZE so the length check cannot underflow or overflow.
            if (offset < directory_end || offset < data_boundary || offset > PAGE_SIZE ||
                length > PAGE_SIZE - offset)
            {
                return false;
            }

            has_nonempty_record = true;
            if (offset < lowest_record_offset)
            {
                lowest_record_offset = offset;
            }
        }

        // data_boundary is exactly the start of the lowest non-empty record;
        // with no non-empty records, including pages with only empty records,
        // it is PAGE_SIZE.
        const std::size_t expected_boundary = has_nonempty_record ? lowest_record_offset : PAGE_SIZE;
        if (data_boundary != expected_boundary)
        {
            return false;
        }

        // Pass two compares only non-empty occupied records. Pass one proved
        // each offset and length is in bounds, so these end calculations are
        // safe. Half-open ranges allow adjacent records without overlap.
        for (std::size_t first = 0; first < slot_count; ++first)
        {
            const std::size_t first_entry = HEADER_SIZE + first * SLOT_ENTRY_SIZE;
            if (std::to_integer<std::uint8_t>(bytes[first_entry + SLOT_STATE_FIELD]) !=
                static_cast<std::uint8_t>(SlotState::occupied))
            {
                continue;
            }
            const std::size_t first_length = read_u16_le(bytes, first_entry + SLOT_LENGTH_FIELD);
            if (first_length == 0)
            {
                continue;
            }
            const std::size_t first_offset = read_u16_le(bytes, first_entry + SLOT_OFFSET_FIELD);
            const std::size_t first_end = first_offset + first_length;

            for (std::size_t second = first + 1; second < slot_count; ++second)
            {
                const std::size_t second_entry = HEADER_SIZE + second * SLOT_ENTRY_SIZE;
                if (std::to_integer<std::uint8_t>(bytes[second_entry + SLOT_STATE_FIELD]) !=
                    static_cast<std::uint8_t>(SlotState::occupied))
                {
                    continue;
                }
                const std::size_t second_length = read_u16_le(bytes, second_entry + SLOT_LENGTH_FIELD);
                if (second_length == 0)
                {
                    continue;
                }
                const std::size_t second_offset = read_u16_le(bytes, second_entry + SLOT_OFFSET_FIELD);
                const std::size_t second_end = second_offset + second_length;

                if (first_offset < second_end && second_offset < first_end)
                {
                    return false;
                }
            }
        }

        return true;
    }

    std::optional<std::size_t> SlottedPage::slotCount() const noexcept
    {
        if (!isValid())
        {
            return std::nullopt;
        }
        return read_u16_le(page_.data(), SLOT_COUNT_OFFSET);
    }

    std::optional<slot_id_t> SlottedPage::insertRecord(std::span<const std::byte> record)
    {
        if (!isValid() || record.size() > PAGE_SIZE)
        {
            return std::nullopt;
        }

        std::byte *const bytes = page_.data();
        const std::size_t slot_count = read_u16_le(bytes, SLOT_COUNT_OFFSET);
        const std::size_t data_boundary = read_u16_le(bytes, DATA_BOUNDARY_OFFSET);
        const std::size_t maximum_slot_count = (PAGE_SIZE - HEADER_SIZE) / SLOT_ENTRY_SIZE;

        std::size_t reusable_slot = slot_count;
        std::size_t live_bytes = 0;
        for (std::size_t slot = 0; slot < slot_count; ++slot)
        {
            const std::size_t entry = HEADER_SIZE + slot * SLOT_ENTRY_SIZE;
            const auto state = std::to_integer<std::uint8_t>(bytes[entry + SLOT_STATE_FIELD]);
            if (state == static_cast<std::uint8_t>(SlotState::free))
            {
                if (reusable_slot == slot_count)
                {
                    reusable_slot = slot;
                }
                continue;
            }
            const std::size_t length = read_u16_le(bytes, entry + SLOT_LENGTH_FIELD);
            if (live_bytes > PAGE_SIZE || length > PAGE_SIZE - live_bytes)
            {
                return std::nullopt;
            }
            live_bytes += length;
        }

        const std::size_t directory_end = HEADER_SIZE + slot_count * SLOT_ENTRY_SIZE;
        const bool appending_slot = reusable_slot == slot_count;
        if (appending_slot && slot_count >= maximum_slot_count)
        {
            return std::nullopt;
        }

        const std::size_t slot_bytes = appending_slot ? SLOT_ENTRY_SIZE : 0;
        const std::size_t total_capacity = PAGE_SIZE - directory_end;
        if (live_bytes > total_capacity)
        {
            return std::nullopt;
        }
        const std::size_t total_free = total_capacity - live_bytes;
        if (record.size() > total_free || slot_bytes > total_free - record.size())
        {
            // Check total reclaimable space before compaction. A failed insert
            // must leave even fragmented pages byte-for-byte unchanged.
            return std::nullopt;
        }

        const std::size_t required_contiguous = record.size() + slot_bytes;
        bool needs_compaction = false;
        if (data_boundary < directory_end)
        {
            return std::nullopt;
        }
        needs_compaction = required_contiguous > data_boundary - directory_end;

        std::vector<std::byte> stable_record;
        std::span<const std::byte> record_bytes = record;
        if (needs_compaction)
        {
            // The input span may point into this page. Preserve it before
            // compaction rewrites page bytes; allocation failure is still
            // before any page mutation.
            stable_record.assign(record.begin(), record.end());
            record_bytes = stable_record;
            if (!compact())
            {
                return std::nullopt;
            }
        }

        // Total-space preflight guarantees compaction leaves enough contiguous
        // room. From here onward the operation has no failing steps.
        const std::size_t current_boundary = read_u16_le(page_.data(), DATA_BOUNDARY_OFFSET);
        const std::size_t new_data_boundary = current_boundary - record_bytes.size();
        const slot_id_t slot_id = static_cast<slot_id_t>(reusable_slot);
        const std::size_t entry = HEADER_SIZE + reusable_slot * SLOT_ENTRY_SIZE;
        const std::size_t record_offset = record.empty() ? PAGE_SIZE : new_data_boundary;
        if (!record_bytes.empty())
        {
            std::memmove(page_.data() + record_offset, record_bytes.data(), record_bytes.size());
        }

        std::byte *const updated_bytes = page_.data();
        write_u16_le(updated_bytes, entry + SLOT_OFFSET_FIELD, static_cast<std::uint16_t>(record_offset));
        write_u16_le(updated_bytes, entry + SLOT_LENGTH_FIELD, static_cast<std::uint16_t>(record_bytes.size()));
        updated_bytes[entry + SLOT_STATE_FIELD] = static_cast<std::byte>(SlotState::occupied);
        updated_bytes[entry + SLOT_RESERVED_FIELD] = std::byte{0};
        if (appending_slot)
        {
            write_u16_le(updated_bytes, SLOT_COUNT_OFFSET, static_cast<std::uint16_t>(slot_count + 1));
        }
        write_u16_le(updated_bytes, DATA_BOUNDARY_OFFSET, static_cast<std::uint16_t>(new_data_boundary));
        return slot_id;
    }

    std::optional<std::vector<std::byte>> SlottedPage::getRecord(slot_id_t slot_id) const
    {
        if (!isValid())
        {
            return std::nullopt;
        }

        const std::byte *const bytes = page_.data();
        const std::size_t slot_count = read_u16_le(bytes, SLOT_COUNT_OFFSET);
        if (static_cast<std::size_t>(slot_id) >= slot_count)
        {
            return std::nullopt;
        }

        const std::size_t entry = HEADER_SIZE + static_cast<std::size_t>(slot_id) * SLOT_ENTRY_SIZE;
        if (std::to_integer<std::uint8_t>(bytes[entry + SLOT_STATE_FIELD]) !=
            static_cast<std::uint8_t>(SlotState::occupied))
        {
            return std::nullopt;
        }

        const std::size_t offset = read_u16_le(bytes, entry + SLOT_OFFSET_FIELD);
        const std::size_t length = read_u16_le(bytes, entry + SLOT_LENGTH_FIELD);
        if (length == 0)
        {
            return std::vector<std::byte>{};
        }
        return std::vector<std::byte>(bytes + offset, bytes + offset + length);
    }

    std::optional<std::vector<std::pair<slot_id_t, std::vector<std::byte>>>>
    SlottedPage::getLiveRecords() const
    {
        if (!isValid())
        {
            return std::nullopt;
        }

        const std::byte *const bytes = page_.data();
        const std::size_t slot_count = read_u16_le(bytes, SLOT_COUNT_OFFSET);
        std::vector<std::pair<slot_id_t, std::vector<std::byte>>> records;
        records.reserve(slot_count);
        for (std::size_t slot = 0; slot < slot_count; ++slot)
        {
            const std::size_t entry = HEADER_SIZE + slot * SLOT_ENTRY_SIZE;
            if (std::to_integer<std::uint8_t>(bytes[entry + SLOT_STATE_FIELD]) !=
                static_cast<std::uint8_t>(SlotState::occupied))
            {
                continue;
            }

            const std::size_t offset = read_u16_le(bytes, entry + SLOT_OFFSET_FIELD);
            const std::size_t length = read_u16_le(bytes, entry + SLOT_LENGTH_FIELD);
            std::vector<std::byte> record;
            if (length != 0)
            {
                record.assign(bytes + offset, bytes + offset + length);
            }
            records.emplace_back(static_cast<slot_id_t>(slot), std::move(record));
        }
        return records;
    }

    bool SlottedPage::deleteRecord(slot_id_t slot_id)
    {
        if (!isValid())
        {
            return false;
        }

        std::byte *const bytes = page_.data();
        const std::size_t slot_count = read_u16_le(bytes, SLOT_COUNT_OFFSET);
        const std::size_t slot = static_cast<std::size_t>(slot_id);
        if (slot >= slot_count)
        {
            return false;
        }

        const std::size_t entry = HEADER_SIZE + slot * SLOT_ENTRY_SIZE;
        if (std::to_integer<std::uint8_t>(bytes[entry + SLOT_STATE_FIELD]) !=
            static_cast<std::uint8_t>(SlotState::occupied))
        {
            return false;
        }

        // A deleted slot has a canonical empty extent. Its old payload bytes
        // become reclaimable fragmentation until compaction.
        write_u16_le(bytes, entry + SLOT_OFFSET_FIELD, 0);
        write_u16_le(bytes, entry + SLOT_LENGTH_FIELD, 0);
        bytes[entry + SLOT_STATE_FIELD] = static_cast<std::byte>(SlotState::free);

        std::size_t lowest_record_offset = PAGE_SIZE;
        for (std::size_t current = 0; current < slot_count; ++current)
        {
            const std::size_t current_entry = HEADER_SIZE + current * SLOT_ENTRY_SIZE;
            if (std::to_integer<std::uint8_t>(bytes[current_entry + SLOT_STATE_FIELD]) !=
                static_cast<std::uint8_t>(SlotState::occupied))
            {
                continue;
            }

            const std::size_t length = read_u16_le(bytes, current_entry + SLOT_LENGTH_FIELD);
            if (length == 0)
            {
                continue;
            }
            const std::size_t offset = read_u16_le(bytes, current_entry + SLOT_OFFSET_FIELD);
            if (offset < lowest_record_offset)
            {
                lowest_record_offset = offset;
            }
        }
        write_u16_le(bytes, DATA_BOUNDARY_OFFSET, static_cast<std::uint16_t>(lowest_record_offset));
        return true;
    }

    bool SlottedPage::compact()
    {
        if (!isValid())
        {
            return false;
        }

        const std::byte *const source = page_.data();
        const std::size_t slot_count = read_u16_le(source, SLOT_COUNT_OFFSET);
        const std::size_t directory_end = HEADER_SIZE + slot_count * SLOT_ENTRY_SIZE;

        // Build the complete compacted image separately. This avoids copying
        // records over source bytes that have not yet been moved.
        Page compacted_page;
        std::byte *const compacted = compacted_page.data();
        std::memcpy(compacted, source, directory_end);

        std::size_t cursor = PAGE_SIZE;
        for (std::size_t slot = 0; slot < slot_count; ++slot)
        {
            const std::size_t entry = HEADER_SIZE + slot * SLOT_ENTRY_SIZE;
            if (std::to_integer<std::uint8_t>(source[entry + SLOT_STATE_FIELD]) !=
                static_cast<std::uint8_t>(SlotState::occupied))
            {
                continue;
            }

            const std::size_t length = read_u16_le(source, entry + SLOT_LENGTH_FIELD);
            if (length == 0)
            {
                continue;
            }

            const std::size_t old_offset = read_u16_le(source, entry + SLOT_OFFSET_FIELD);
            cursor -= length;
            std::memcpy(compacted + cursor, source + old_offset, length);
            write_u16_le(compacted, entry + SLOT_OFFSET_FIELD, static_cast<std::uint16_t>(cursor));
        }

        write_u16_le(compacted, DATA_BOUNDARY_OFFSET, static_cast<std::uint16_t>(cursor));
        std::memcpy(page_.data(), compacted, PAGE_SIZE);
        return true;
    }

} // namespace minidb
