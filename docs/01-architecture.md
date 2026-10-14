# 1. Architecture — how the pieces fit together

## The problem

You want to find a file and you roughly remember its name. Windows Search takes
seconds, because it maintains a heavyweight index of file *contents* and queries
it through a database layer designed for far more complex questions than
"which files are called something like this".

For the name-only case you do not need any of that. What you need is:

1. Every filename on the drive, held in memory, in a compact form.
2. A way to jump straight to the matching ones.

That is the whole program. Everything in `src/` exists to do those two things
well.

## The five stages

```
   +-----------+
   |  DISK     |   433,198 files spread over 56,203 folders
   +-----------+
         |
         |  (1) SCAN         scanner.cpp — FindFirstFileExW, depth-first
         v                                  ~25 seconds, once
   +-----------+
   |  INDEX    |   index.cpp — flat arrays, ~70 MB in RAM
   +-----------+
         |
         |  (2) SORT         index.cpp — sort file IDs by name, ~300 ms
         v
   +-----------+
   | sortedBy  |   every prefix is now a contiguous range
   |   Name    |
   +-----------+
         |
         |  (3) BUILD TRIE   trie.cpp — a shortcut into that array, ~110 ms
         v
   +-----------+
   |   TRIE    |
   +-----------+
         |
         |  (4) SEARCH       search.cpp — prefix + substring, then rank
         v
   +-----------+
   |  RESULTS  |   at most 1,000, ordered best-first
   +-----------+
         |
         |  (5) DISPLAY      mainwnd.cpp — virtual list, ~30 rows drawn
         v
   +-----------+
   |  SCREEN   |
   +-----------+
```

Stages 1–3 happen once. Stages 4–5 happen on every keystroke, which is why
almost every design decision in this project is about making them cheap.

Stages 1–3 are also skipped entirely on most runs: the finished index is saved
to disk, and loading it back takes **52 ms** instead of 25 seconds.

## The modules

Dependencies only ever point downward. Nothing lower knows anything about
what is above it.

```
                  main.cpp
                     |
                  mainwnd.cpp          <- Win32 GUI
                     |
      +--------------+--------------+
      |              |              |
  search.cpp    persist.cpp    scanner.cpp
      |              |              |
    trie.cpp         |              |
      |              |              |
      +--------------+--------------+
                     |
                  index.cpp            <- the data model
                     |
                   util.cpp            <- case folding, paths, timing
```

| Module | Responsibility |
|---|---|
| `util` | Case folding, long-path handling, timing, number formatting |
| `index` | The data model. Stores files, rebuilds paths, prefix binary search |
| `scanner` | Walks the filesystem and fills an index |
| `trie` | Prefix tree built over the sorted index |
| `search` | Runs a query and ranks the results |
| `persist` | Saves and loads the index |
| `mainwnd` | The window, its controls, and message handling |
| `main` | Entry point and message loop |

Two things follow from this shape:

**The engine can be tested without the GUI.** `cli.cpp` is a console front end
over the exact same modules. Every correctness test — including `--selftest`,
which checks the trie against the binary search on thousands of real queries —
runs there, where a wrong answer is obviously a search bug and not a listview
message you got subtly wrong.

**A faster scanner could be dropped in.** The "Everything" tool scans a drive in
~3 seconds by reading the NTFS Master File Table directly, rather than walking
directories. That needs administrator rights, so this project does not do it —
but if it did, only `scanner.cpp` would change. Nothing above it knows or cares
how the index got filled.

## Threading

Only two threads ever exist, and the split is deliberate.

```
   UI THREAD                          SCAN THREAD
   ---------                          -----------
   message loop                       walks the disk
   searches the index                 builds a SEPARATE index
   draws rows                              |
        ^                                  | every ~100 ms
        |  PostMessage(numbers)            |
        +----------------------------------+
        |                                  |
        |  PostMessage(done)          when finished:
        +----------------------------------+  hand over the whole
                                              index under a mutex
```

Two rules make this safe, and both are easy to get wrong:

**Only the UI thread touches the window.** The scan thread never calls anything
that draws. It calls `PostMessage`, which is thread-safe by design: it drops a
note in the window's queue and returns immediately. The UI thread picks it up
whenever it next checks. Progress is reported as plain numbers in the message
rather than a pointer, so there is nothing to leak if the post fails.

**The index is replaced, never modified in place.** The obvious approach — have
the scan thread append to the index the UI is searching — is a use-after-free
waiting to happen, because `std::vector` grows by allocating a bigger block and
**freeing the old one**. If the UI thread is midway through a search when that
happens, it is reading memory that has just been handed back to the allocator.
It will work in testing and crash at random later.

So the scanner builds an entirely separate `IndexBundle` and hands over the
finished article. The UI swaps a `shared_ptr` to point at the new one; the old
one is freed automatically once nothing is using it. The search path needs no
locks at all, and can never see a half-built index.

## Where the time goes

Measured on a 476 GB NTFS drive with 433,198 indexed files:

| Stage | Cost | How often |
|---|---|---|
| Scan (cold cache) | 24.5 s | Once, or on F5 |
| Scan (warm cache) | 4.4 s | |
| Sort | 283 ms | After each scan |
| Build trie | 110 ms | After each scan or load |
| **Load saved index** | **52 ms** | Every startup after the first |
| Prefix search | 0.27 µs | Every keystroke |
| Full search with substring | 15–40 ms | Every keystroke (debounced) |

The dominant cost by far is the initial scan, and the saved index exists
specifically to make you pay it only once.

The one figure worth noticing is the gap between a prefix search (microseconds)
and a full search including the substring pass (tens of milliseconds) — a
factor of about 50,000. [Document 3](03-indexing.md) explains why that gap
exists and why closing it is not worth doing.
