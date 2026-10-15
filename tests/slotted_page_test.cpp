#include "minidb/storage/slotted_page.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

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

void set_one_free_slot(minidb::Page& page) {
    minidb::SlottedPage view(page);
    view.initialize();
    write_u16_le(page.data(), 4, 1);
    write_u16_le(page.data(), 6, static_cast<std::uint16_t>(minidb::PAGE_SIZE));
}

void test_initialization_and_empty_valid_page() {
    minidb::Page page;
    minidb::SlottedPage view(page);

    expect(!view.isValid(), "a zeroed, uninitialized page should be invalid");
    view.initialize();
    expect(view.isValid(), "initialized empty page should be valid");
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

    set_one_free_slot(page);
    page.data()[12] = std::byte{2}; // unknown state
    expect(!view.isValid(), "unknown slot state should be rejected");

    set_one_free_slot(page);
    page.data()[13] = std::byte{1}; // reserved byte
    expect(!view.isValid(), "nonzero slot reserved byte should be rejected");

    set_one_free_slot(page);
    write_u16_le(page.data(), 8, 100); // a free slot must have no record extent
    expect(!view.isValid(), "free slot with a nonzero offset should be rejected");

    set_one_free_slot(page);
    write_u16_le(page.data(), 6, 100);
    write_u16_le(page.data(), 8, 8191);
    write_u16_le(page.data(), 10, 2);
    page.data()[12] = static_cast<std::byte>(minidb::SlottedPage::SlotState::occupied);
    expect(!view.isValid(), "record extent running beyond the page should be rejected");

    view.initialize();
    write_u16_le(page.data(), 4, 2);
    write_u16_le(page.data(), 6, 100);
    write_u16_le(page.data(), 8, 200);
    write_u16_le(page.data(), 10, 10);
    page.data()[12] = static_cast<std::byte>(minidb::SlottedPage::SlotState::occupied);
    write_u16_le(page.data(), 14, 205);
    write_u16_le(page.data(), 16, 10);
    page.data()[18] = static_cast<std::byte>(minidb::SlottedPage::SlotState::occupied);
    expect(!view.isValid(), "overlapping live record extents should be rejected");
}

} // namespace

int main() {
    try {
        test_initialization_and_empty_valid_page();
        test_invalid_magic_and_version();
        test_invalid_slot_count_and_data_boundary();
        test_corrupted_slot_entries();
        std::cout << "SlottedPage tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SlottedPage test failed: " << error.what() << '\n';
        return 1;
    }
}
