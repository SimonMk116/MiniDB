#include "minidb/storage/heap_table.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>

namespace minidb {
namespace {

constexpr std::size_t META_MAGIC_OFFSET = 0;
constexpr std::size_t META_VERSION_OFFSET = 4;
constexpr std::size_t META_KIND_OFFSET = 5;
constexpr std::size_t META_RESERVED_OFFSET = 6;
constexpr std::size_t META_NEXT_OFFSET = 8;
constexpr std::size_t META_COUNT_OFFSET = 12;
constexpr std::size_t META_CAPACITY_OFFSET = 14;
constexpr std::size_t META_ENTRIES_OFFSET = 16;
constexpr std::byte META_MAGIC[4]{std::byte{'H'}, std::byte{'T'}, std::byte{'B'}, std::byte{'L'}};
constexpr std::uint8_t META_VERSION = 1;
constexpr std::uint8_t ROOT_KIND = 1;
constexpr std::uint8_t CONTINUATION_KIND = 2;
constexpr page_id_t NO_PAGE = std::numeric_limits<page_id_t>::max();
constexpr std::size_t META_ENTRY_SIZE = sizeof(page_id_t);
constexpr std::size_t META_CAPACITY = (PAGE_SIZE - META_ENTRIES_OFFSET) / META_ENTRY_SIZE;
constexpr std::size_t MAX_FRESH_RECORD_SIZE = PAGE_SIZE - SlottedPage::HEADER_SIZE - SlottedPage::SLOT_ENTRY_SIZE;

std::uint16_t read_u16_le(const std::byte* bytes, std::size_t offset) noexcept {
    const auto low = std::to_integer<std::uint16_t>(bytes[offset]);
    const auto high = std::to_integer<std::uint16_t>(bytes[offset + 1]);
    return static_cast<std::uint16_t>(low | static_cast<std::uint16_t>(high << 8));
}

std::uint32_t read_u32_le(const std::byte* bytes, std::size_t offset) noexcept {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < sizeof(std::uint32_t); ++i) {
        value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + i])) << (i * 8);
    }
    return value;
}

void write_u16_le(std::byte* bytes, std::size_t offset, std::uint16_t value) noexcept {
    bytes[offset] = static_cast<std::byte>(value & 0xFFU);
    bytes[offset + 1] = static_cast<std::byte>((value >> 8) & 0xFFU);
}

void write_u32_le(std::byte* bytes, std::size_t offset, std::uint32_t value) noexcept {
    for (std::size_t i = 0; i < sizeof(std::uint32_t); ++i) {
        bytes[offset + i] = static_cast<std::byte>((value >> (i * 8)) & 0xFFU);
    }
}

void initialize_metadata_page(Page& page, std::uint8_t kind) noexcept {
    page.reset();
    std::byte* const bytes = page.data();
    for (std::size_t i = 0; i < sizeof(META_MAGIC); ++i) {
        bytes[META_MAGIC_OFFSET + i] = META_MAGIC[i];
    }
    bytes[META_VERSION_OFFSET] = static_cast<std::byte>(META_VERSION);
    bytes[META_KIND_OFFSET] = static_cast<std::byte>(kind);
    write_u32_le(bytes, META_NEXT_OFFSET, NO_PAGE);
    write_u16_le(bytes, META_COUNT_OFFSET, 0);
    write_u16_le(bytes, META_CAPACITY_OFFSET, static_cast<std::uint16_t>(META_CAPACITY));
}

class PinnedPage {
public:
    PinnedPage(BufferPoolManager& pool, page_id_t page_id, Page* page) noexcept
        : pool_(pool), page_id_(page_id), page_(page) {}

    PinnedPage(const PinnedPage&) = delete;
    PinnedPage& operator=(const PinnedPage&) = delete;

    ~PinnedPage() {
        if (active_) {
            (void)pool_.unpinPage(page_id_, dirty_);
        }
    }

    Page& page() const noexcept { return *page_; }
    void markDirty() noexcept { dirty_ = true; }

    void release() {
        if (!active_) {
            throw std::logic_error("page pin was released more than once");
        }
        active_ = false;
        if (!pool_.unpinPage(page_id_, dirty_)) {
            throw std::logic_error("BufferPoolManager rejected a page unpin");
        }
    }

private:
    BufferPoolManager& pool_;
    page_id_t page_id_;
    Page* page_;
    bool dirty_{false};
    bool active_{true};
};

struct MetadataSnapshot {
    std::vector<page_id_t> data_page_ids;
    std::vector<page_id_t> metadata_page_ids;
    page_id_t last_metadata_page_id{};
    std::uint16_t last_entry_count{};
};

void validate_metadata_header(const Page& page, std::uint8_t expected_kind, page_id_t page_id) {
    const std::byte* const bytes = page.data();
    for (std::size_t i = 0; i < sizeof(META_MAGIC); ++i) {
        if (bytes[META_MAGIC_OFFSET + i] != META_MAGIC[i]) {
            throw std::runtime_error("HeapTable metadata page has invalid magic");
        }
    }
    if (std::to_integer<std::uint8_t>(bytes[META_VERSION_OFFSET]) != META_VERSION ||
        std::to_integer<std::uint8_t>(bytes[META_KIND_OFFSET]) != expected_kind ||
        bytes[META_RESERVED_OFFSET] != std::byte{0} ||
        bytes[META_RESERVED_OFFSET + 1] != std::byte{0}) {
        throw std::runtime_error("HeapTable metadata page has an unsupported or malformed header");
    }

    const std::size_t count = read_u16_le(bytes, META_COUNT_OFFSET);
    const std::size_t capacity = read_u16_le(bytes, META_CAPACITY_OFFSET);
    if (capacity != META_CAPACITY || count > capacity) {
        throw std::runtime_error("HeapTable metadata page has invalid entry capacity or count");
    }

    const page_id_t next = read_u32_le(bytes, META_NEXT_OFFSET);
    if (next == page_id) {
        throw std::runtime_error("HeapTable metadata page links to itself");
    }
}

MetadataSnapshot read_metadata(BufferPoolManager& pool, page_id_t root_page_id) {
    MetadataSnapshot snapshot;
    std::unordered_set<page_id_t> seen_metadata_pages;
    std::unordered_set<page_id_t> seen_data_pages;
    page_id_t current_page_id = root_page_id;
    std::uint8_t expected_kind = ROOT_KIND;

    while (true) {
        if (!seen_metadata_pages.insert(current_page_id).second) {
            throw std::runtime_error("HeapTable metadata page chain contains a cycle or duplicate page ID");
        }

        Page* const page = [&]() -> Page* {
            try {
                Page* fetched = pool.fetchPage(current_page_id);
                if (fetched == nullptr) {
                    throw std::runtime_error("Buffer pool has no frame available to read HeapTable metadata");
                }
                return fetched;
            } catch (const std::out_of_range&) {
                throw std::runtime_error("HeapTable metadata links to a page outside the database file");
            }
        }();
        PinnedPage pin(pool, current_page_id, page);
        validate_metadata_header(pin.page(), expected_kind, current_page_id);

        const std::byte* const bytes = pin.page().data();
        const std::size_t count = read_u16_le(bytes, META_COUNT_OFFSET);
        for (std::size_t i = 0; i < count; ++i) {
            const std::size_t entry_offset = META_ENTRIES_OFFSET + i * META_ENTRY_SIZE;
            const page_id_t data_page_id = read_u32_le(bytes, entry_offset);
            if (data_page_id == NO_PAGE || !seen_data_pages.insert(data_page_id).second) {
                throw std::runtime_error("HeapTable metadata contains an invalid or duplicate data page ID");
            }
            snapshot.data_page_ids.push_back(data_page_id);
        }

        const page_id_t next_page_id = read_u32_le(bytes, META_NEXT_OFFSET);
        snapshot.metadata_page_ids.push_back(current_page_id);
        snapshot.last_metadata_page_id = current_page_id;
        snapshot.last_entry_count = static_cast<std::uint16_t>(count);
        pin.release();

        if (next_page_id == NO_PAGE) {
            break;
        }
        if (seen_metadata_pages.contains(next_page_id)) {
            throw std::runtime_error("HeapTable metadata page chain contains a cycle or duplicate page ID");
        }
        current_page_id = next_page_id;
        expected_kind = CONTINUATION_KIND;
    }

    for (const page_id_t data_page_id : snapshot.data_page_ids) {
        if (seen_metadata_pages.contains(data_page_id)) {
            throw std::runtime_error("HeapTable metadata page is also listed as a data page");
        }
    }
    return snapshot;
}

Page* fetch_required_page(BufferPoolManager& pool, page_id_t page_id, const char* purpose) {
    Page* page = nullptr;
    try {
        page = pool.fetchPage(page_id);
    } catch (const std::out_of_range&) {
        throw std::runtime_error(std::string("HeapTable ") + purpose + " page ID is outside the database file");
    }
    if (page == nullptr) {
        throw std::runtime_error(std::string("Buffer pool has no frame available for HeapTable ") + purpose);
    }
    return page;
}

} // namespace

std::optional<page_id_t> HeapTable::create(BufferPoolManager& buffer_pool) {
    page_id_t root_page_id{};
    Page* const page = buffer_pool.newPage(root_page_id);
    if (page == nullptr) {
        return std::nullopt;
    }
    PinnedPage pin(buffer_pool, root_page_id, page);
    initialize_metadata_page(pin.page(), ROOT_KIND);
    pin.markDirty();
    pin.release();
    return root_page_id;
}

HeapTable::HeapTable(BufferPoolManager& buffer_pool, page_id_t metadata_root_page_id)
    : buffer_pool_(buffer_pool), metadata_root_page_id_(metadata_root_page_id) {
    (void)read_metadata(buffer_pool_, metadata_root_page_id_);
}

page_id_t HeapTable::metadataRootPageId() const noexcept {
    return metadata_root_page_id_;
}

std::optional<RecordId> HeapTable::insertRecord(std::span<const std::byte> record) {
    if (record.size() > MAX_FRESH_RECORD_SIZE) {
        return std::nullopt;
    }

    const auto data_page_ids = loadDataPageIds();
    for (const page_id_t page_id : data_page_ids) {
        Page* const page = fetch_required_page(buffer_pool_, page_id, "data");
        PinnedPage pin(buffer_pool_, page_id, page);
        SlottedPage slotted_page(pin.page());
        if (!slotted_page.isValid()) {
            throw std::runtime_error("HeapTable contains a corrupted SlottedPage");
        }
        const auto slot_id = slotted_page.insertRecord(record);
        if (slot_id) {
            pin.markDirty();
            pin.release();
            return RecordId{page_id, *slot_id};
        }
    }

    page_id_t new_data_page_id{};
    Page* const new_page = buffer_pool_.newPage(new_data_page_id);
    if (new_page == nullptr) {
        throw std::runtime_error("Buffer pool has no frame available to allocate a HeapTable data page");
    }
    PinnedPage pin(buffer_pool_, new_data_page_id, new_page);
    SlottedPage slotted_page(pin.page());
    slotted_page.initialize();
    const auto slot_id = slotted_page.insertRecord(record);
    if (!slot_id) {
        throw std::logic_error("Preflighted record did not fit on a fresh SlottedPage");
    }
    pin.markDirty();
    pin.release();

    appendDataPageId(new_data_page_id);
    return RecordId{new_data_page_id, *slot_id};
}

std::optional<std::vector<std::byte>> HeapTable::getRecord(RecordId id) const {
    const auto data_page_ids = loadDataPageIds();
    if (std::find(data_page_ids.begin(), data_page_ids.end(), id.page_id) == data_page_ids.end()) {
        return std::nullopt;
    }

    Page* const page = fetch_required_page(buffer_pool_, id.page_id, "data");
    PinnedPage pin(buffer_pool_, id.page_id, page);
    SlottedPage slotted_page(pin.page());
    if (!slotted_page.isValid()) {
        throw std::runtime_error("HeapTable contains a corrupted SlottedPage");
    }
    return slotted_page.getRecord(id.slot_id);
}

bool HeapTable::deleteRecord(RecordId id) {
    const auto data_page_ids = loadDataPageIds();
    if (std::find(data_page_ids.begin(), data_page_ids.end(), id.page_id) == data_page_ids.end()) {
        return false;
    }

    Page* const page = fetch_required_page(buffer_pool_, id.page_id, "data");
    PinnedPage pin(buffer_pool_, id.page_id, page);
    SlottedPage slotted_page(pin.page());
    if (!slotted_page.isValid()) {
        throw std::runtime_error("HeapTable contains a corrupted SlottedPage");
    }
    if (!slotted_page.deleteRecord(id.slot_id)) {
        return false;
    }
    pin.markDirty();
    return true;
}

std::vector<HeapRecord> HeapTable::scan() const {
    const auto data_page_ids = loadDataPageIds();
    std::vector<HeapRecord> records;
    for (const page_id_t page_id : data_page_ids) {
        Page* const page = fetch_required_page(buffer_pool_, page_id, "data");
        PinnedPage pin(buffer_pool_, page_id, page);
        SlottedPage slotted_page(pin.page());
        auto live_records = slotted_page.getLiveRecords();
        if (!live_records) {
            throw std::runtime_error("HeapTable contains a corrupted SlottedPage");
        }
        for (auto& [slot_id, data] : *live_records) {
            records.push_back(HeapRecord{RecordId{page_id, slot_id}, std::move(data)});
        }
    }
    return records;
}

std::vector<page_id_t> HeapTable::loadDataPageIds() const {
    return read_metadata(buffer_pool_, metadata_root_page_id_).data_page_ids;
}

void HeapTable::appendDataPageId(page_id_t data_page_id) {
    MetadataSnapshot snapshot = read_metadata(buffer_pool_, metadata_root_page_id_);
    if (std::find(snapshot.data_page_ids.begin(), snapshot.data_page_ids.end(), data_page_id) !=
            snapshot.data_page_ids.end() ||
        std::find(snapshot.metadata_page_ids.begin(), snapshot.metadata_page_ids.end(), data_page_id) !=
            snapshot.metadata_page_ids.end()) {
        throw std::runtime_error("HeapTable cannot append a duplicate page ID");
    }

    if (snapshot.last_entry_count < META_CAPACITY) {
        Page* const page = fetch_required_page(buffer_pool_, snapshot.last_metadata_page_id, "metadata");
        PinnedPage pin(buffer_pool_, snapshot.last_metadata_page_id, page);
        const std::uint8_t expected_kind = snapshot.last_metadata_page_id == metadata_root_page_id_
                                               ? ROOT_KIND
                                               : CONTINUATION_KIND;
        validate_metadata_header(pin.page(), expected_kind, snapshot.last_metadata_page_id);
        std::byte* const bytes = pin.page().data();
        const std::size_t entry_offset = META_ENTRIES_OFFSET +
                                         static_cast<std::size_t>(snapshot.last_entry_count) * META_ENTRY_SIZE;
        write_u32_le(bytes, entry_offset, data_page_id);
        write_u16_le(bytes, META_COUNT_OFFSET,
                     static_cast<std::uint16_t>(snapshot.last_entry_count + 1));
        pin.markDirty();
        return;
    }

    page_id_t continuation_page_id{};
    Page* const continuation_page = buffer_pool_.newPage(continuation_page_id);
    if (continuation_page == nullptr) {
        throw std::runtime_error("Buffer pool has no frame available to extend HeapTable metadata");
    }
    {
        PinnedPage pin(buffer_pool_, continuation_page_id, continuation_page);
        initialize_metadata_page(pin.page(), CONTINUATION_KIND);
        std::byte* const bytes = pin.page().data();
        write_u32_le(bytes, META_ENTRIES_OFFSET, data_page_id);
        write_u16_le(bytes, META_COUNT_OFFSET, 1);
        pin.markDirty();
    }

    Page* const last_page = fetch_required_page(buffer_pool_, snapshot.last_metadata_page_id, "metadata");
    PinnedPage pin(buffer_pool_, snapshot.last_metadata_page_id, last_page);
    const std::uint8_t expected_kind = snapshot.last_metadata_page_id == metadata_root_page_id_
                                           ? ROOT_KIND
                                           : CONTINUATION_KIND;
    validate_metadata_header(pin.page(), expected_kind, snapshot.last_metadata_page_id);
    if (read_u32_le(pin.page().data(), META_NEXT_OFFSET) != NO_PAGE) {
        throw std::runtime_error("HeapTable metadata changed while appending a data page");
    }
    write_u32_le(pin.page().data(), META_NEXT_OFFSET, continuation_page_id);
    pin.markDirty();
}

} // namespace minidb
