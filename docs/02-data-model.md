# 2. The data model — why flat arrays beat objects here

This is the most important document of the five. Every other design decision in
the project falls out of what is described here.

## The design you would write first

```cpp
struct File {
    std::wstring name;
    std::wstring fullPath;
    uint64_t     size;
    uint64_t     modified;
};
std::vector<File> files;          // 433,198 of these
```

This is clear, obvious, and the right shape for most programs. Here it is
roughly **seven times too big and far too slow**. It is worth understanding
exactly why, because the reasons are not obvious and they generalise.

### Cost 1: every string is a separate heap allocation

`std::wstring` stores its characters in its own block of memory. So this design
performs **866,396 allocations** (one per name, one per path). Allocation is not
free — and freeing them all at exit takes a noticeable moment too.

### Cost 2: the bookkeeping outweighs the data

A `std::wstring` is about 32 bytes of *structure* — a pointer, a length, a
capacity, and a small internal buffer — before a single character is stored.
Two of them per file is ~64 bytes of overhead. The average filename is about 16
characters, or 32 bytes. **The bookkeeping is twice the size of the data.**

### Cost 3: every comparison is a cache miss

This is the one that actually matters.

A modern CPU reads memory in 64-byte chunks called cache lines, and it is
extremely good at guessing what you will want next *if you read in a straight
line*. Reading a random address is roughly 100× slower than reading the next
address in sequence.

With one allocation per string, the names are scattered all over the heap. A
search that checks every file follows 433,198 pointers to 433,198 unrelated
addresses. The CPU cannot predict any of them, so it stalls on nearly every
one.

### Cost 4: full paths are stored over and over

`C:\Users\parvb\AppData\Local\Programs\` is 37 characters. A folder with 500
files in it stores that same prefix **500 times**.

## What this project does instead

### Structure of arrays, with a string pool

All the names go into one continuous array. Each file record stores *where* its
name starts rather than the name itself:

```
nameBlob:  r e p o r t . p d f m a i n . c p p R E A D M E . m d ...
           ^                   ^               ^
           offset 0            offset 10       offset 18
           length 10           length 8        length 9

files[0] = { nameOffset: 0,  nameLen: 10, parentDir: 7, size: ..., mtime: ... }
files[1] = { nameOffset: 10, nameLen:  8, parentDir: 3, size: ..., mtime: ... }
files[2] = { nameOffset: 18, nameLen:  9, parentDir: 7, size: ..., mtime: ... }
```

One allocation for every name on the drive. The records are all exactly 32
bytes, sitting back to back, so walking them is a straight line through
memory — the case the CPU handles best.

### FileEntry: 32 bytes, no holes

```cpp
struct FileEntry {
    uint64_t size;        // bytes  0-7
    uint64_t mtime;       // bytes  8-15
    uint32_t nameOffset;  // bytes 16-19
    uint32_t parentDir;   // bytes 20-23
    uint32_t attrs;       // bytes 24-27
    uint16_t nameLen;     // bytes 28-29
    uint16_t _pad;        // bytes 30-31
};
static_assert(sizeof(FileEntry) == 32);
```

**Field order is not arbitrary.** A CPU requires each value to start at an
address that is a multiple of its own size, so the compiler inserts invisible
padding to make that happen. Declared small-to-large, the same fields become 40
bytes:

```
uint32_t nameOffset;   bytes  0-3
uint16_t nameLen;      bytes  4-5
                       bytes  6-7    <-- 2 wasted
uint32_t parentDir;    bytes  8-11
                       bytes 12-15   <-- 4 wasted
uint64_t size;         bytes 16-23
uint64_t mtime;        bytes 24-31
uint32_t attrs;        bytes 32-35
                       bytes 36-39   <-- 4 wasted
                       = 40 bytes
```

Declaring largest-first packs them with no gaps. That saves 8 bytes × 433,198 =
3.5 MB, and — more importantly — makes the size *deterministic*, which matters
when the struct is written straight to disk.

We deliberately do **not** use `#pragma pack`. That would remove padding by
letting a `uint64_t` start at a 4-byte boundary, which is slower to access and
undefined behaviour on some architectures. Ordering the fields by hand achieves
the same result safely.

### Path compression: folders are stored once

No file record contains a path. Instead there is a second table where each
folder appears exactly once and points at its parent:

```
dirs[0]  = { name: "C:",        parent: ROOT }
dirs[3]  = { name: "Users",     parent: 0    }
dirs[7]  = { name: "parvb",     parent: 3    }
dirs[9]  = { name: "Documents", parent: 7    }

files[1] = { name: "report.pdf", parentDir: 9, ... }
```

To display the full path, walk the chain backwards and reverse it:

```
  9 -> "Documents"
  7 -> "parvb"
  3 -> "Users"
  0 -> "C:"            ROOT reached, stop

  reversed and joined:  C:\Users\parvb\Documents
  plus the filename:    C:\Users\parvb\Documents\report.pdf
```

`C:\Users\parvb\Documents` is stored **once**, no matter how many files are in
it. And walking a four-link chain costs microseconds — which is irrelevant,
because it is only ever done for the ~30 rows actually visible on screen, never
for all 433,198.

This is the same technique the "Everything" tool uses.

## The numbers

Measured on this machine: 433,198 files, 56,203 directories.

| | Naive design | This design |
|---|---|---|
| Heap allocations | ~866,000 | 5 |
| Name storage | ~14 MB | 13.9 MB |
| Path storage | ~260 MB | 1.6 MB (folders once) |
| Per-file overhead | ~64 B | 32 B |
| Records | ~28 MB | 13.9 MB |
| **Total** | **~500 MB** | **52–72 MB** |

About seven times smaller, with every access pattern also far friendlier to the
CPU.

## The trap this creates

Names are packed with **no separators**, which means:

> ### Filenames in the blob are NOT null-terminated.

Every C string function assumes a terminating zero byte to know where to stop.
Given a pointer into the middle of the blob, all of these will read straight
past the end of the name and into the next one — and past the end of the array
entirely for the last name:

```
wcscmp    wcsncmp    _wcsicmp    _wcsnicmp
wcslen    wcscpy     wcsstr      StringCchCopyW
```

The correct form always carries an explicit length:

```cpp
// WRONG - reads past the end of the name
if (wcscmp(idx.name(fileId), L"report.pdf") == 0) { ... }

// RIGHT - length is passed in, so it knows where to stop
if (compareNames(idx.name(fileId), idx.nameLen(fileId),
                 L"report.pdf", 10) == 0) { ... }
```

`compareNames()` in `index.h` is the one correct implementation. Use it.

This is the single easiest out-of-bounds bug to write in this codebase, and it
usually appears as *occasionally* wrong results rather than a crash — because
reading a few bytes past a name normally lands in the next name, which is
perfectly valid memory.

## Two blobs, one set of offsets

There are actually two name arrays:

```cpp
std::vector<wchar_t> nameBlob;        // "Report.PDF"  - as it appears on disk
std::vector<wchar_t> nameBlobFolded;  // "REPORT.PDF"  - all uppercase
```

The folded copy exists so that case-insensitive matching is a plain
character comparison, with no per-character case conversion in the inner loop.
Both use the **same** `nameOffset` and `nameLen`, which is what makes the pairing
free.

That shared-offset arrangement carries one hard requirement:

> The two arrays must always have identical lengths.

Which is why the folding function must be **length-preserving**, and why
`FileIndex::addName()` is the only function that writes to either array — it
appends to both, together, every time. If they ever drifted apart, every file
would begin displaying some *other* file's name.

See [gotcha #3](05-gotchas.md#3-accented-filenames-never-match) for the case
folding function that gets this right and the two obvious candidates that do
not.

## The payoff: saving to disk becomes trivial

Saving a normal C++ object graph is genuinely hard. Pointers are memory
addresses that mean nothing in another process, and every `std::wstring` owns
a separate allocation that has to be found and written individually. Entire
libraries exist to solve this.

Look at what this index actually contains:

```cpp
std::vector<wchar_t>    nameBlob;       // plain characters
std::vector<FileEntry>  files;          // plain numbers
std::vector<DirEntry>   dirs;           // plain numbers
std::vector<uint32_t>   sortedByName;   // plain numbers
```

**No pointers anywhere.** Every reference is an integer index, and the number 9
means the same thing in every process that ever runs. So saving is writing the
bytes out, and loading is reading them back:

```cpp
fwrite(files.data(), 1, files.size() * sizeof(FileEntry), f);
```

That is why startup is 52 ms instead of 25 seconds. It was not a separate
feature that had to be designed — it fell out of a data model chosen for speed.

Good data structure choices tend to pay off more than once.

*(Writing raw bytes to disk does have its own set of traps. See
[document 6 in the reading order](../src/persist.h) and
[gotchas #7 and #8](05-gotchas.md).)*
