# MiniDB

MiniDB is an educational relational database project built incrementally in C++20. The current work focuses on the storage foundation: fixed-size pages, file I/O, a buffer pool, and the initial on-page format for slotted pages.

## Current components

- `Page`: an 8,192-byte raw page buffer.
- `DiskManager`: page reads, writes, allocation, and reuse through a `FreeList` strategy.
- `InMemoryFreeList`: process-local free-page tracking; free-page state does not survive restart.
- `BufferPoolManager`: pinned pages, dirty tracking, flushing, and Clock-based eviction.
- `SlottedPage`: a non-owning view over an existing `Page`, currently supporting explicit initialization and format validation only.

Slotted-page insertion, retrieval, deletion, slot reuse, and compaction are not implemented yet. See [the slotted-page format and current scope](docs/slotted_page.md).

## Build and test

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

CTest currently runs four executables: DiskManager persistence, DiskManager robustness, BufferPoolManager behavior, and SlottedPage initialization/validation.

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

The storage foundation is implemented and tested. SlottedPage Stage 2 is complete: format initialization and validation are available, but no record operations have been added. The next step is to review and implement tuple insertion incrementally, followed by retrieval, deletion/reuse, and compaction.

The project deliberately does not yet include schemas, SQL types, heap files, indexes, query execution, transactions, or concurrency.
