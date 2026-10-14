// =============================================================================
// index.h - the in-memory database of every file on the drive
// =============================================================================
//
// THE CENTRAL IDEA
// ----------------
// A naive design stores one std::wstring per file:
//
//     struct File { std::wstring name; std::wstring fullPath; ... };
//     std::vector<File> files;      // 600,000 of these
//
// That is slow and fat for three reasons:
//
//   1. Every std::wstring is its own heap allocation. 600k allocations is slow
//      to build, slow to free, and fragments memory.
//   2. Each one carries ~32 bytes of bookkeeping (pointer, size, capacity) on
//      top of the actual characters.
//   3. Comparing two of them means following a pointer into a random part of
//      the heap - a cache miss almost every time. In a search that touches
//      every file, cache misses ARE the runtime.
//
// So instead we use "structure of arrays" with a string pool:
//
//     nameBlob:  [r][e][p][o][r][t][.][p][d][f][m][a][i][n][.][c][p][p] ...
//                 ^                            ^
//                 offset 0, length 10          offset 10, length 8
//
//     files[0] = { nameOffset=0,  nameLen=10, parentDir=7, size=..., ... }
//     files[1] = { nameOffset=10, nameLen=8,  parentDir=3, size=..., ... }
//
// One allocation for all names. Entries are fixed-size and sit back to back,
// so scanning them streams linearly through memory - the case the CPU
// prefetcher handles best.
//
//
// PATH COMPRESSION
// ----------------
// We never store full paths. Storing "C:\Users\parvb\Documents\" once per file
// would waste enormous space - a deep folder with 500 files repeats the same
// 40 characters 500 times.
//
// Instead there is a separate directory table where each folder appears ONCE
// and points at its parent:
//
//     dirs[0] = { name="C:",        parent=ROOT }
//     dirs[3] = { name="Users",     parent=0    }
//     dirs[7] = { name="parvb",     parent=3    }
//     dirs[9] = { name="Documents", parent=7    }
//
//     files[1].parentDir = 9
//
// To display a full path we walk the chain backwards (9 -> 7 -> 3 -> 0) and
// reverse it. That costs a few microseconds, but we only ever do it for the
// ~40 rows actually visible on screen, never for all 600,000.
//
// This is the same trick the "Everything" search tool uses, and it is the
// difference between a ~500 MB index and a ~100 MB one.
//
//
// THE BIGGEST TRAP IN THIS CODEBASE
// ---------------------------------
// Names in the blob are packed with NO separators, so THEY ARE NOT
// NUL-TERMINATED. Every C string function assumes a terminating zero, so all
// of these will happily read into the next filename and past the end of the
// last one:
//
//     wcscmp  wcsncmp  _wcsicmp  _wcsnicmp  wcslen  wcscpy  StringCchCopyW
//
// ALWAYS use the length-carrying forms instead: std::wmemcmp with an explicit
// count, and compare lengths separately. compareNames() below does this
// correctly - use it rather than rolling your own.
// =============================================================================
#pragma once

#include <vector>
#include <string>
#include <utility>
#include <cstdint>
#include <cwchar>
#include <type_traits>

// -----------------------------------------------------------------------------
// FileEntry - one file. Exactly 32 bytes, no padding holes.
// -----------------------------------------------------------------------------
// Field ORDER matters here. The compiler must align each member to its own
// size, so declaring them small-to-large leaves gaps:
//
//     uint32_t a;  // bytes 0-3
//     uint64_t b;  // must start at byte 8, so bytes 4-7 are WASTED padding
//
// Declaring largest-first packs them with no holes. That matters because:
//   * it saves 8 bytes x 600,000 files = ~5 MB, and
//   * padding bytes are uninitialised, so they would make our saved index file
//     differ run to run for no reason, and could shift silently if a field or
//     compiler ever changed while the format version stayed the same.
//
// We deliberately do NOT use #pragma pack: that would let a uint64_t land on a
// 4-byte boundary, which is slower and undefined behaviour to access on some
// architectures. Manual ordering gets the same result safely.
// -----------------------------------------------------------------------------
struct FileEntry {
    uint64_t size;        // file size in bytes
    uint64_t mtime;       // last modified, as a FILETIME (100ns ticks since 1601)
    uint32_t nameOffset;  // index into FileIndex::nameBlob
    uint32_t parentDir;   // index into FileIndex::dirs
    uint32_t attrs;       // FILE_ATTRIBUTE_* bit flags from Windows
    uint16_t nameLen;     // length in wchar_t, NOT bytes
    uint16_t _pad;        // explicit, so the 32-byte size is intentional
};

static_assert(sizeof(FileEntry) == 32,
              "FileEntry must stay 32 bytes - the on-disk format depends on it");
static_assert(std::is_trivially_copyable_v<FileEntry>,
              "FileEntry must be memcpy-able so we can dump it straight to disk");

// -----------------------------------------------------------------------------
// DirEntry - one directory. 12 bytes.
// -----------------------------------------------------------------------------
struct DirEntry {
    uint32_t nameOffset;  // index into nameBlob
    uint32_t parent;      // index into dirs, or FileIndex::ROOT to stop
    uint16_t nameLen;
    uint16_t _pad;
};

static_assert(sizeof(DirEntry) == 12, "DirEntry layout changed");
static_assert(std::is_trivially_copyable_v<DirEntry>, "DirEntry must be memcpy-able");

// -----------------------------------------------------------------------------
// compareNames - the ONE correct way to compare two packed names
// -----------------------------------------------------------------------------
// Returns <0, 0 or >0 like strcmp. Safe on non-NUL-terminated data because it
// is told both lengths up front.
//
// Compares the shared prefix first, and only if that is equal does the shorter
// string sort first. So "rep" < "report", and "apple" < "banana".
//
// This defines a "strict weak ordering", which std::sort REQUIRES. An
// inconsistent comparator is undefined behaviour and in practice makes
// std::sort walk off the end of its partition and crash.
inline int compareNames(const wchar_t* a, size_t la, const wchar_t* b, size_t lb) {
    size_t n = (la < lb) ? la : lb;
    if (n > 0) {
        int c = std::wmemcmp(a, b, n);
        if (c != 0) return c;
    }
    if (la < lb) return -1;
    if (la > lb) return  1;
    return 0;
}

// -----------------------------------------------------------------------------
// FileIndex
// -----------------------------------------------------------------------------
class FileIndex {
public:
    // Sentinel meaning "this directory has no parent" (i.e. it is a drive root).
    static constexpr uint32_t ROOT    = 0xFFFFFFFFu;
    static constexpr uint32_t INVALID = 0xFFFFFFFFu;

    // Windows caps a single path component at 255 characters.
    static constexpr size_t MAX_NAME = 255;

    // --- storage ------------------------------------------------------------
    // These are public on purpose. FileIndex is a plain data holder, not an
    // encapsulation exercise: the scanner fills it, search reads it, and
    // persist dumps it to disk. Hiding the vectors behind getters would add
    // noise without preventing any mistake we actually care about.

    std::vector<wchar_t>   nameBlob;        // all names, original case
    std::vector<wchar_t>   nameBlobFolded;  // same names, UPPERCASED

    std::vector<FileEntry> files;
    std::vector<DirEntry>  dirs;

    // File IDs sorted by folded name. Because it is sorted, every prefix
    // occupies one CONTIGUOUS range, which is what makes both binary search
    // and the trie work. See search.h.
    std::vector<uint32_t>  sortedByName;

    // ---- building -----------------------------------------------------------

    // Adds a directory and returns its ID. Pass ROOT as parent for a drive root.
    uint32_t addDir(const wchar_t* name, size_t len, uint32_t parent);

    // Adds a file and returns its ID.
    uint32_t addFile(const wchar_t* name, size_t len, uint32_t parentDir,
                     uint64_t size, uint64_t mtime, uint32_t attrs);

    // Builds sortedByName. Call once after all files are added; the trie and
    // every prefix search depend on it.
    void buildSortIndex();

    void clear();

    // Pre-allocates so the vectors do not repeatedly reallocate during a scan.
    void reserve(size_t expectedFiles, size_t expectedDirs);

    // ---- reading ------------------------------------------------------------

    size_t fileCount() const { return files.size(); }
    size_t dirCount()  const { return dirs.size();  }
    bool   empty()     const { return files.empty(); }

    // Pointer to a file's name. REMEMBER: not NUL-terminated. Always pair it
    // with nameLen() and use compareNames() / wmemcmp.
    const wchar_t* name(uint32_t fileId) const {
        return nameBlob.data() + files[fileId].nameOffset;
    }
    const wchar_t* nameFolded(uint32_t fileId) const {
        return nameBlobFolded.data() + files[fileId].nameOffset;
    }
    size_t nameLen(uint32_t fileId) const { return files[fileId].nameLen; }

    // Safe, copying accessor for when you need a real string (display, etc).
    std::wstring nameStr(uint32_t fileId) const {
        return std::wstring(name(fileId), nameLen(fileId));
    }

    // Rebuilds a full path by walking the parent chain. Costs a few
    // microseconds - fine for visible rows, do NOT call it in a search loop.
    std::wstring dirPath(uint32_t dirId) const;
    std::wstring fullPath(uint32_t fileId) const;

    // ---- prefix lookup ------------------------------------------------------
    //
    // Finds the half-open range of sortedByName whose folded names begin with
    // the folded query. Searches only within [lo, hi), so it can either scan
    // the whole index (pass 0, sortedByName.size()) or narrow a range the trie
    // already found.
    //
    // WHY A CONTIGUOUS RANGE EXISTS AT ALL
    // ------------------------------------
    // sortedByName is in plain lexicographic order, so every name starting
    // with "rep" is grouped together with nothing else in between:
    //
    //     ... remove.txt  rep.txt  repair.c  report.pdf  reset.ini ...
    //                     |_____________________________|
    //                      everything starting with "rep"
    //
    // Two binary searches find the two edges. That is ~21 comparisons for a
    // million files, which is why prefix search is effectively instant.
    //
    // This contiguity is the assumption the trie also relies on, and it holds
    // ONLY for plain code-unit ordering. It would break under linguistic
    // collation (CompareStringW, lstrcmpiW, wcscoll), where punctuation such
    // as a hyphen is treated as a tiebreaker rather than a character - there,
    // "coop", "co-op" and "cool" can interleave and the "co-" group is no
    // longer contiguous. Never sort this array with those functions.
    //
    // Returns {0, 0} when nothing matches.
    std::pair<size_t, size_t> prefixRange(size_t lo, size_t hi,
                                          const wchar_t* foldedQuery,
                                          size_t queryLen) const;

    // Convenience overload: search the whole index.
    std::pair<size_t, size_t> prefixRange(const wchar_t* foldedQuery,
                                          size_t queryLen) const {
        return prefixRange(0, sortedByName.size(), foldedQuery, queryLen);
    }

    // Total heap bytes held, for the status bar and benchmarks.
    size_t memoryBytes() const;

    // Checks every offset and parent link is in range. Used after loading an
    // index from disk, where the bytes cannot be trusted. Returns false on the
    // first problem found.
    bool validate() const;

private:
    // Appends one name to BOTH blobs and returns the shared offset.
    //
    // The two blobs MUST always be appended together, and this is the only
    // function that touches them. If they ever end up different lengths the
    // shared-offset invariant breaks and every file starts displaying some
    // other file's name - a spectacular and very confusing failure.
    uint32_t addName(const wchar_t* n, size_t len);
};
