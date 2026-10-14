// =============================================================================
// scanner.h - walks the filesystem and fills a FileIndex
// =============================================================================
//
// This is the only module that talks to the filesystem. Everything downstream
// (search, GUI, persistence) works purely on the FileIndex it produces, which
// is why a completely different scanner - one that reads the NTFS Master File
// Table directly for a ~3 second full-drive scan - could be dropped in later
// without touching another file.
//
// HOW THE WALK WORKS
// ------------------
// Depth-first, using an explicit stack rather than recursion:
//
//     push (rootDirId, "C:")
//     while stack not empty:
//         pop a directory
//         FindFirstFileExW on  <that path>\*
//         for each entry:
//             ".." or "."  -> skip
//             a directory  -> record it, push it onto the stack
//             a file       -> record it
//
// Recursion would be more natural to read but risks a stack overflow on deeply
// nested trees, and an explicit stack is also what the multithreaded version
// needs (it becomes a shared work queue), so the shape is the same either way.
//
// We use a STACK rather than a queue (depth-first rather than breadth-first)
// for two practical reasons: the pending list stays small (hundreds of entries
// instead of tens of thousands), and finishing one folder before starting the
// next keeps related directory data together in the disk cache.
// =============================================================================
#pragma once

#include "index.h"

#include <atomic>
#include <functional>
#include <string>
#include <vector>

// Counters describing what a scan did. Useful in the status bar, and essential
// for sanity-checking the file count against PowerShell.
struct ScanStats {
    uint64_t filesFound     = 0;
    uint64_t dirsFound      = 0;
    uint64_t accessDenied   = 0;  // folders Windows would not let us read
    uint64_t excluded       = 0;  // folders skipped by the exclusion list
    uint64_t reparseSkipped = 0;  // junctions / symlinks / OneDrive placeholders
    uint64_t otherErrors    = 0;
    double   elapsedSec     = 0.0;
    bool     cancelled      = false;
};

class Scanner {
public:
    Scanner() = default;

    // ---- exclusions ---------------------------------------------------------
    //
    // Two kinds of pattern are supported, distinguished by whether the pattern
    // contains a backslash:
    //
    //   "node_modules"          matches any folder with that NAME, anywhere
    //   "C:\Windows\WinSxS"     matches that one specific full path
    //
    // Matching is case-insensitive (both sides are folded with the same
    // function the index uses).
    void setExclusions(const std::vector<std::wstring>& patterns);

    // Loads patterns from a UTF-8 text file, one per line. Lines starting with
    // '#' and blank lines are ignored. Returns false if the file is missing,
    // in which case a sensible built-in default list is used instead.
    bool loadExclusions(const std::wstring& configPath);

    // Installs the built-in defaults (Windows component store, node_modules,
    // .git, temp folders...).
    void useDefaultExclusions();

    // ---- running a scan -----------------------------------------------------

    // Called periodically during a scan (roughly every 100ms, never per-file -
    // per-file would cost more than the scan itself). Optional.
    std::function<void(const ScanStats&)> onProgress;

    // Walks everything under rootPath (e.g. "C:\") and appends it to index.
    // Returns false only if the root itself could not be opened; individual
    // unreadable subfolders are counted in stats and skipped.
    //
    // NOTE: this does NOT call index.buildSortIndex() - the caller does that
    // once, after all roots have been scanned.
    bool scan(const std::wstring& rootPath, FileIndex& index, ScanStats& stats);

    // ---- cancellation -------------------------------------------------------
    //
    // Safe to call from another thread: that is the whole point. When the user
    // closes the window mid-scan we must stop the worker and join it before
    // tearing anything down, because destroying a running std::thread calls
    // std::terminate() and kills the process outright.
    void requestCancel() { m_cancel.store(true, std::memory_order_relaxed); }
    void resetCancel()   { m_cancel.store(false, std::memory_order_relaxed); }
    bool isCancelled() const { return m_cancel.load(std::memory_order_relaxed); }

private:
    std::atomic<bool> m_cancel{false};

    // Both lists are stored pre-folded (uppercased) so matching is a plain
    // comparison with no per-check conversion.
    std::vector<std::wstring> m_excludeNames;  // bare names: "NODE_MODULES"
    std::vector<std::wstring> m_excludePaths;  // full paths: "C:\WINDOWS\WINSXS"

    // Returns true if a directory should be skipped entirely.
    bool isExcluded(const std::wstring& foldedName,
                    const std::wstring& foldedFullPath) const;
};
