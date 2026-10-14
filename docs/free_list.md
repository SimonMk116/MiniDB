# Free page management

`DiskManager` asks a `FreeList` for reusable page IDs before extending the
database file:

1. `allocatePage()` calls `FreeList::acquirePage()`.
2. If an ID is returned, `DiskManager` zeroes that page and returns the ID.
3. If the result is `std::nullopt`, `DiskManager` appends a new page.
4. `freePage(id)` checks that the ID belongs to the current file, then calls
   `FreeList::releasePage(id)`.

The strategy only tracks IDs; `DiskManager` remains responsible for reading,
writing, and extending the database pages.

## Current strategy

`InMemoryFreeList` is declared in
`include/minidb/storage/free_list.hpp` and implemented in
`src/storage/free_list.cpp`. It uses a vector as a LIFO stack and an unordered
set to reject duplicate frees. Its state disappears when `DiskManager` is
destroyed, so it is suitable for exercising reuse within one process but does
not recover freed IDs after restart.

## Adding another strategy

For a small implementation, add a class beside `InMemoryFreeList` in
`free_list.hpp` and its method definitions in `free_list.cpp`. For a larger
one, add a pair such as:

```text
include/minidb/storage/bitmap_free_list.hpp
src/storage/bitmap_free_list.cpp
```

The new class should inherit `FreeList` and implement `acquirePage()` and
`releasePage()`. `acquirePage()` returns `std::nullopt` when it has no reusable
IDs. `DiskManager` handles appending pages in that case. `releasePage()` may
reject a duplicate ID with an exception, as `InMemoryFreeList` does.

Add the new `.cpp` file to every executable/library target in `CMakeLists.txt`
that compiles `src/storage/disk_manager.cpp`. Those targets need the concrete
strategy's definitions because `DiskManager`'s default argument constructs
`InMemoryFreeList`.

## Selecting a strategy

The `DiskManager` constructor accepts an owning `std::unique_ptr<FreeList>`.
The default argument selects `InMemoryFreeList`:

```cpp
DiskManager disk("database.db");
```

To select another implementation, include its header and pass it explicitly:

```cpp
DiskManager disk(
    "database.db",
    std::make_unique<BitmapFreeList>(/* strategy configuration */));
```

Because the constructor takes a `unique_ptr`, `DiskManager` owns the strategy
for its lifetime. Existing one-argument construction continues to use the
in-memory implementation.

## Persistence design to settle for disk-backed strategies

The current interface has no way for a strategy to perform disk I/O. Before
implementing a persistent linked list or bitmap, decide where its metadata
lives and how it is read and written. Options include reserving database pages
for metadata or giving the strategy a small metadata-storage interface. Avoid
having a strategy depend directly on `DiskManager` without considering the
dependency cycle (`DiskManager` itself owns and calls the strategy).

Also define how startup rebuilds or loads the free IDs, how metadata pages are
kept out of normal allocation, and what happens if a metadata write fails.
Those decisions may require extending `FreeList` or its construction
configuration; the two current methods alone are only enough for an
in-memory strategy.
