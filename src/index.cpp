// =============================================================================
// index.cpp - implementation of the in-memory file database. See index.h.
// =============================================================================
#include "index.h"
#include "util.h"

#include <algorithm>
#include <numeric>
#include <stdexcept>

// A path cannot realistically nest deeper than this. Used as a safety stop when
// walking the parent chain: if a corrupt index ever contained a cycle
// (dir A's parent is B, B's parent is A) an unguarded loop would hang forever.
static constexpr int MAX_PATH_DEPTH = 512;

// -----------------------------------------------------------------------------
// addName - append one name to both blobs
// -----------------------------------------------------------------------------
// This is the only place either blob is written. Keeping it in one function is
// what guarantees the two stay exactly the same length, which the shared
// nameOffset depends on.
uint32_t FileIndex::addName(const wchar_t* n, size_t len) {
    // nameOffset is a uint32_t, so the blob can hold at most ~4.29 billion
    // wchar_t (8.6 GB of text). We would need ~200 million files to get there,
    // but check anyway: silently wrapping around would corrupt every entry
    // added afterwards, and the symptom (random wrong filenames) would be
    // baffling.
    if (nameBlob.size() + len > 0xFFFFFFFFull) {
        throw std::runtime_error("index.cpp: name blob exceeded 4G characters");
    }

    const uint32_t offset = static_cast<uint32_t>(nameBlob.size());

    // Append the original-case copy.
    nameBlob.insert(nameBlob.end(), n, n + len);

    // Append the same characters again, then uppercase just that new region.
    // We fold in place rather than folding into a temporary and copying,
    // because this runs once per file and allocations here are what make a
    // scan slow.
    const size_t foldedStart = nameBlobFolded.size();
    nameBlobFolded.insert(nameBlobFolded.end(), n, n + len);
    if (len > 0) {
        util::foldCaseInPlace(nameBlobFolded.data() + foldedStart, len);
    }

    return offset;
}

// -----------------------------------------------------------------------------
// addDir / addFile
// -----------------------------------------------------------------------------
uint32_t FileIndex::addDir(const wchar_t* n, size_t len, uint32_t parent) {
    if (len > MAX_NAME) len = MAX_NAME;   // defensive; Windows enforces 255

    DirEntry d{};
    d.nameOffset = addName(n, len);
    d.parent     = parent;
    d.nameLen    = static_cast<uint16_t>(len);
    d._pad       = 0;

    dirs.push_back(d);
    return static_cast<uint32_t>(dirs.size() - 1);
}

uint32_t FileIndex::addFile(const wchar_t* n, size_t len, uint32_t parentDir,
                            uint64_t size, uint64_t mtime, uint32_t attrs) {
    if (len > MAX_NAME) len = MAX_NAME;

    FileEntry f{};
    f.size       = size;
    f.mtime      = mtime;
    f.nameOffset = addName(n, len);
    f.parentDir  = parentDir;
    f.attrs      = attrs;
    f.nameLen    = static_cast<uint16_t>(len);
    f._pad       = 0;

    files.push_back(f);
    return static_cast<uint32_t>(files.size() - 1);
}

// -----------------------------------------------------------------------------
// buildSortIndex
// -----------------------------------------------------------------------------
// Produces sortedByName: every file ID, ordered by folded name.
//
// We sort a vector of IDs rather than sorting the FileEntry array itself. Two
// reasons: moving 32-byte structs around is more work than moving 4-byte IDs,
// and file IDs stay stable so anything else holding one does not break.
void FileIndex::buildSortIndex() {
    sortedByName.resize(files.size());

    // Fill with 0, 1, 2, ... n-1.
    std::iota(sortedByName.begin(), sortedByName.end(), 0u);

    std::sort(sortedByName.begin(), sortedByName.end(),
              [this](uint32_t a, uint32_t b) {
                  const int c = compareNames(nameFolded(a), nameLen(a),
                                             nameFolded(b), nameLen(b));
                  if (c != 0) return c < 0;

                  // TIEBREAKER - not optional.
                  // Duplicate filenames are extremely common (think all the
                  // README.md files on a drive). Without a tiebreaker their
                  // relative order is unspecified, which means:
                  //   * the saved index file differs between runs, and
                  //   * results visibly reshuffle when you retype the same
                  //     query.
                  // Ordering equal names by ID makes the sort deterministic.
                  return a < b;
              });
}

// -----------------------------------------------------------------------------
// Path reconstruction
// -----------------------------------------------------------------------------
// Walks the parent chain from a directory up to its drive root, then emits the
// names in reverse (root first).
//
//   dirs[9] = Documents -> dirs[7] = parvb -> dirs[3] = Users -> dirs[0] = "C:"
//   result: C:\Users\parvb\Documents
std::wstring FileIndex::dirPath(uint32_t dirId) const {
    if (dirId == ROOT || dirId >= dirs.size()) return std::wstring();

    // Collect the chain bottom-up into a small fixed buffer. A stack array
    // avoids a heap allocation on a function that runs for every visible row.
    uint32_t chain[MAX_PATH_DEPTH];
    int depth = 0;

    uint32_t cur = dirId;
    while (cur != ROOT && cur < dirs.size() && depth < MAX_PATH_DEPTH) {
        chain[depth++] = cur;
        cur = dirs[cur].parent;
    }

    // Now emit top-down. The root entry holds "C:", so the result starts
    // "C:\..." with no leading separator needed.
    std::wstring out;
    size_t total = 0;
    for (int i = 0; i < depth; ++i) total += dirs[chain[i]].nameLen + 1;
    out.reserve(total);

    for (int i = depth - 1; i >= 0; --i) {
        const DirEntry& d = dirs[chain[i]];
        out.append(nameBlob.data() + d.nameOffset, d.nameLen);
        if (i > 0) out.push_back(L'\\');
    }
    return out;
}

std::wstring FileIndex::fullPath(uint32_t fileId) const {
    if (fileId >= files.size()) return std::wstring();
    const FileEntry& f = files[fileId];
    return util::joinPath(dirPath(f.parentDir),
                          nameBlob.data() + f.nameOffset, f.nameLen);
}

// -----------------------------------------------------------------------------
// prefixRange - two binary searches over the sorted order
// -----------------------------------------------------------------------------
std::pair<size_t, size_t> FileIndex::prefixRange(size_t lo, size_t hi,
                                                 const wchar_t* q,
                                                 size_t qlen) const {
    if (qlen == 0 || lo >= hi || hi > sortedByName.size()) return { 0, 0 };

    // Classifies one entry relative to the group of names starting with q:
    //     < 0  sorts BEFORE the group
    //     = 0  is IN the group (i.e. the name starts with q)
    //     > 0  sorts AFTER the group
    //
    // The subtle case is a name SHORTER than the query whose characters all
    // match, such as name "rep" against query "report". It does not start with
    // the query, and compareNames puts shorter names first, so it belongs
    // before the group - hence the explicit -1.
    auto classify = [&](uint32_t fileId) -> int {
        const wchar_t* n  = nameFolded(fileId);
        const size_t   nl = nameLen(fileId);
        const size_t   m  = (nl < qlen) ? nl : qlen;

        if (m > 0) {
            const int c = std::wmemcmp(n, q, m);
            if (c != 0) return c;
        }
        if (nl < qlen) return -1;
        return 0;
    };

    // First edge: the first entry that is NOT before the group.
    size_t a = lo, b = hi;
    while (a < b) {
        const size_t mid = a + (b - a) / 2;
        if (classify(sortedByName[mid]) < 0) a = mid + 1;
        else                                 b = mid;
    }
    const size_t begin = a;

    // Second edge: the first entry that is AFTER the group. Starting this
    // search at begin rather than lo saves a few comparisons.
    a = begin;
    b = hi;
    while (a < b) {
        const size_t mid = a + (b - a) / 2;
        if (classify(sortedByName[mid]) <= 0) a = mid + 1;
        else                                  b = mid;
    }
    const size_t end = a;

    if (begin >= end) return { 0, 0 };
    return { begin, end };
}

// -----------------------------------------------------------------------------
// Housekeeping
// -----------------------------------------------------------------------------
void FileIndex::clear() {
    // Swapping with a fresh empty vector actually RELEASES the memory.
    // clear() alone keeps the capacity allocated, which would mean a rescan
    // held two full indexes worth of RAM at once.
    std::vector<wchar_t>().swap(nameBlob);
    std::vector<wchar_t>().swap(nameBlobFolded);
    std::vector<FileEntry>().swap(files);
    std::vector<DirEntry>().swap(dirs);
    std::vector<uint32_t>().swap(sortedByName);
}

void FileIndex::reserve(size_t expectedFiles, size_t expectedDirs) {
    // Growing a vector means allocating a bigger block and copying everything
    // across. Reserving up front turns ~20 reallocations of a 100 MB buffer
    // into one, which is a large chunk of scan time.
    files.reserve(expectedFiles);
    dirs.reserve(expectedDirs);

    // Guess an average name length of 16 characters for the blobs.
    const size_t blobGuess = (expectedFiles + expectedDirs) * 16;
    nameBlob.reserve(blobGuess);
    nameBlobFolded.reserve(blobGuess);
}

size_t FileIndex::memoryBytes() const {
    // capacity(), not size(): we want what is actually held from the OS.
    return nameBlob.capacity()       * sizeof(wchar_t)
         + nameBlobFolded.capacity() * sizeof(wchar_t)
         + files.capacity()          * sizeof(FileEntry)
         + dirs.capacity()           * sizeof(DirEntry)
         + sortedByName.capacity()   * sizeof(uint32_t);
}

// -----------------------------------------------------------------------------
// validate
// -----------------------------------------------------------------------------
// Confirms every index stored inside the structure actually points somewhere
// legal. This matters after loading index.bin from disk: those bytes could be
// truncated, from an older build, or corrupted. Without this check a bad
// nameOffset would not fail here - it would fail much later as an access
// violation deep inside a paint handler, which is far harder to diagnose.
//
// Costs about 10ms for a million entries. Cheap insurance.
bool FileIndex::validate() const {
    // The two blobs must be the same length, or every shared offset is wrong.
    if (nameBlob.size() != nameBlobFolded.size()) return false;

    const size_t blobSize = nameBlob.size();
    const size_t dirCnt   = dirs.size();
    const size_t fileCnt  = files.size();

    for (const DirEntry& d : dirs) {
        // Offset + length must stay inside the blob.
        if (static_cast<size_t>(d.nameOffset) + d.nameLen > blobSize) return false;
        // Parent must be a real directory, or the ROOT sentinel.
        if (d.parent != ROOT && d.parent >= dirCnt) return false;
    }

    for (const FileEntry& f : files) {
        if (static_cast<size_t>(f.nameOffset) + f.nameLen > blobSize) return false;
        if (f.parentDir != ROOT && f.parentDir >= dirCnt) return false;
    }

    // sortedByName must be a permutation of valid file IDs. We do not verify it
    // is actually sorted here (that would cost a full comparison pass); the
    // range check is what prevents out-of-bounds reads.
    if (!sortedByName.empty() && sortedByName.size() != fileCnt) return false;
    for (uint32_t id : sortedByName) {
        if (id >= fileCnt) return false;
    }

    return true;
}
