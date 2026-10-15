# MiniDB

MiniDB is an educational relational database project built incrementally in C++20. The current work focuses on the storage foundation: fixed-size pages, file I/O, a buffer pool, and the initial on-page format for slotted pages.

## Current components

- `Page`: an 8,192-byte raw page buffer.
- `DiskManager`: page reads, writes, allocation, and reuse through a `FreeList` strategy.
- `InMemoryFreeList`: process-local free-page tracking; free-page state does not survive restart.
- `BufferPoolManager`: pinned pages, dirty tracking, flushing, and Clock-based eviction.
- `SlottedPage`: a non-owning view over an existing `Page`, supporting format initialization/validation, binary record insertion/retrieval, deletion, lowest-free-slot reuse, and on-demand compaction.
- `HeapTable`: multi-page binary record storage with composite page/slot IDs, scans, deletion, and a persistent metadata page chain.

See [the slotted-page format and implementation](docs/slotted_page.md) for its byte layout, invariants, and API behavior.

## Build and test

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

CTest currently runs five executables, including HeapTable behavior and clean-reopen tests. See [HeapTable format and lifecycle](docs/heap_table.md).

## Repository structure

```text
include/minidb/storage/  public storage interfaces
src/storage/            storage implementations
tests/                  standalone test executables registered with CTest
docs/                   storage design notes
```

The executable and test targets are declared in `CMakeLists.txt`.

## Storage model

Pages are fixed at 8 KiB. The disk file is treated as a contiguous sequence of pages, with byte offset `page_id * PAGE_SIZE`. Higher-level structures interpret page bytes without changing the page-storage contract.

The BufferPoolManager is single-threaded. It tracks frame pins and dirty state, uses a second-chance Clock replacer for unpinned frames, and propagates DiskManager I/O errors. Callers explicitly unpin and flush pages; no RAII page guards are implemented yet.

The current FreeList implementation is in-memory only. See [docs/free_list.md](docs/free_list.md) for allocation and persistence limitations.

## Development status

The storage foundation includes tested SlottedPage and HeapTable record operations. HeapTable persistence requires the caller to retain the metadata root page ID and flush the buffer pool before shutdown; there is not yet a named-table catalog or crash recovery.

The project deliberately does not yet include schemas, SQL types, indexes, query execution, transactions, WAL, crash recovery, or concurrency.
