// =============================================================================
// persist.cpp - reading and writing index.bin. See persist.h for the design.
// =============================================================================
#include "persist.h"
#include "util.h"

#include <windows.h>
#include <shlobj.h>      // SHGetKnownFolderPath, FOLDERID_LocalAppData
#include <io.h>          // _commit
#include <cstdio>
#include <cstring>      // memcpy, used by the checksum

namespace persist {

namespace {

// "FSIX" spelled out as bytes, so the first four bytes of the file identify it.
// Checking a magic number means we notice immediately if we are handed some
// completely unrelated file, rather than interpreting its bytes as counts.
constexpr uint32_t MAGIC = 0x58495346u;

// The header sits at the very start of the file.
//
// Everything needed to verify the file BEFORE allocating anything is in here.
// That ordering matters: reading a corrupt count and then calling resize() on
// it is how you end up asking Windows for a 130 GB allocation.
struct Header {
    uint32_t magic;
    uint32_t version;

    // sizeof() of each struct at the time of writing. If a field is ever added
    // to FileEntry, this changes and old files are rejected instead of being
    // read with the wrong stride - which would turn every entry after the
    // first into nonsense, with no obvious symptom.
    uint32_t sizeofFileEntry;
    uint32_t sizeofDirEntry;

    uint64_t nameBlobCount;   // number of wchar_t, not bytes
    uint64_t fileCount;
    uint64_t dirCount;
    uint64_t sortedCount;

    // Total size the finished file must have. Comparing this against the
    // actual size on disk catches truncation in one cheap check.
    uint64_t expectedFileSize;

    uint64_t savedAt;         // FILETIME, so the UI can show index age

    // Hash of everything after the header. The size check catches a truncated
    // or padded file, but NOT a file of exactly the right length whose bytes
    // have rotted - a failing disk, a bad cable, or a half-finished write that
    // happened to land on a size boundary. Those would load "successfully"
    // with quietly wrong data in them.
    uint64_t checksum;

    uint64_t reserved;        // room for one more field without a version bump
};

static_assert(sizeof(Header) == 80, "Header layout changed - bump FORMAT_VERSION");

// -----------------------------------------------------------------------------
// hashBytes - fast corruption detection, NOT security
// -----------------------------------------------------------------------------
// This is FNV-1a, chosen because it is about ten lines long and detects any
// single flipped bit. It is NOT a cryptographic hash: someone who wanted to
// could construct a different file with the same value. That is fine - we are
// defending against faulty hardware and interrupted writes, not an attacker.
//
// The loop consumes 8 bytes at a time rather than 1, which makes hashing a
// 35 MB index take a few milliseconds instead of tens.
//
// The seed parameter lets us chain calls across several buffers so the four
// vectors hash as though they were one continuous run of bytes.
uint64_t hashBytes(const void* data, size_t bytes, uint64_t seed) {
    constexpr uint64_t PRIME = 0x100000001b3ull;

    const unsigned char* p = static_cast<const unsigned char*>(data);
    uint64_t h = seed;
    size_t i = 0;

    for (; i + 8 <= bytes; i += 8) {
        uint64_t word;
        // memcpy rather than a pointer cast: the buffer is not guaranteed to
        // be 8-byte aligned, and casting to uint64_t* to read it would be
        // undefined behaviour. Every compiler turns this memcpy into a single
        // load instruction, so it costs nothing.
        memcpy(&word, p + i, 8);
        h = (h ^ word) * PRIME;
    }
    for (; i < bytes; ++i) {
        h = (h ^ p[i]) * PRIME;
    }
    return h;
}

// Convenience overload for hashing one vector, chaining from a previous value.
template <typename T>
uint64_t hashVector(const std::vector<T>& v, uint64_t seed) {
    if (v.empty()) return seed;
    return hashBytes(v.data(), v.size() * sizeof(T), seed);
}

// Starting value for the chain (the standard FNV-1a offset basis).
constexpr uint64_t HASH_SEED = 0xcbf29ce484222325ull;

// Hashes the whole payload in the same order it is written to disk.
uint64_t computeChecksum(const FileIndex& idx) {
    uint64_t h = HASH_SEED;
    h = hashVector(idx.nameBlob,     h);
    h = hashVector(idx.files,        h);
    h = hashVector(idx.dirs,         h);
    h = hashVector(idx.sortedByName, h);
    return h;
}

// RAII wrapper so no early return can leak the FILE handle.
class FileCloser {
public:
    explicit FileCloser(FILE* f) : m_f(f) {}
    ~FileCloser() { if (m_f) fclose(m_f); }

    FileCloser(const FileCloser&)            = delete;
    FileCloser& operator=(const FileCloser&) = delete;

    FILE* get() const { return m_f; }

    // Hands ownership back to the caller, who must then fclose it themselves.
    // Used by save(), which needs to close explicitly before renaming.
    FILE* release() { FILE* f = m_f; m_f = nullptr; return f; }

private:
    FILE* m_f;
};

// Writes one vector's raw bytes. Returns false on a short write.
//
// fwrite returns the number of ELEMENTS written, not bytes - a classic
// off-by-a-lot mistake. We pass the element size as 1 so the return value is
// a byte count and the comparison is straightforward.
template <typename T>
bool writeVector(FILE* f, const std::vector<T>& v) {
    if (v.empty()) return true;
    const size_t bytes = v.size() * sizeof(T);
    return fwrite(v.data(), 1, bytes, f) == bytes;
}

// Reads exactly count elements into v.
template <typename T>
bool readVector(FILE* f, std::vector<T>& v, uint64_t count) {
    v.clear();
    if (count == 0) return true;

    v.resize(static_cast<size_t>(count));

    // resize() on an empty vector can leave data() as nullptr in principle,
    // and passing nullptr to fread is undefined behaviour even with a count
    // of zero. The count == 0 early return above covers it, but the check
    // costs nothing.
    if (v.data() == nullptr) return false;

    const size_t bytes = static_cast<size_t>(count) * sizeof(T);
    return fread(v.data(), 1, bytes, f) == bytes;
}

} // anonymous namespace

// -----------------------------------------------------------------------------
// defaultIndexPath
// -----------------------------------------------------------------------------
std::wstring defaultIndexPath() {
    PWSTR localAppData = nullptr;

    // SHGetKnownFolderPath rather than getenv("LOCALAPPDATA"):
    //   * getenv gives 8-bit text, which mangles non-ASCII user names
    //   * the environment variable can be missing or overridden
    // The API is the supported way and always returns UTF-16.
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData))) {
        return std::wstring();
    }

    std::wstring dir = localAppData;
    CoTaskMemFree(localAppData);   // the API allocated this; we must free it

    dir = util::joinPath(dir, L"FastSearch");

    // Creating a directory that already exists is not an error for us.
    if (!CreateDirectoryW(dir.c_str(), nullptr)) {
        if (GetLastError() != ERROR_ALREADY_EXISTS) return std::wstring();
    }

    return util::joinPath(dir, L"index.bin");
}

// -----------------------------------------------------------------------------
// save
// -----------------------------------------------------------------------------
bool save(const FileIndex& idx, const std::wstring& path) {
    if (path.empty()) return false;

    // Write to a temporary file first. The real index.bin is only replaced
    // once the temporary one is complete and flushed - so a crash or power cut
    // part way through leaves the previous good index intact rather than a
    // half-written file that still passes the magic and version checks.
    const std::wstring tmpPath = path + L".tmp";

    // "wb" - BINARY mode. In text mode the C runtime would rewrite every 0x0A
    // byte as 0x0D 0x0A on the way out, silently corrupting the file every
    // single time. _wfopen rather than fopen so non-ASCII paths work.
    FileCloser fc(_wfopen(tmpPath.c_str(), L"wb"));
    if (!fc.get()) return false;
    FILE* f = fc.get();

    Header h{};
    h.magic            = MAGIC;
    h.version          = FORMAT_VERSION;
    h.sizeofFileEntry  = static_cast<uint32_t>(sizeof(FileEntry));
    h.sizeofDirEntry   = static_cast<uint32_t>(sizeof(DirEntry));
    h.nameBlobCount    = idx.nameBlob.size();
    h.fileCount        = idx.files.size();
    h.dirCount         = idx.dirs.size();
    h.sortedCount      = idx.sortedByName.size();

    // NOTE: nameBlobFolded is deliberately NOT saved. It is regenerated on
    // load from nameBlob by the same folding function. That halves the file
    // size, and more importantly it makes it IMPOSSIBLE for the two blobs to
    // get out of sync on disk - the invariant the whole design rests on is
    // re-established by construction every time we load.
    h.expectedFileSize = sizeof(Header)
                       + h.nameBlobCount * sizeof(wchar_t)
                       + h.fileCount     * sizeof(FileEntry)
                       + h.dirCount      * sizeof(DirEntry)
                       + h.sortedCount   * sizeof(uint32_t);

    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER uli;
    uli.LowPart  = now.dwLowDateTime;
    uli.HighPart = now.dwHighDateTime;
    h.savedAt = uli.QuadPart;

    // Computed over exactly the bytes we are about to write, in the same
    // order, so load() can recompute it and compare.
    h.checksum = computeChecksum(idx);

    bool ok = fwrite(&h, 1, sizeof(h), f) == sizeof(h);
    ok = ok && writeVector(f, idx.nameBlob);
    ok = ok && writeVector(f, idx.files);
    ok = ok && writeVector(f, idx.dirs);
    ok = ok && writeVector(f, idx.sortedByName);

    if (ok) {
        // Two-stage flush, and both stages matter:
        //   fflush  - pushes the C library's buffer into the OS
        //   _commit - tells the OS to actually write its cache to the disk
        // Without _commit the data can still be sitting in RAM when the rename
        // completes, so a power cut would leave a renamed but empty file.
        ok = (fflush(f) == 0) && (_commit(_fileno(f)) == 0);
    }

    // Close before renaming - Windows will not rename an open file.
    fclose(fc.release());

    if (!ok) {
        DeleteFileW(tmpPath.c_str());
        return false;
    }

    // The atomic step. MOVEFILE_REPLACE_EXISTING overwrites the old index in
    // one filesystem operation: at no point does a reader see a partial file.
    if (!MoveFileExW(tmpPath.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmpPath.c_str());
        return false;
    }
    return true;
}

// -----------------------------------------------------------------------------
// load
// -----------------------------------------------------------------------------
LoadResult load(FileIndex& idx, const std::wstring& path) {
    idx.clear();
    if (path.empty()) return LoadResult::FileNotFound;

    FileCloser fc(_wfopen(path.c_str(), L"rb"));   // BINARY mode, see save()
    if (!fc.get()) return LoadResult::FileNotFound;
    FILE* f = fc.get();

    Header h{};
    if (fread(&h, 1, sizeof(h), f) != sizeof(h)) return LoadResult::ReadError;

    // --- checks, cheapest and most decisive first ---------------------------

    if (h.magic != MAGIC)                            return LoadResult::BadMagic;
    if (h.version != FORMAT_VERSION)                 return LoadResult::VersionMismatch;
    if (h.sizeofFileEntry != sizeof(FileEntry) ||
        h.sizeofDirEntry  != sizeof(DirEntry))       return LoadResult::LayoutMismatch;

    // Compare the recorded size against the file's real size. This single
    // check catches truncation, extra trailing bytes, and most corruption -
    // and crucially it happens BEFORE we allocate anything based on the
    // counts, so a garbage count can never turn into a huge allocation.
    if (_fseeki64(f, 0, SEEK_END) != 0) return LoadResult::ReadError;
    const int64_t actualSize = _ftelli64(f);
    if (actualSize < 0) return LoadResult::ReadError;

    if (static_cast<uint64_t>(actualSize) != h.expectedFileSize) {
        return LoadResult::SizeMismatch;
    }

    // Cross-check the counts against each other. sortedByName is built from
    // files, so it must be either empty or exactly as long.
    if (h.sortedCount != 0 && h.sortedCount != h.fileCount) {
        return LoadResult::SizeMismatch;
    }

    if (_fseeki64(f, sizeof(Header), SEEK_SET) != 0) return LoadResult::ReadError;

    // --- read the payload ---------------------------------------------------

    if (!readVector(f, idx.nameBlob,     h.nameBlobCount)) return LoadResult::ReadError;
    if (!readVector(f, idx.files,        h.fileCount))     return LoadResult::ReadError;
    if (!readVector(f, idx.dirs,         h.dirCount))      return LoadResult::ReadError;
    if (!readVector(f, idx.sortedByName, h.sortedCount))   return LoadResult::ReadError;

    // --- checksum -----------------------------------------------------------
    //
    // Recompute over what we just read and compare. This is what catches the
    // right-sized-but-rotten file that every other check above would wave
    // through. It runs before validate() because it is both cheaper and more
    // decisive: if the bytes are wrong, nothing else is worth checking.
    //
    // Note this is computed BEFORE nameBlobFolded is rebuilt, because the
    // folded blob is not part of the file.
    if (computeChecksum(idx) != h.checksum) {
        idx.clear();
        return LoadResult::ChecksumFailed;
    }

    // Rebuild the folded blob rather than reading it. Same characters, then
    // uppercased in place with the very same function used when indexing.
    // Doing this here (after the checksum) also means the folded blob is
    // always derived from bytes we have already verified.
    idx.nameBlobFolded = idx.nameBlob;
    if (!idx.nameBlobFolded.empty()) {
        util::foldCaseInPlace(idx.nameBlobFolded.data(), idx.nameBlobFolded.size());
    }

    // --- final structural check ---------------------------------------------
    //
    // The size check above proves the file has the right SHAPE. This proves
    // the CONTENTS are sane: every name offset lands inside the blob, every
    // parent link points at a real directory, every sorted entry is a real
    // file ID. Without it, a corrupt-but-correctly-sized file would load
    // happily and then fault later inside a paint handler.
    if (!idx.validate()) {
        idx.clear();
        return LoadResult::ValidationFailed;
    }

    return LoadResult::Ok;
}

// -----------------------------------------------------------------------------
const wchar_t* describe(LoadResult r) {
    switch (r) {
        case LoadResult::Ok:               return L"loaded";
        case LoadResult::FileNotFound:     return L"no saved index";
        case LoadResult::BadMagic:         return L"not an index file";
        case LoadResult::VersionMismatch:  return L"saved by a different version";
        case LoadResult::LayoutMismatch:   return L"data layout changed";
        case LoadResult::SizeMismatch:     return L"file is truncated or corrupt";
        case LoadResult::ReadError:        return L"read error";
        case LoadResult::ChecksumFailed:   return L"checksum mismatch, file is damaged";
        case LoadResult::ValidationFailed: return L"contents failed validation";
    }
    return L"unknown";
}

} // namespace persist
