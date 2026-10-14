// =============================================================================
// search.cpp - query execution and ranking. See search.h.
// =============================================================================
#include "search.h"
#include "util.h"

#include <algorithm>
#include <cwchar>

namespace search {

// -----------------------------------------------------------------------------
// containsFolded - substring test on non-NUL-terminated data
// -----------------------------------------------------------------------------
// The obvious implementation compares the needle at every position:
//
//     for (i = 0; i + nlen <= hlen; ++i)
//         if (wmemcmp(hay + i, needle, nlen) == 0) return true;
//
// That is correct but calls wmemcmp at every single position. Since most
// positions fail on the very first character, we use wmemchr to jump straight
// to the next candidate instead. wmemchr is a runtime function the C library
// implements with vector instructions, so it scans many characters per CPU
// cycle - far faster than a character-at-a-time loop we could write here.
//
// (There are asymptotically better algorithms - Boyer-Moore and friends - but
// they need a preprocessing pass over the needle and only pay off on long
// patterns. Search queries are a handful of characters, so this simple version
// wins in practice.)
bool containsFolded(const wchar_t* hay, size_t hayLen,
                    const wchar_t* needle, size_t needleLen) {
    if (needleLen == 0)      return true;
    if (needleLen > hayLen)  return false;

    const wchar_t  first  = needle[0];
    const wchar_t* p      = hay;
    // Number of positions where the needle could still fit.
    size_t         remain = hayLen - needleLen + 1;

    while (remain > 0) {
        const wchar_t* hit = static_cast<const wchar_t*>(wmemchr(p, first, remain));
        if (hit == nullptr) return false;

        if (std::wmemcmp(hit, needle, needleLen) == 0) return true;

        // Resume one character past the failed candidate.
        const size_t advanced = static_cast<size_t>(hit - p) + 1;
        if (advanced >= remain) return false;
        remain -= advanced;
        p       = hit + 1;
    }
    return false;
}

// -----------------------------------------------------------------------------
// run
// -----------------------------------------------------------------------------
std::vector<SearchResult> run(const FileIndex& idx,
                              const Trie* trie,
                              const std::wstring& rawQuery,
                              const SearchOptions& options,
                              SearchStats* stats) {
    util::Timer timer;

    std::vector<SearchResult> results;
    SearchStats local;

    // Fold the query with the EXACT function used on the stored names. If
    // these two ever disagree, matching silently fails for some characters.
    const std::wstring q = util::foldCase(rawQuery);
    const size_t qlen = q.size();

    if (qlen == 0 || idx.empty() || idx.sortedByName.empty()) {
        if (stats) { local.elapsedMs = timer.elapsedMs(); *stats = local; }
        return results;
    }

    const wchar_t* qp = q.c_str();

    // ---- Step 1: prefix matches -------------------------------------------
    //
    // Both paths must return the same range. The trie is the fast path; the
    // binary search is the authority. --selftest compares them.
    std::pair<size_t, size_t> range;
    if (options.useTrie && trie != nullptr && !trie->empty()) {
        range = trie->findPrefix(idx, qp, qlen);
    } else {
        range = idx.prefixRange(qp, qlen);
    }

    results.reserve(std::min<size_t>(options.maxResults * 2, 4096));

    for (size_t i = range.first; i < range.second; ++i) {
        const uint32_t fileId = idx.sortedByName[i];
        // A name the same length as the query that starts with it IS the
        // query - an exact match, which outranks everything else.
        const MatchKind kind = (idx.nameLen(fileId) == qlen)
                             ? MatchKind::Exact
                             : MatchKind::Prefix;
        results.push_back({ fileId, kind });
    }
    local.prefixMatches = results.size();

    // ---- Step 2: substring matches ----------------------------------------
    //
    // Skipped entirely when we already have enough prefix matches to fill the
    // result list. Every substring match ranks BELOW every prefix match, so
    // any we found would be truncated away - scanning for them would be pure
    // wasted work. This is why typing a common prefix stays instant.
    const bool needSubstring =
        options.useSubstring && results.size() < options.maxResults;

    if (needSubstring) {
        const size_t fileCount = idx.fileCount();
        local.filesScanned = fileCount;

        for (uint32_t fileId = 0; fileId < fileCount; ++fileId) {
            const wchar_t* name = idx.nameFolded(fileId);
            const size_t   nlen = idx.nameLen(fileId);

            // Already counted in step 1 - skip so it does not appear twice.
            if (startsWithFolded(name, nlen, qp, qlen)) continue;

            if (containsFolded(name, nlen, qp, qlen)) {
                results.push_back({ fileId, MatchKind::Substring });
            }
        }
        local.substringMatches = results.size() - local.prefixMatches;
    }

    local.totalMatches = results.size();

    // ---- Step 3: rank ------------------------------------------------------
    //
    // We use partial_sort rather than sort. A broad query like "e" can match
    // hundreds of thousands of files, and fully sorting those would take
    // longer than the scan that found them. partial_sort only puts the best N
    // in order and leaves the rest in arbitrary order, which is exactly what
    // we need since everything past N is discarded anyway.
    auto rank = [&idx](const SearchResult& a, const SearchResult& b) {
        // 1. Match quality: Exact, then Prefix, then Substring.
        if (a.kind != b.kind) return a.kind < b.kind;

        // 2. Shorter names first. If you typed "report", then "report.pdf" is
        //    a better answer than "report-2019-final-archived-copy.pdf":
        //    the shorter the name, the more of it you actually typed.
        const size_t la = idx.nameLen(a.fileId);
        const size_t lb = idx.nameLen(b.fileId);
        if (la != lb) return la < lb;

        // 3. Most recently modified first - recent files are usually the ones
        //    being looked for.
        const uint64_t ta = idx.files[a.fileId].mtime;
        const uint64_t tb = idx.files[b.fileId].mtime;
        if (ta != tb) return ta > tb;

        // 4. Final tiebreaker so the order never changes between identical
        //    searches. Without this, results visibly reshuffle when you
        //    retype the same query.
        return a.fileId < b.fileId;
    };

    if (results.size() > options.maxResults) {
        std::partial_sort(results.begin(),
                          results.begin() + options.maxResults,
                          results.end(),
                          rank);
        results.resize(options.maxResults);
    } else {
        std::sort(results.begin(), results.end(), rank);
    }

    local.elapsedMs = timer.elapsedMs();
    if (stats) *stats = local;
    return results;
}

} // namespace search
