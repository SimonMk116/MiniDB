# SlottedPage format (Stage 2)

`SlottedPage` is a non-owning view over an existing `Page`. The `Page` owns the
8,192 bytes; the view only reads or writes those bytes. The referenced `Page`
must outlive the view.

The current implementation supports explicit initialization and validation.
It does not yet support inserting, retrieving, deleting, or compacting records.

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

The directory grows upward from byte 8. Record data will be allocated from the
end of the page downward. The data boundary is the lower edge of that record
area. The directory end is derived as `8 + slot_count * 6`; it is not stored
separately.

Each 6-byte slot entry contains:

| Entry offset | Size | Field |
|---|---:|---|
| 0–1 | 2 | Record byte offset, little-endian |
| 2–3 | 2 | Record length, little-endian |
| 4 | 1 | State: `0` free, `1` occupied |
| 5 | 1 | Reserved; must be zero |

Free entries must have zero offset and length. An occupied entry may have a
zero length, which represents a valid empty record. Slot IDs are zero-based
directory indices; physical byte offsets are not record identifiers. Future
deletion will leave entries in place and reuse the lowest free slot ID.

## Validation rules

The validator rejects unsupported magic/version values, nonzero reserved
bytes, slot counts that cannot fit, and data boundaries outside the page or
before the directory end. For every occupied slot it checks that the offset
and length fit within the page, that the record begins at or after the data
boundary, and that nonempty record extents do not overlap.

The maximum possible slot count under this format is
`(8192 - 8) / 6 = 1364`. Bounds are checked before calculating record ends, so
validation does not rely on overflowing `offset + length` arithmetic.

An uninitialized page and a malformed/corrupted page both make `isValid()`
return `false`; the API does not currently distinguish those cases. Validation
never modifies bytes. `initialize()` is explicit and destructive: it clears the
whole page, writes the magic/version, sets the slot count to zero, and sets the
data boundary to `PAGE_SIZE` (encoded as `0x2000`).

## Tests

`slotted_page_test` checks initialization and empty-page validity; invalid
magic/version; excessive slot count; data boundaries before the directory and
beyond the page; and malformed slot state, reserved byte, free-entry extent,
out-of-page record extent, and overlapping live records. Record operations,
fragmentation behavior, and insertion atomicity will be tested in later stages
when those operations exist.
