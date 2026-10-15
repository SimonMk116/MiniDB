# SlottedPage format and current implementation

`SlottedPage` is a non-owning view over an existing `Page`. The `Page` owns the
8,192 bytes; the view only reads or writes those bytes. The referenced `Page`
must outlive the view.

The implementation supports explicit initialization, validation, binary record
insertion and copied retrieval, deletion, lowest-free-slot reuse, and
compaction. It remains a small single-page component; record IDs are slot IDs.

## On-page representation

All multibyte integers use explicit little-endian byte encoding. The bytes are
not reinterpreted as C++ structs, so compiler padding, alignment, object
lifetime, and host byte order do not determine the disk format.

| Byte range | Size | Field |
|---|---:|---|
| 0–1 | 2 | Magic bytes `SP` (`0x53 0x50`) |
| 2 | 1 | Format version (`1`) |
| 3 | 1 | Reserved; must be zero |
| 4–5 | 2 | Slot count, little-endian |
| 6–7 | 2 | Data boundary, little-endian |
| 8 onward | 6 per slot | Slot directory |

The directory grows upward from byte 8. Record data is allocated from the end
of the page downward. `data_boundary` is the lowest offset occupied by any
non-empty live record; it is `PAGE_SIZE` when there are no non-empty live
records. The directory end is derived as `8 + slot_count * 6`; it is not stored
separately. Free space lies between the directory end and `data_boundary`.

Each 6-byte slot entry contains:

| Entry offset | Size | Field |
|---|---:|---|
| 0–1 | 2 | Record byte offset, little-endian |
| 2–3 | 2 | Record length, little-endian |
| 4 | 1 | State: `0` free, `1` occupied |
| 5 | 1 | Reserved; must be zero |

Free entries must have zero offset and length. An occupied zero-length entry
represents an empty record and uses the canonical offset `PAGE_SIZE`; empty
records do not affect `data_boundary`. Non-empty records have positive length
and begin at or above `data_boundary`. Slot IDs are zero-based directory
indices; physical byte offsets are not record identifiers. Deletion leaves a
canonical free entry in place, and insertion reuses the lowest free slot ID.

## Validation rules

The validator rejects unsupported magic/version values, nonzero reserved
bytes, slot counts that cannot fit, and data boundaries outside the page or
before the directory end. For every occupied slot it checks that non-empty
records begin after the directory and at or above `data_boundary`, fit within
the page, and do not overlap. It also checks that `data_boundary` equals the
lowest non-empty record offset, or `PAGE_SIZE` when there are no non-empty
records. Free slots and empty records must use their canonical representations.

The maximum possible slot count under this format is
`(8192 - 8) / 6 = 1364`. Bounds are checked before calculating record ends, so
validation does not rely on overflowing `offset + length` arithmetic.

An uninitialized page and a malformed/corrupted page both make `isValid()`
return `false`; the API does not currently distinguish those cases. Validation
never modifies bytes. `initialize()` is explicit and destructive: it clears the
whole page, writes the magic/version, sets the slot count to zero, and sets the
data boundary to `PAGE_SIZE` (encoded as `0x2000`).

## Tests

`insertRecord()` accepts `std::span<const std::byte>` and returns an optional
zero-based slot ID. It reuses the lowest free entry before appending. Empty
records are valid. When contiguous space is insufficient, it first checks
whether total free space (including reclaimable deleted bytes) can satisfy the
request. It compacts only if the request can then succeed. Any failed insertion
leaves every page byte unchanged, including the layout of fragmented pages.
The input may contain arbitrary binary bytes. `getRecord()` returns an
independently owned `std::vector<std::byte>`; invalid pages, free slots, and
out-of-range IDs return no value.
`slotCount()` returns the validated directory size, including deleted slots,
or no value for an invalid page. `getLiveRecords()` validates once and returns
all live slot IDs and owned byte vectors in slot order; HeapTable uses this
batch operation during scans.

`deleteRecord()` marks an occupied slot free without removing its directory
entry or moving other records. Its old payload becomes reclaimable
fragmentation. The data boundary is recomputed from the lowest remaining live
non-empty record. Deleting a free or invalid slot returns false.

`compact()` packs live non-empty payloads toward the page end, updates their
physical offsets, preserves all slot IDs and empty/free entries, and sets the
boundary to the lowest packed record offset. It returns false without changing
the page when validation fails. Insert-triggered compaction builds its new
layout in a temporary `Page` before replacing the old bytes.

`slotted_page_test` covers initialization; malformed headers and slot entries;
record extents overlapping the directory, extending past the page, overlapping
each other, and adjacent without overlap; canonical empty records; and
`data_boundary` consistency. It also covers binary and multiple records,
owned retrieval, empty records, invalid IDs, maximum usable payload, deletion,
stable IDs, lowest-slot reuse, explicit and insertion-triggered compaction,
fragmentation, and byte-for-byte preservation after failed insertions.
