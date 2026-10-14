// =============================================================================
// scanner.cpp - the filesystem walk. See scanner.h for the overall shape.
// =============================================================================
#include "scanner.h"
#include "util.h"

#include <windows.h>
#include <cstdio>
#include <algorithm>

// -----------------------------------------------------------------------------
// FindHandle - RAII wrapper around a FindFirstFileW search handle
// -----------------------------------------------------------------------------
// Every successful FindFirstFileExW must be paired with exactly one FindClose.
// We visit ~300,000 directories, and the loop below has several early-exit
// paths (cancelled, excluded, error). Getting a FindClose onto every one of
// those paths by hand is exactly the kind of thing that goes wrong once and
// then leaks a handle per directory until the process runs out and the scan
// starts failing with ERROR_TOO_MANY_OPEN_FILES - far from the actual mistake.
//
// A destructor cannot be forgotten. This is the one place in this project
// where a tiny class genuinely earns its keep.
namespace {

class FindHandle {
public:
    explicit FindHandle(HANDLE h) : m_h(h) {}

    ~FindHandle() {
        if (m_h != INVALID_HANDLE_VALUE) FindClose(m_h);
    }

    // Non-copyable: two objects closing the same handle would be a double free.
    FindHandle(const FindHandle&)            = delete;
    FindHandle& operator=(const FindHandle&) = delete;

    bool  valid() const { return m_h != INVALID_HANDLE_VALUE; }
    HANDLE get() const  { return m_h; }

private:
    HANDLE m_h;
};

// One directory waiting to be visited.
//
// We carry the full path STRING here rather than just the directory ID, even
// though the ID alone would be enough to rebuild the path from the parent
// chain. Two reasons:
//
//   1. Speed: rebuilding the path per directory means re-walking the chain
//      300,000 times.
//   2. Thread safety, which matters when this becomes a shared work queue.
//      Rebuilding a path READS the dirs and nameBlob vectors, while other
//      workers are APPENDING to them. An append can reallocate and free the
//      old buffer underneath the reader - a use-after-free that shows up as a
//      rare, unreproducible crash. Carrying the string sidesteps the entire
//      problem rather than trying to lock around it.
struct PendingDir {
    uint32_t     dirId;
    std::wstring path;   // full path, WITHOUT the \\?\ prefix
};

// Extracts the last-write time as a single 64-bit value.
//
// Note we do NOT do the tempting *(uint64_t*)&ft. FILETIME is only guaranteed
// 4-byte aligned, so reading it as a uint64_t is undefined behaviour and can
// genuinely fault on stricter architectures. Going through ULARGE_INTEGER is
// the documented, portable way.
inline uint64_t fileTimeToU64(const FILETIME& ft) {
    ULARGE_INTEGER uli;
    uli.LowPart  = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    return uli.QuadPart;
}

inline uint64_t fileSizeToU64(const WIN32_FIND_DATAW& fd) {
    ULARGE_INTEGER uli;
    uli.LowPart  = fd.nFileSizeLow;
    uli.HighPart = fd.nFileSizeHigh;
    return uli.QuadPart;
}

// True for the "." and ".." pseudo-entries every directory contains.
// Following ".." would send the walk straight back up and loop forever.
inline bool isDotEntry(const wchar_t* n) {
    return n[0] == L'.' && (n[1] == L'\0' || (n[1] == L'.' && n[2] == L'\0'));
}

} // anonymous namespace

// -----------------------------------------------------------------------------
// Exclusions
// -----------------------------------------------------------------------------
void Scanner::setExclusions(const std::vector<std::wstring>& patterns) {
    m_excludeNames.clear();
    m_excludePaths.clear();

    for (const std::wstring& raw : patterns) {
        if (raw.empty()) continue;

        std::wstring p = util::foldCase(raw);

        // Normalise forward slashes to backslashes so a config file written
        // with "/" still works.
        std::replace(p.begin(), p.end(), L'/', L'\\');

        // Drop any trailing separator so "C:\Windows\WinSxS\" and
        // "C:\Windows\WinSxS" behave identically.
        while (!p.empty() && p.back() == L'\\') p.pop_back();
        if (p.empty()) continue;

        // A backslash anywhere means the user meant a specific full path;
        // otherwise it is a bare folder name to match anywhere on the drive.
        if (p.find(L'\\') != std::wstring::npos) {
            m_excludePaths.push_back(std::move(p));
        } else {
            m_excludeNames.push_back(std::move(p));
        }
    }
}

void Scanner::useDefaultExclusions() {
    setExclusions({
        // The Windows component store. Enormous, full of hard links to files
        // that already appear elsewhere, and of no interest in a search tool.
        L"C:\\Windows\\WinSxS",
        L"C:\\Windows\\servicing",
        L"C:\\Windows\\Installer",
        L"C:\\$Recycle.Bin",
        L"C:\\System Volume Information",
        // Developer noise: thousands of files nobody searches for by name.
        L"node_modules",
        L".git",
        L".svn",
        L"__pycache__",
        L".gradle",
        // Caches that churn constantly.
        L"AppData\\Local\\Temp",
        L"AppData\\Local\\Microsoft\\Windows\\INetCache",
        L"AppData\\Local\\Packages",
    });
}

bool Scanner::loadExclusions(const std::wstring& configPath) {
    FILE* f = _wfopen(configPath.c_str(), L"rb");
    if (!f) {
        useDefaultExclusions();
        return false;
    }

    std::vector<std::wstring> patterns;
    char line[1024];

    while (fgets(line, sizeof(line), f)) {
        // Strip trailing newline / carriage return.
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0 || line[0] == '#') continue;   // blank or comment

        // The config file is UTF-8; convert to UTF-16 for Windows.
        int wide = MultiByteToWideChar(CP_UTF8, 0, line, -1, nullptr, 0);
        if (wide <= 1) continue;

        std::wstring w(static_cast<size_t>(wide - 1), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, line, -1, &w[0], wide);
        patterns.push_back(std::move(w));
    }
    fclose(f);

    if (patterns.empty()) {
        useDefaultExclusions();
        return false;
    }
    setExclusions(patterns);
    return true;
}

bool Scanner::isExcluded(const std::wstring& foldedName,
                         const std::wstring& foldedFullPath) const {
    for (const std::wstring& n : m_excludeNames) {
        if (foldedName == n) return true;
    }
    for (const std::wstring& p : m_excludePaths) {
        // Match if the path IS the pattern, or ENDS WITH "\pattern".
        // The second case lets "AppData\Local\Temp" match under any user
        // profile without naming each one.
        if (foldedFullPath == p) return true;
        if (foldedFullPath.size() > p.size()) {
            const size_t at = foldedFullPath.size() - p.size();
            if (foldedFullPath[at - 1] == L'\\' &&
                foldedFullPath.compare(at, p.size(), p) == 0) {
                return true;
            }
        }
    }
    return false;
}

// -----------------------------------------------------------------------------
// scan
// -----------------------------------------------------------------------------
bool Scanner::scan(const std::wstring& rootPath, FileIndex& index, ScanStats& stats) {
    util::Timer timer;
    util::Timer progressClock;

    // Normalise the root: "C:\" and "C:" should both give a stored name of
    // "C:", so that dirPath() produces "C:\Users\..." rather than "C:\\Users".
    std::wstring root = rootPath;
    std::replace(root.begin(), root.end(), L'/', L'\\');
    while (root.size() > 1 && root.back() == L'\\') root.pop_back();
    if (root.empty()) return false;

    // Seed the index with the root directory itself.
    const uint32_t rootId =
        index.addDir(root.c_str(), root.size(), FileIndex::ROOT);
    stats.dirsFound++;

    std::vector<PendingDir> stack;
    stack.reserve(4096);
    stack.push_back({ rootId, root });

    WIN32_FIND_DATAW fd;

    while (!stack.empty()) {
        // Check for cancellation once per directory rather than per file:
        // an atomic load is cheap but not free, and per-directory is a fine
        // enough granularity to feel instant to the user.
        if (isCancelled()) {
            stats.cancelled = true;
            break;
        }

        // pop_back + move: taking from the END of a vector is O(1), and moving
        // the string avoids copying the path.
        PendingDir cur = std::move(stack.back());
        stack.pop_back();

        // Build the search pattern: "C:\Users\parvb" + "\" + "*".
        // joinPath() is used rather than plain concatenation because the drive
        // root already ends in a backslash in some spellings, and a doubled
        // separator is fatal once the \\?\ prefix is applied.
        const std::wstring pattern = util::joinPath(cur.path, L"*", 1);

        // The \\?\ prefix lifts the 260-character path limit. It is added ONLY
        // here, at the API call - never stored - because the Windows shell
        // does not understand it.
        const std::wstring searchPath = util::toExtendedPath(pattern);

        // FindExInfoBasic tells Windows not to bother retrieving the old 8.3
        // short name ("PROGRA~1"), which we never use.
        // FIND_FIRST_EX_LARGE_FETCH asks for entries in bigger batches.
        // Together these are roughly 30% faster than plain FindFirstFileW.
        FindHandle h(FindFirstFileExW(searchPath.c_str(),
                                      FindExInfoBasic,
                                      &fd,
                                      FindExSearchNameMatch,
                                      nullptr,
                                      FIND_FIRST_EX_LARGE_FETCH));

        if (!h.valid()) {
            const DWORD err = GetLastError();
            // These two are entirely expected and are not worth reporting:
            //   ACCESS_DENIED  - System Volume Information, other user
            //                    profiles, various protected system folders
            //   PATH_NOT_FOUND - a folder deleted between us listing its
            //                    parent and getting here
            if (err == ERROR_ACCESS_DENIED)       stats.accessDenied++;
            else if (err == ERROR_PATH_NOT_FOUND ||
                     err == ERROR_FILE_NOT_FOUND) { /* vanished; ignore */ }
            else                                   stats.otherErrors++;
            continue;
        }

        do {
            if (isDotEntry(fd.cFileName)) continue;

            const size_t nameLen = wcslen(fd.cFileName);
            if (nameLen == 0) continue;

            const bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

            if (isDir) {
                // REPARSE POINTS: junctions, symlinks, and OneDrive
                // placeholders. Two separate hazards:
                //
                //   1. A junction can point at an ancestor of itself, so
                //      following it loops forever. (C:\Users\All Users is a
                //      junction to C:\ProgramData, for example.)
                //   2. OneDrive "files on demand" folders are reparse points
                //      whose contents are not local. Merely enumerating is
                //      safe, but any code that opened the files would trigger
                //      gigabytes of downloads.
                //
                // Skipping them costs us a few duplicate paths and is by far
                // the safer default.
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                    stats.reparseSkipped++;
                    continue;
                }

                const std::wstring childPath =
                    util::joinPath(cur.path, fd.cFileName, nameLen);

                if (isExcluded(util::foldCase(std::wstring(fd.cFileName, nameLen)),
                               util::foldCase(childPath))) {
                    stats.excluded++;
                    continue;
                }

                const uint32_t childId =
                    index.addDir(fd.cFileName, nameLen, cur.dirId);
                stats.dirsFound++;

                stack.push_back({ childId, childPath });
            } else {
                index.addFile(fd.cFileName, nameLen, cur.dirId,
                              fileSizeToU64(fd),
                              fileTimeToU64(fd.ftLastWriteTime),
                              fd.dwFileAttributes);
                stats.filesFound++;
            }
        } while (FindNextFileW(h.get(), &fd));

        // Report progress on a timer, NOT per file. Posting an update for each
        // of 600,000 files would cost far more than the scan itself, and in the
        // GUI build would flood the window message queue faster than it could
        // be drained.
        if (onProgress && progressClock.elapsedMs() >= 100.0) {
            stats.elapsedSec = timer.elapsedSec();
            onProgress(stats);
            progressClock.reset();
        }
    }

    stats.elapsedSec = timer.elapsedSec();
    if (onProgress) onProgress(stats);
    return true;
}
