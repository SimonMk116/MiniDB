#include "minidb/storage/slotted_page.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void write_u16_le(std::byte* bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::byte>(value & 0x00FFU);
    bytes[offset + 1] = static_cast<std::byte>((value >> 8) & 0x00FFU);
}

std::uint16_t read_u16_le(const std::byte* bytes, std::size_t offset) {
    const auto low = std::to_integer<std::uint16_t>(bytes[offset]);
    const auto high = std::to_integer<std::uint16_t>(bytes[offset + 1]);
    return static_cast<std::uint16_t>(low | static_cast<std::uint16_t>(high << 8));
}

void initialize_slots(minidb::Page& page, std::uint16_t count, std::uint16_t data_boundary) {
    minidb::SlottedPage view(page);
    view.initialize();
    write_u16_le(page.data(), 4, count);
    write_u16_le(page.data(), 6, data_boundary);
}

void set_slot(minidb::Page& page, std::size_t slot_index, std::uint16_t offset,
              std::uint16_t length, minidb::SlottedPage::SlotState state) {
    const std::size_t entry = minidb::SlottedPage::HEADER_SIZE +
                              slot_index * minidb::SlottedPage::SLOT_ENTRY_SIZE;
    write_u16_le(page.data(), entry, offset);
    write_u16_le(page.data(), entry + 2, length);
    page.data()[entry + 4] = static_cast<std::byte>(state);
}

void test_initialization_and_empty_valid_page() {
    minidb::Page page;
    minidb::SlottedPage view(page);

    expect(!view.isValid(), "a zeroed, uninitialized page should be invalid");
    view.initialize();
    expect(view.isValid(), "initialized empty page should be valid");
    expect(view.slotCount() == 0, "empty valid page should report zero slots");
    expect(page.data()[0] == std::byte{0x53} && page.data()[1] == std::byte{0x50},
           "initializer should write the slotted-page magic bytes");
    expect(page.data()[2] == std::byte{1}, "initializer should write format version 1");
    expect(page.data()[3] == std::byte{0}, "initializer should zero reserved header byte");
    expect(page.data()[4] == std::byte{0} && page.data()[5] == std::byte{0},
           "empty page should have zero slots in little-endian form");
    expect(page.data()[6] == std::byte{0} && page.data()[7] == std::byte{0x20},
           "empty data boundary should equal PAGE_SIZE in little-endian form");
    for (std::size_t i = minidb::SlottedPage::HEADER_SIZE; i < minidb::PAGE_SIZE; ++i) {
        expect(page.data()[i] == std::byte{0}, "initialization should clear the remaining page bytes");
    }
}

void test_invalid_magic_and_version() {
    minidb::Page page;
    minidb::SlottedPage view(page);
    view.initialize();

    page.data()[0] = std::byte{0};
    expect(!view.isValid(), "invalid magic should be rejected");
    view.initialize();
    page.data()[2] = std::byte{2};
    expect(!view.isValid(), "unsupported format version should be rejected");
}

void test_invalid_slot_count_and_data_boundary() {
    minidb::Page page;
    minidb::SlottedPage view(page);
    view.initialize();

    write_u16_le(page.data(), 4, 0xFFFF);
    expect(!view.isValid(), "slot count beyond directory capacity should be rejected");

    view.initialize();
    write_u16_le(page.data(), 6, 7);
    expect(!view.isValid(), "data boundary before the header/directory should be rejected");

    view.initialize();
    write_u16_le(page.data(), 6, static_cast<std::uint16_t>(minidb::PAGE_SIZE + 1));
    expect(!view.isValid(), "data boundary beyond the page should be rejected");
}

void test_corrupted_slot_entries() {
    minidb::Page page;
    minidb::SlottedPage view(page);

    initialize_slots(page, 1, static_cast<std::uint16_t>(minidb::PAGE_SIZE));
    page.data()[12] = std::byte{2}; // unknown state
    expect(!view.isValid(), "unknown slot state should be rejected");

    initialize_slots(page, 1, static_cast<std::uint16_t>(minidb::PAGE_SIZE));
    page.data()[13] = std::byte{1}; // reserved byte
    expect(!view.isValid(), "nonzero slot reserved byte should be rejected");

    initialize_slots(page, 1, static_cast<std::uint16_t>(minidb::PAGE_SIZE));
    write_u16_le(page.data(), 8, 100); // a free slot must have no record extent
    expect(!view.isValid(), "free slot with a nonzero offset should be rejected");

    initialize_slots(page, 1, static_cast<std::uint16_t>(minidb::PAGE_SIZE));
    write_u16_le(page.data(), 10, 1); // free slot length
    expect(!view.isValid(), "free slot with a nonzero length should be rejected");

    initialize_slots(page, 1, 14);
    set_slot(page, 0, 12, 2, minidb::SlottedPage::SlotState::occupied);
    expect(!view.isValid(), "record overlapping the slot directory should be rejected");

    initialize_slots(page, 1, 100);
    set_slot(page, 0, 8191, 2, minidb::SlottedPage::SlotState::occupied);
    expect(!view.isValid(), "record extent running beyond the page should be rejected");

    initialize_slots(page, 2, 200);
    set_slot(page, 0, 200, 10, minidb::SlottedPage::SlotState::occupied);
    set_slot(page, 1, 205, 10, minidb::SlottedPage::SlotState::occupied);
    expect(!view.isValid(), "overlapping live record extents should be rejected");

    initialize_slots(page, 2, 90);
    set_slot(page, 0, 100, 10, minidb::SlottedPage::SlotState::occupied);
    set_slot(page, 1, 90, 10, minidb::SlottedPage::SlotState::occupied);
    expect(view.isValid(), "adjacent non-overlapping records should be accepted");
}

void test_canonical_empty_record_representation() {
    minidb::Page page;
    minidb::SlottedPage view(page);

    initialize_slots(page, 1, static_cast<std::uint16_t>(minidb::PAGE_SIZE));
    set_slot(page, 0, static_cast<std::uint16_t>(minidb::PAGE_SIZE), 0,
             minidb::SlottedPage::SlotState::occupied);
    expect(view.isValid(), "empty record at PAGE_SIZE should be accepted");

    initialize_slots(page, 1, static_cast<std::uint16_t>(minidb::PAGE_SIZE));
    set_slot(page, 0, 100, 0, minidb::SlottedPage::SlotState::occupied);
    expect(!view.isValid(), "empty record at a noncanonical offset should be rejected");

    initialize_slots(page, 2, static_cast<std::uint16_t>(minidb::PAGE_SIZE));
    set_slot(page, 0, static_cast<std::uint16_t>(minidb::PAGE_SIZE), 0,
             minidb::SlottedPage::SlotState::occupied);
    set_slot(page, 1, static_cast<std::uint16_t>(minidb::PAGE_SIZE), 0,
             minidb::SlottedPage::SlotState::occupied);
    expect(view.isValid(), "page containing only empty records should keep PAGE_SIZE boundary");
}

void test_data_boundary_matches_lowest_nonempty_record() {
    minidb::Page page;
    minidb::SlottedPage view(page);

    initialize_slots(page, 1, 90);
    set_slot(page, 0, 100, 10, minidb::SlottedPage::SlotState::occupied);
    expect(!view.isValid(), "data boundary below the lowest record should be rejected");

    initialize_slots(page, 1, 101);
    set_slot(page, 0, 100, 10, minidb::SlottedPage::SlotState::occupied);
    expect(!view.isValid(), "data boundary above a record offset should be rejected");
}

void test_insert_and_retrieve_binary_record() {
    minidb::Page page;
    minidb::SlottedPage view(page);
    view.initialize();

    const std::array<std::byte, 6> binary_record{
        std::byte{0x00}, std::byte{0xFF}, std::byte{0x41},
        std::byte{0x00}, std::byte{0x80}, std::byte{0x7F}};
    const auto slot = view.insertRecord(binary_record);
    expect(slot.has_value() && *slot == 0, "first insertion should return slot ID zero");
    expect(view.isValid(), "page should remain valid after insertion");

    const auto retrieved = view.getRecord(*slot);
    expect(retrieved.has_value(), "inserted record should be retrievable");
    expect(std::equal(retrieved->begin(), retrieved->end(), binary_record.begin(), binary_record.end()),
           "retrieval should preserve every binary byte");
}

void test_multiple_records_and_owned_retrieval() {
    minidb::Page page;
    minidb::SlottedPage view(page);
    view.initialize();

    const std::array<std::byte, 2> first{std::byte{0x11}, std::byte{0x22}};
    const std::array<std::byte, 9> second{
        std::byte{0x30}, std::byte{0x31}, std::byte{0x32}, std::byte{0x33}, std::byte{0x34},
        std::byte{0x35}, std::byte{0x36}, std::byte{0x37}, std::byte{0x38}};
    const auto first_slot = view.insertRecord(first);
    const auto second_slot = view.insertRecord(second);
    expect(first_slot == 0 && second_slot == 1, "records should receive sequential slot IDs");
    expect(view.isValid(), "page with multiple records should be valid");

    auto first_copy = view.getRecord(*first_slot);
    auto second_copy = view.getRecord(*second_slot);
    expect(first_copy && second_copy, "both inserted records should be retrievable");
    expect(std::equal(first_copy->begin(), first_copy->end(), first.begin(), first.end()),
           "first record bytes should match");
    expect(std::equal(second_copy->begin(), second_copy->end(), second.begin(), second.end()),
           "second record bytes should match");

    // The result owns its bytes; changing the page afterward cannot change a
    // previously returned record copy.
    const std::array<std::byte, 1> third{std::byte{0x99}};
    expect(view.insertRecord(third) == 2, "third record should append at slot ID two");
    expect(std::equal(second_copy->begin(), second_copy->end(), second.begin(), second.end()),
           "retrieved record must be independent of subsequent page changes");
}

void test_empty_record_and_invalid_slot_ids() {
    minidb::Page page;
    minidb::SlottedPage view(page);
    view.initialize();

    const auto empty_slot = view.insertRecord(std::span<const std::byte>{});
    expect(empty_slot == 0, "empty record should be insertable");
    expect(view.slotCount() == 1, "slotCount should include occupied slots");
    expect(view.isValid(), "page containing an empty record should remain valid");
    const auto empty_record = view.getRecord(*empty_slot);
    expect(empty_record.has_value() && empty_record->empty(),
           "empty record retrieval should return an owned empty vector");
    expect(!view.getRecord(1), "slot equal to slot count should be rejected");
    expect(!view.getRecord(std::numeric_limits<minidb::slot_id_t>::max()),
           "large invalid slot ID should be rejected");
}

void test_slot_count_rejects_corrupted_page() {
    minidb::Page page;
    minidb::SlottedPage view(page);
    expect(!view.slotCount(), "uninitialized page should not report a slot count");
    view.initialize();
    write_u16_le(page.data(), 4, 0xFFFF);
    expect(!view.slotCount(), "corrupted directory count should not be reported");
}

void test_insufficient_space_and_failed_insert_atomicity() {
    minidb::Page page;
    minidb::SlottedPage view(page);
    view.initialize();

    std::vector<std::byte> too_large(minidb::PAGE_SIZE, std::byte{0x5A});
    std::array<std::byte, minidb::PAGE_SIZE> before{};
    std::copy(page.data(), page.data() + minidb::PAGE_SIZE, before.begin());
    expect(!view.insertRecord(too_large), "record larger than available page space should fail");
    expect(std::equal(before.begin(), before.end(), page.data()),
           "failed insertion into an empty page must leave all bytes unchanged");

    const std::size_t maximum_single_record_size =
        minidb::PAGE_SIZE - minidb::SlottedPage::HEADER_SIZE - minidb::SlottedPage::SLOT_ENTRY_SIZE;
    std::vector<std::byte> boundary_record(maximum_single_record_size, std::byte{0xC3});
    const auto boundary_slot = view.insertRecord(boundary_record);
    expect(boundary_slot == 0 && view.isValid(), "record exactly filling the usable payload should fit");
    const auto boundary_copy = view.getRecord(*boundary_slot);
    expect(boundary_copy && *boundary_copy == boundary_record,
           "record at the exact page-space boundary should round-trip");

    std::copy(page.data(), page.data() + minidb::PAGE_SIZE, before.begin());
    expect(!view.insertRecord(std::span<const std::byte>{}),
           "an additional slot should fail when no directory space remains");
    expect(std::equal(before.begin(), before.end(), page.data()),
           "failed insertion after an existing record must leave all bytes unchanged");
}

void test_delete_preserves_slots_and_rejects_invalid_ids() {
    minidb::Page page;
    minidb::SlottedPage view(page);
    view.initialize();
    const std::array<std::byte, 3> first{std::byte{1}, std::byte{2}, std::byte{3}};
    const std::array<std::byte, 2> second{std::byte{4}, std::byte{5}};
    expect(view.insertRecord(first) == 0 && view.insertRecord(second) == 1,
           "setup records should receive stable IDs");

    expect(view.deleteRecord(0), "deleting a live slot should succeed");
    expect(view.isValid(), "page should remain valid after deletion");
    expect(!view.getRecord(0), "deleted slot should no longer be retrievable");
    expect(!view.deleteRecord(0), "deleting an already-free slot should fail");
    expect(!view.deleteRecord(2), "deleting an out-of-range slot should fail");
    expect(!view.deleteRecord(std::numeric_limits<minidb::slot_id_t>::max()),
           "deleting a large invalid slot ID should fail");
    const auto still_live = view.getRecord(1);
    expect(still_live && *still_live == std::vector<std::byte>(second.begin(), second.end()),
           "deletion must preserve other records and their slot IDs");
}

void test_reuses_lowest_free_slot_id() {
    minidb::Page page;
    minidb::SlottedPage view(page);
    view.initialize();
    const std::array<std::byte, 1> payload{std::byte{0xA5}};
    expect(view.insertRecord(payload) == 0, "first setup slot should be zero");
    expect(view.insertRecord(payload) == 1, "second setup slot should be one");
    expect(view.insertRecord(payload) == 2, "third setup slot should be two");
    expect(view.deleteRecord(2) && view.deleteRecord(0), "setup deletions should succeed");

    expect(view.insertRecord(payload) == 0, "insertion should reuse the lowest free slot");
    expect(view.insertRecord(payload) == 2, "next insertion should reuse the next free slot");
    expect(view.isValid(), "page should remain valid after lowest-slot reuse");
    expect(view.getRecord(1).has_value(), "reuse must leave the untouched live slot intact");
}

void test_compaction_preserves_live_slot_ids_and_records() {
    minidb::Page page;
    minidb::SlottedPage view(page);
    view.initialize();
    const std::vector<std::byte> first(1300, std::byte{0x11});
    const std::vector<std::byte> second(900, std::byte{0x22});
    const std::vector<std::byte> third(700, std::byte{0x33});
    expect(view.insertRecord(first) == 0, "first record should use slot zero");
    expect(view.insertRecord(second) == 1, "second record should use slot one");
    expect(view.insertRecord(third) == 2, "third record should use slot two");
    expect(view.deleteRecord(1), "middle record deletion should succeed");

    expect(view.compact(), "valid page should compact successfully");
    expect(view.isValid(), "compacted page should remain valid");
    expect(view.getRecord(0) == first && view.getRecord(2) == third,
           "compaction should preserve live payloads and slot IDs");
    expect(!view.getRecord(1), "compaction should leave deleted slots free");
    expect(read_u16_le(page.data(), 6) == minidb::PAGE_SIZE - first.size() - third.size(),
           "compaction should pack records and set the boundary to the lowest offset");
}

void test_fragmented_insert_compacts_and_reuses_slot() {
    minidb::Page page;
    minidb::SlottedPage view(page);
    view.initialize();
    const std::vector<std::byte> large(3000, std::byte{0xA1});
    const std::vector<std::byte> survivor(2000, std::byte{0xB2});
    const std::vector<std::byte> replacement(4000, std::byte{0xC3});
    expect(view.insertRecord(large) == 0 && view.insertRecord(survivor) == 1,
           "fragmentation setup should insert two records");
    expect(view.deleteRecord(0), "fragmentation setup deletion should succeed");

    // 4000 bytes fit in total free space but not in the current contiguous gap.
    // This operation must compact and reuse slot zero.
    const auto inserted = view.insertRecord(replacement);
    expect(inserted == 0, "fragmented insertion should compact and reuse the lowest free slot");
    expect(view.isValid(), "page should remain valid after insert-triggered compaction");
    expect(view.getRecord(0) == replacement && view.getRecord(1) == survivor,
           "compaction and insertion should preserve both records under their original IDs");
}

void test_failed_fragmented_insert_does_not_compact_or_modify_page() {
    minidb::Page page;
    minidb::SlottedPage view(page);
    view.initialize();
    const std::vector<std::byte> first(3000, std::byte{0x61});
    const std::vector<std::byte> second(2000, std::byte{0x62});
    const std::vector<std::byte> cannot_fit(7000, std::byte{0x73});
    expect(view.insertRecord(first) == 0 && view.insertRecord(second) == 1,
           "fragmentation failure setup should insert two records");
    expect(view.deleteRecord(0), "fragmentation failure setup deletion should succeed");

    std::array<std::byte, minidb::PAGE_SIZE> before{};
    std::copy(page.data(), page.data() + minidb::PAGE_SIZE, before.begin());
    expect(!view.insertRecord(cannot_fit), "record exceeding total reclaimable space should fail");
    expect(std::equal(before.begin(), before.end(), page.data()),
           "failed fragmented insertion must preserve every page byte, including fragmentation");
}

void test_compaction_with_empty_and_deleted_slots() {
    minidb::Page page;
    minidb::SlottedPage view(page);
    view.initialize();
    const std::array<std::byte, 4> payload{std::byte{9}, std::byte{8}, std::byte{7}, std::byte{6}};
    expect(view.insertRecord(std::span<const std::byte>{}) == 0, "empty record should use slot zero");
    expect(view.insertRecord(payload) == 1, "payload should use slot one");
    expect(view.insertRecord(payload) == 2, "second payload should use slot two");
    expect(view.deleteRecord(2), "delete before compaction should succeed");
    expect(view.compact(), "compaction should accept empty and free entries");
    expect(view.isValid(), "page should remain valid after compaction with empty/free slots");
    expect(view.getRecord(0).has_value() && view.getRecord(0)->empty(),
           "empty record representation should survive compaction");
    expect(view.getRecord(1) == std::vector<std::byte>(payload.begin(), payload.end()),
           "live payload should survive compaction");
    expect(!view.getRecord(2), "free slot should remain free after compaction");
}

} // namespace

int main() {
    try {
        test_initialization_and_empty_valid_page();
        test_invalid_magic_and_version();
        test_invalid_slot_count_and_data_boundary();
        test_corrupted_slot_entries();
        test_canonical_empty_record_representation();
        test_data_boundary_matches_lowest_nonempty_record();
        test_insert_and_retrieve_binary_record();
        test_multiple_records_and_owned_retrieval();
        test_empty_record_and_invalid_slot_ids();
        test_slot_count_rejects_corrupted_page();
        test_insufficient_space_and_failed_insert_atomicity();
        test_delete_preserves_slots_and_rejects_invalid_ids();
        test_reuses_lowest_free_slot_id();
        test_compaction_preserves_live_slot_ids_and_records();
        test_fragmented_insert_compacts_and_reuses_slot();
        test_failed_fragmented_insert_does_not_compact_or_modify_page();
        test_compaction_with_empty_and_deleted_slots();
        std::cout << "SlottedPage tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SlottedPage test failed: " << error.what() << '\n';
        return 1;
    }
}
