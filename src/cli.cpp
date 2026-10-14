// =============================================================================
// cli.cpp - console front end, used to prove the engine works
// =============================================================================
//
// This exists so the scanner, index, trie and search can be developed and
// tested WITHOUT any GUI code in the way. When a search returns the wrong
// thing, you want to be certain the bug is in the search and not in some
// listview message you got subtly wrong.
//
// Build it with:   mingw32-make console
//
// Usage:
//   fsearch-cli.exe                     scan C:\ then search interactively
//   fsearch-cli.exe --root C:\Users     scan just one folder (much faster)
//   fsearch-cli.exe --selftest          check the trie against binary search
//   fsearch-cli.exe --bench             time both prefix strategies
//   fsearch-cli.exe --query report      run one search and exit
//   fsearch-cli.exe --no-substring      prefix matching only
// =============================================================================
#include "index.h"
#include "persist.h"
#include "scanner.h"
#include "search.h"
#include "trie.h"
#include "util.h"

#include <windows.h>
#include <fcntl.h>
#include <io.h>

#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace {

// -----------------------------------------------------------------------------
// Console setup
// -----------------------------------------------------------------------------
// By default the Windows console treats output as 8-bit text, so any filename
// with an accent or non-Latin character prints as mojibake. Changing the
// stream mode fixes that, but WHICH mode depends on where output is going:
//
//   * to a real console window  -> UTF-16 (_O_U16TEXT), which is what the
//                                  Windows console natively speaks
//   * redirected to a file/pipe -> UTF-8 (_O_U8TEXT). Writing raw UTF-16 bytes
//                                  into a pipe produces a file that most other
//                                  tools read as garbage with a NUL between
//                                  every character.
//
// _isatty() tells us which case we are in.
//
// One catch: once a stream is in either mode you must use ONLY the wide
// functions on it (wprintf, fputws). A stray printf triggers a runtime
// assertion. That is why this file uses wprintf throughout.
void setupConsole() {
    const int outMode = _isatty(_fileno(stdout)) ? _O_U16TEXT : _O_U8TEXT;
    _setmode(_fileno(stdout), outMode);
    _setmode(_fileno(stderr), outMode);

    if (_isatty(_fileno(stdin))) {
        _setmode(_fileno(stdin), _O_U16TEXT);
    }
}

// -----------------------------------------------------------------------------
// A NOTE ON %ls, WHICH COST AN HOUR THE FIRST TIME
// -----------------------------------------------------------------------------
// Everywhere below, wide strings are printed with %ls and NEVER %s. The reason
// is a genuine difference between compilers:
//
//   Microsoft's compiler : in wprintf, %s means wchar_t*
//   ISO C (what MinGW    : in wprintf, %s means char*
//   follows here)          and %ls means wchar_t*
//
// So passing a wchar_t* to %s under MinGW makes it read your UTF-16 string as
// if it were bytes. Since ASCII characters in UTF-16 are stored as the letter
// followed by a zero byte, and a zero byte terminates a C string, EVERY string
// prints as just its first character: "C:\Users\parvb" comes out as "C".
//
// It looks like a data corruption bug - as though the scanner truncated
// everything - and you go hunting in completely the wrong place.
//
// %ls means wchar_t* under both compilers, so it is always the safe choice.
// -----------------------------------------------------------------------------

void printIndexSummary(const FileIndex& idx, const Trie& trie,
                       const ScanStats& stats) {
    wprintf(L"\n");
    wprintf(L"  Files indexed    : %ls\n", util::formatCount(stats.filesFound).c_str());
    wprintf(L"  Directories      : %ls\n", util::formatCount(stats.dirsFound).c_str());
    wprintf(L"  Access denied    : %ls\n", util::formatCount(stats.accessDenied).c_str());
    wprintf(L"  Excluded folders : %ls\n", util::formatCount(stats.excluded).c_str());
    wprintf(L"  Reparse skipped  : %ls\n", util::formatCount(stats.reparseSkipped).c_str());
    wprintf(L"  Scan time        : %.2f s\n", stats.elapsedSec);
    wprintf(L"\n");
    wprintf(L"  Index memory     : %ls\n", util::formatSize(idx.memoryBytes()).c_str());
    wprintf(L"  Trie nodes       : %ls  (%ls)\n",
            util::formatCount(trie.nodeCount()).c_str(),
            util::formatSize(trie.memoryBytes()).c_str());
    wprintf(L"\n");
}

void printResults(const FileIndex& idx,
                  const std::vector<SearchResult>& results,
                  const SearchStats& stats,
                  size_t showMax) {
    if (results.empty()) {
        wprintf(L"  (no matches)   %.2f ms\n\n", stats.elapsedMs);
        return;
    }

    const size_t show = (results.size() < showMax) ? results.size() : showMax;
    for (size_t i = 0; i < show; ++i) {
        const SearchResult& r = results[i];

        const wchar_t* tag = L"sub ";
        if (r.kind == MatchKind::Exact)       tag = L"EXACT";
        else if (r.kind == MatchKind::Prefix) tag = L"pre ";

        const std::wstring name = idx.nameStr(r.fileId);
        const std::wstring dir  = idx.dirPath(idx.files[r.fileId].parentDir);

        wprintf(L"  %ls  %-40.40ls  %10ls  %ls\n",
                tag,
                name.c_str(),
                util::formatSize(idx.files[r.fileId].size).c_str(),
                dir.c_str());
    }

    if (results.size() > show) {
        wprintf(L"  ... and %ls more\n",
                util::formatCount(results.size() - show).c_str());
    }

    wprintf(L"\n  %ls matches (%ls prefix, %ls substring)  in %.2f ms\n\n",
            util::formatCount(stats.totalMatches).c_str(),
            util::formatCount(stats.prefixMatches).c_str(),
            util::formatCount(stats.substringMatches).c_str(),
            stats.elapsedMs);
}

// -----------------------------------------------------------------------------
// selftest - the trie must agree with the binary search, always
// -----------------------------------------------------------------------------
// This is the single most valuable test in the project. The trie is an
// optimisation layered on top of a simple, obviously-correct binary search, so
// the way to trust it is to check it never disagrees.
//
// Rather than inventing queries, we draw prefixes from names that are actually
// in the index. Random letter combinations would almost all match nothing and
// would exercise only the "not found" path.
bool runSelfTest(const FileIndex& idx, const Trie& trie, int iterations) {
    if (idx.empty()) {
        wprintf(L"  selftest: index is empty, nothing to check\n");
        return true;
    }

    // Fixed seed so a failure can be reproduced exactly.
    std::mt19937 rng(12345);
    std::uniform_int_distribution<size_t> pickFile(0, idx.fileCount() - 1);

    int failures = 0;

    for (int i = 0; i < iterations; ++i) {
        const uint32_t fileId = static_cast<uint32_t>(pickFile(rng));
        const size_t   nlen   = idx.nameLen(fileId);
        if (nlen == 0) continue;

        // Take a prefix of random length, deliberately sometimes longer than
        // the trie's depth cap so the comparison-fallback path gets tested.
        std::uniform_int_distribution<size_t> pickLen(1, nlen);
        const size_t qlen = pickLen(rng);

        const std::wstring q(idx.nameFolded(fileId), qlen);

        const auto viaBinary = idx.prefixRange(q.c_str(), qlen);
        const auto viaTrie   = trie.findPrefix(idx, q.c_str(), qlen);

        if (viaBinary != viaTrie) {
            if (failures < 10) {
                wprintf(L"  MISMATCH on \"%ls\": binary=[%zu,%zu) trie=[%zu,%zu)\n",
                        q.c_str(),
                        viaBinary.first, viaBinary.second,
                        viaTrie.first, viaTrie.second);
            }
            ++failures;
        }
    }

    // Also check a query that certainly matches nothing.
    const std::wstring missing = L"ZZZQQQNOTHINGMATCHESTHIS";
    if (idx.prefixRange(missing.c_str(), missing.size()) !=
        trie.findPrefix(idx, missing.c_str(), missing.size())) {
        wprintf(L"  MISMATCH on the deliberately-absent query\n");
        ++failures;
    }

    if (failures == 0) {
        wprintf(L"  selftest PASSED - trie and binary search agree on %d queries\n\n",
                iterations);
        return true;
    }
    wprintf(L"  selftest FAILED - %d mismatches\n\n", failures);
    return false;
}

// -----------------------------------------------------------------------------
// bench - measure rather than assume
// -----------------------------------------------------------------------------
void runBench(const FileIndex& idx, const Trie& trie) {
    if (idx.empty()) return;

    // Build a realistic set of queries: prefixes taken from real filenames.
    std::mt19937 rng(999);
    std::uniform_int_distribution<size_t> pickFile(0, idx.fileCount() - 1);

    std::vector<std::wstring> queries;
    queries.reserve(2000);
    for (int i = 0; i < 2000; ++i) {
        const uint32_t fileId = static_cast<uint32_t>(pickFile(rng));
        const size_t nlen = idx.nameLen(fileId);
        if (nlen < 3) continue;
        const size_t qlen = 3 + (static_cast<size_t>(rng()) % (nlen - 2));
        queries.emplace_back(idx.nameFolded(fileId), qlen);
    }

    wprintf(L"  Benchmarking %ls prefix lookups...\n\n",
            util::formatCount(queries.size()).c_str());

    // --- binary search ---
    util::Timer t1;
    size_t sink = 0;   // accumulate something so the optimiser cannot delete
                       // the loop entirely - a classic benchmarking mistake
    for (const std::wstring& q : queries) {
        const auto r = idx.prefixRange(q.c_str(), q.size());
        sink += r.second - r.first;
    }
    const double binMs = t1.elapsedMs();

    // --- trie ---
    util::Timer t2;
    for (const std::wstring& q : queries) {
        const auto r = trie.findPrefix(idx, q.c_str(), q.size());
        sink += r.second - r.first;
    }
    const double trieMs = t2.elapsedMs();

    wprintf(L"  Binary search : %8.3f ms total   %7.3f us per lookup\n",
            binMs, binMs * 1000.0 / static_cast<double>(queries.size()));
    wprintf(L"  Trie          : %8.3f ms total   %7.3f us per lookup\n",
            trieMs, trieMs * 1000.0 / static_cast<double>(queries.size()));
    wprintf(L"  Trie memory   : %ls for %ls nodes\n",
            util::formatSize(trie.memoryBytes()).c_str(),
            util::formatCount(trie.nodeCount()).c_str());
    wprintf(L"  (checksum %zu - ignore, it just stops the compiler optimising\n"
            L"   the measured loops away)\n\n", sink);

    // --- a full substring search, which is the real hot path ---
    SearchOptions opt;
    SearchStats st;
    const wchar_t* subQueries[] = { L"report", L"config", L"a", L"test" };
    wprintf(L"  Full searches (prefix + substring scan):\n");
    for (const wchar_t* sq : subQueries) {
        search::run(idx, &trie, sq, opt, &st);
        wprintf(L"    %-10ls  %8.2f ms   %ls matches\n",
                sq, st.elapsedMs, util::formatCount(st.totalMatches).c_str());
    }
    wprintf(L"\n");
}

} // anonymous namespace

// -----------------------------------------------------------------------------
// wmain
// -----------------------------------------------------------------------------
// wmain rather than main: the -municode flag makes this the entry point, and
// it hands us arguments already in UTF-16 so paths with non-ASCII characters
// survive intact.
int wmain(int argc, wchar_t** argv) {
    setupConsole();

    std::wstring root      = L"C:\\";
    std::wstring oneQuery;
    bool selfTest     = false;
    bool bench        = false;
    bool useSubstring = true;
    bool useTrie      = true;
    bool noCache      = false;

    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--root" && i + 1 < argc)       root = argv[++i];
        else if (a == L"--query" && i + 1 < argc) oneQuery = argv[++i];
        else if (a == L"--selftest")              selfTest = true;
        else if (a == L"--bench")                 bench = true;
        else if (a == L"--no-substring")          useSubstring = false;
        else if (a == L"--no-trie")               useTrie = false;
        else if (a == L"--rescan")                noCache = true;
        else {
            wprintf(L"Unknown argument: %ls\n", a.c_str());
            return 1;
        }
    }

    wprintf(L"\nFastSearch - console build\n");

    FileIndex idx;
    ScanStats stats;
    double sortMs = 0.0;

    // The saved index does not record WHICH folder it came from, so it is only
    // reused for a default whole-drive run. Asking for a specific --root
    // always rescans - otherwise you would silently get results for the whole
    // drive instead of the folder you asked about.
    const bool cacheUsable = !noCache && root == L"C:\\";
    const std::wstring indexPath = persist::defaultIndexPath();

    bool loaded = false;
    if (cacheUsable && !indexPath.empty()) {
        util::Timer loadTimer;
        const persist::LoadResult r = persist::load(idx, indexPath);
        if (r == persist::LoadResult::Ok) {
            loaded = true;
            stats.filesFound = idx.fileCount();
            stats.dirsFound  = idx.dirCount();
            wprintf(L"Loaded saved index: %ls files in %.0f ms\n",
                    util::formatCount(idx.fileCount()).c_str(),
                    loadTimer.elapsedMs());
        } else {
            wprintf(L"No usable saved index (%ls) - scanning.\n",
                    persist::describe(r));
        }
    }

    if (!loaded) {
        wprintf(L"Scanning %ls ...\n", root.c_str());

        // Pre-allocating for a typical drive avoids ~20 reallocations of an
        // ever-growing 100 MB buffer during the scan.
        idx.reserve(700000, 80000);

        Scanner scanner;
        // Resolved next to the .exe rather than the current directory.
        // Falls back to the built-in defaults if absent.
        scanner.loadExclusions(
            util::joinPath(util::exeDirectory(), L"config.txt"));

        // Progress is reported on a timer inside the scanner, not per file.
        scanner.onProgress = [](const ScanStats& s) {
            wprintf(L"\r  %ls files, %ls dirs  (%.1fs)          ",
                    util::formatCount(s.filesFound).c_str(),
                    util::formatCount(s.dirsFound).c_str(),
                    s.elapsedSec);
            fflush(stdout);
        };

        if (!scanner.scan(root, idx, stats)) {
            wprintf(L"\nERROR: could not scan %ls\n", root.c_str());
            return 1;
        }
        wprintf(L"\r                                                        \r");

        // Sorting must happen before any search or trie build.
        util::Timer sortTimer;
        idx.buildSortIndex();
        sortMs = sortTimer.elapsedMs();

        if (cacheUsable && !indexPath.empty()) {
            util::Timer saveTimer;
            if (persist::save(idx, indexPath)) {
                wprintf(L"Saved index to %ls (%.0f ms)\n",
                        indexPath.c_str(), saveTimer.elapsedMs());
            } else {
                wprintf(L"WARNING: could not save index to %ls\n", indexPath.c_str());
            }
        }
    }

    util::Timer trieTimer;
    Trie trie;
    trie.build(idx);
    const double trieMs = trieTimer.elapsedMs();

    printIndexSummary(idx, trie, stats);
    wprintf(L"  Sort index built : %.0f ms\n", sortMs);
    wprintf(L"  Trie built       : %.0f ms\n\n", trieMs);

    if (!idx.validate()) {
        wprintf(L"  WARNING: index failed validation\n\n");
    }

    if (selfTest) {
        return runSelfTest(idx, trie, 5000) ? 0 : 1;
    }

    if (bench) {
        runBench(idx, trie);
        return 0;
    }

    SearchOptions opt;
    opt.useSubstring = useSubstring;
    opt.useTrie      = useTrie;

    if (!oneQuery.empty()) {
        SearchStats st;
        const auto results = search::run(idx, &trie, oneQuery, opt, &st);
        printResults(idx, results, st, 25);
        return 0;
    }

    // Interactive loop.
    wprintf(L"Type a search (blank line or 'quit' to exit).\n\n");
    wchar_t line[512];
    for (;;) {
        wprintf(L"search> ");
        fflush(stdout);

        if (!fgetws(line, 512, stdin)) break;

        // Strip the trailing newline.
        size_t len = wcslen(line);
        while (len > 0 && (line[len - 1] == L'\n' || line[len - 1] == L'\r')) {
            line[--len] = L'\0';
        }
        if (len == 0) break;
        if (wcscmp(line, L"quit") == 0 || wcscmp(line, L"exit") == 0) break;

        SearchStats st;
        const auto results = search::run(idx, &trie, line, opt, &st);
        printResults(idx, results, st, 20);
    }

    return 0;
}
