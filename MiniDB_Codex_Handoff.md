# MiniDB handoff

## Project

MiniDB is a step-by-step educational relational database written in C++20 and
built with CMake. The current platform is Linux. `PAGE_SIZE` is centrally
defined as 8,192 bytes in `include/minidb/storage/page.hpp`.

## Current implementation

- `Page` owns a fixed-size byte array.
- `DiskManager` reads/writes pages and allocates sequential or reusable page
  IDs. Its default `InMemoryFreeList` does not persist free-page metadata.
- `BufferPoolManager` owns a configurable number of frames (default 16), tracks
  page pins and dirty state, flushes dirty pages, and evicts using the
  second-chance Clock policy. It is single-threaded.
- `SlottedPage` is a non-owning `Page&` view. It supports a versioned byte
  format, explicit initialization and validation, binary record insertion and
  copied retrieval, deletion, lowest-free-slot reuse, compaction, and a
  validated `slotCount()` accessor.
- `HeapTable` stores records across SlottedPages. It uses composite page/slot
  record IDs and a persistent root plus linked metadata pages for membership.
  The caller retains the root page ID to reopen a table.

See [docs/free_list.md](docs/free_list.md),
[docs/slotted_page.md](docs/slotted_page.md), and
[docs/heap_table.md](docs/heap_table.md) for storage details.

## SlottedPage status

The format has an 8-byte header and 6-byte slot entries. Multibyte values are
little-endian and are encoded byte-by-byte; raw bytes are never reinterpreted as
C++ structs. Slot IDs are directory indices, not data offsets. The validator
checks header fields, directory and data boundaries, slot state/reserved bytes,
record bounds, and overlapping occupied extents.

Insertion validates and preflights total space before mutation. If fragmented,
it compacts only after confirming the insertion can fit, so failed insertions
leave the full page byte-for-byte unchanged. Deletion leaves directory entries
in place; insertion reuses the lowest free slot. Compaction preserves live slot
IDs and rewrites only physical record offsets.

## Build and tests

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

CMake registers five test executables: DiskManager persistence, DiskManager
robustness, BufferPoolManager, SlottedPage, and HeapTable. HeapTable tests cover
metadata validation, record operations, scans, buffer-pool pin cleanup, and
clean-close/reopen persistence.

## Working constraints

- Inspect current source and Git status before making changes.
- Keep changes incremental and explain invariants and design choices.
- Avoid unrelated refactoring, schemas, SQL types, indexes, WAL, transactions,
  crash recovery, and concurrency during the HeapTable work.
- Run the requested build/tests and report only results actually observed.
- Wait for review before implementing additional higher-level components.
