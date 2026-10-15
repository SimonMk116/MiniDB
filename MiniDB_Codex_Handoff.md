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
- `SlottedPage` is a non-owning `Page&` view. Stage 2 currently implements its
  versioned byte format, explicit initialization, and validation only.

See [docs/free_list.md](docs/free_list.md) and
[docs/slotted_page.md](docs/slotted_page.md) for storage-format details.

## SlottedPage status

The format has an 8-byte header and 6-byte slot entries. Multibyte values are
little-endian and are encoded byte-by-byte; raw bytes are never reinterpreted as
C++ structs. Slot IDs are directory indices, not data offsets. The validator
checks header fields, directory and data boundaries, slot state/reserved bytes,
record bounds, and overlapping occupied extents.

The next step, after design/code review, is Stage 3: insertion and its
failure-atomicity tests. Retrieval, deletion and slot reuse, then compaction,
remain later incremental steps. Do not implement all record operations at once.

## Build and tests

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

CMake currently registers four test executables: DiskManager persistence,
DiskManager robustness, BufferPoolManager, and SlottedPage initialization and
validation. Record insertion/retrieval/deletion tests do not exist until those
operations are implemented.

## Working constraints

- Inspect current source and Git status before making changes.
- Keep changes incremental and explain invariants and design choices.
- Avoid unrelated refactoring, schemas, SQL types, indexes, heap files, and
  concurrency during the SlottedPage work.
- Run the requested build/tests and report only results actually observed.
- Wait for review between the SlottedPage implementation stages.
