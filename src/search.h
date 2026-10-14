// =============================================================================
// search.h - turning what the user typed into a ranked list of files
// =============================================================================
//
// THERE ARE TWO KINDS OF MATCH, AND THEY COST VERY DIFFERENT AMOUNTS
// ------------------------------------------------------------------
//
// PREFIX  - "rep" matches "report.pdf"
//           Answered from the sorted index in ~21 comparisons, or a handful of
//           hops through the trie. Effectively free, however many files exist.
//
// SUBSTRING - "port" also matches "report.pdf"
//           Cannot use a prefix index at all. No amount of sorting by the
//           START of a name helps you find something in the MIDDLE.
//           So we check every file, one after another.
//
// That second one sounds unacceptable and is not, which is worth understanding
// because the instinct to over-engineer here is strong.
//
// All the folded names live in one contiguous block of memory. Scanning it is
// the single friendliest thing you can ask a CPU to do: the prefetcher sees a
// straight line and keeps the pipeline fed. ~12 million characters works out
// around 10-40ms - fast enough that a person cannot perceive it, and we only
// run it once typing pauses (see the debounce in mainwnd.cpp).
//
// Making this "properly" fast would mean an n-gram index: for every 3-letter
// sequence, a list of the files containing it. That is several times the
// memory of the entire rest of the index, a large amount of code, and it
// would save maybe 20 milliseconds nobody would notice. Knowing when NOT to
// build the clever thing is the more useful skill.
//
//
// RANKING
// -------
// Order matters more than people expect. A search tool that returns the right
// file 40 rows down feels broken even though it technically worked. We sort:
//
//   1. Exact match first        ("report" typed, "report" exists)
//   2. Then prefix matches      ("report.pdf", "reports-old.zip")
//   3. Then substring matches   ("q3-report.pdf")
//   4. Within a group, shorter names first - a shorter name means what you
//      typed makes up more of it, which usually means it is what you meant
//   5. Then most recently modified first
// =============================================================================
#pragma once

#include "index.h"
#include "trie.h"

#include <cstdint>
#include <string>
#include <vector>

// Why a file matched. The numeric values ARE the ranking order, so do not
// reorder them casually.
enum class MatchKind : uint8_t {
    Exact     = 0,
    Prefix    = 1,
    Substring = 2
};

struct SearchResult {
    uint32_t  fileId;
    MatchKind kind;
};

struct SearchOptions {
    // How many results to return. The GUI only ever shows a few dozen rows at
    // a time, so there is no point ranking and returning thousands.
    size_t maxResults = 1000;

    // Whether to fall back to substring matching. Turning this off gives
    // prefix-only search, which is what a strict autocomplete would do.
    bool useSubstring = true;

    // Use the trie for the prefix step instead of binary search. Both must
    // produce identical results - that is what --selftest verifies.
    bool useTrie = true;
};

// Filled in when the caller asks for it. Used by the status bar and --bench.
struct SearchStats {
    size_t prefixMatches    = 0;
    size_t substringMatches = 0;
    size_t totalMatches     = 0;   // before truncation to maxResults
    size_t filesScanned     = 0;   // 0 when the substring pass was skipped
    double elapsedMs        = 0.0;
};

namespace search {

// Runs a search. rawQuery is the user's text exactly as typed - it is folded
// internally, so do NOT pre-fold it.
//
// Pass trie = nullptr (or set options.useTrie = false) to use the binary
// search baseline.
//
// Returns at most options.maxResults entries, already ranked.
std::vector<SearchResult> run(const FileIndex& idx,
                              const Trie* trie,
                              const std::wstring& rawQuery,
                              const SearchOptions& options,
                              SearchStats* stats = nullptr);

// True if needle appears anywhere inside hay. Both must already be folded.
//
// Takes explicit lengths because names in the blob are NOT NUL-terminated -
// see the warning at the top of index.h. Using wcsstr here would read past the
// end of the name and into the next one.
bool containsFolded(const wchar_t* hay, size_t hayLen,
                    const wchar_t* needle, size_t needleLen);

// True if hay starts with needle. Both folded, both explicit lengths.
inline bool startsWithFolded(const wchar_t* hay, size_t hayLen,
                             const wchar_t* needle, size_t needleLen) {
    if (needleLen > hayLen) return false;
    return needleLen == 0 || std::wmemcmp(hay, needle, needleLen) == 0;
}

} // namespace search
