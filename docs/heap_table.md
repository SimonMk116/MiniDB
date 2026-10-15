# HeapTable

`HeapTable` stores arbitrary binary records in `SlottedPage` data pages and
returns record IDs composed of a `page_id_t` and a `slot_id_t`. Retrieval and
scanning return owned byte vectors. Records are visited in metadata page order
and then slot-ID order; no sorted order or index is maintained.

## Metadata format

The table root is a dedicated metadata page allocated by
`HeapTable::create()`. The returned root page ID is the table's durable
identifier and must be retained by the caller to reopen the table. This project
does not yet have a catalog or table names.

Root and continuation metadata pages use the same 16-byte header. All
multibyte integers are little-endian; no C++ structs are written directly.

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | Magic bytes `HTBL` |
| 4 | 1 | Format version (`1`) |
| 5 | 1 | Kind (`1` root, `2` continuation) |
| 6 | 2 | Reserved; must be zero |
| 8 | 4 | Next metadata page ID; `0xFFFFFFFF` means no next page |
| 12 | 2 | Number of data page IDs in this page |
| 14 | 2 | Entry capacity; must equal the format capacity |
| 16 onward | 4 each | Data page IDs |

The capacity is `(PAGE_SIZE - 16) / sizeof(page_id_t)`, currently 2,044 IDs.
The loader checks the magic, version, kind, reserved bytes, stored capacity,
entry count, linked-page bounds, cycles, duplicate metadata IDs, duplicate data
IDs, and overlap between metadata and data page IDs. Invalid metadata throws;
it is never silently treated as an empty table.

## Operations and lifecycle

`HeapTable::create(buffer_pool)` allocates and initializes the root metadata
page. Constructing `HeapTable(buffer_pool, root_id)` reopens an existing table
and validates its metadata chain. Insertion tries existing pages in order and
allocates a fresh SlottedPage when none has enough space. A record larger than
the maximum fresh-page payload is rejected before page allocation.

`getRecord()` returns `nullopt` for an ID outside the table, a deleted slot, or
an out-of-range slot. `deleteRecord()` returns false for those cases. Disk I/O
errors and malformed metadata or SlottedPages throw exceptions. Buffer-pool
exhaustion also throws from HeapTable operations; `BufferPoolManager::newPage()`
itself returns `nullptr` without allocating if every frame is pinned.

Every fetched or newly allocated page is unpinned once on all normal and
exception paths. Modified data and metadata pages are marked dirty. Call
`BufferPoolManager::flushAllPages()` before a clean shutdown when persistence is
required; the manager does not flush automatically on destruction.

## Persistence limits

Data-page membership and records survive a clean flush, close, and reopen when
the caller retains the root page ID. The free list remains process-local, and
there is no named-table catalog, WAL, transaction support, crash recovery, or
atomic multi-page update. An I/O failure during page-list growth can leave an
allocated but unreferenced page. The implementation is single-threaded.

`heap_table_test` covers allocation/pinning, empty and multi-page tables,
insertion, retrieval, deletion, scans, empty records, slot reuse, oversized
records, malformed metadata, duplicate IDs, corrupted SlottedPages, exception
path unpinning, and clean-close/reopen persistence.
