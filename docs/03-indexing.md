# 3. Indexing — sorted arrays, tries, and what the numbers actually say

## What "index" means here

An index is a structure that lets you skip work. Without one, finding every
file starting with `rep` means checking all 433,198 names. With one, you jump
more or less straight to the answer.

This project has two, and they answer different questions:

| Question | Structure | Cost |
|---|---|---|
| Names **starting with** `rep` | Sorted array (+ trie) | 0.27 µs |
| Names **containing** `port` | Nothing — linear scan | ~20 ms |

That second row is not an oversight. It is explained at the end, and the
reasoning matters more than the code.

## The sorted array

`sortedByName` holds every file ID, ordered by folded (uppercased) name:

```
index:   ...  38   39    40      41        42        43         44    ...
name:    ... RELAY REMOVE REP.TXT REPAIR.C REPORT.PDF REPORT2.PDF RESET.INI ...
                              |______________________________|
                                everything starting with "REP"
```

Because it is sorted, **every prefix occupies one contiguous range.** There is
no way for a name starting with `REP` to end up separated from the others —
sorting put them together by definition.

So "find everything starting with REP" becomes "find the two edges of that
block", and each edge is a binary search: ~21 comparisons for 433,198 entries.

```cpp
auto range = idx.prefixRange(L"REP", 3);    // returns { 40, 44 }
for (size_t i = range.first; i < range.second; ++i) {
    uint32_t fileId = idx.sortedByName[i];
    ...
}
```

This costs **4 bytes per file** on top of the index, and it is the authority on
correctness for the whole program.

### The one thing that would break this

Contiguity holds for plain character-by-character ordering. It does **not** hold
for *linguistic collation* — the locale-aware comparison used by
`CompareStringW`, `lstrcmpiW` and `wcscoll`.

Those treat punctuation as a tiebreaker rather than a character, so:

```
   plain ordering              linguistic ordering
   --------------              -------------------
   co-op                       cool
   cool                        co-op        <-- "co-" is no longer
   coop                        coop              contiguous
```

Sorting with one of those functions would silently break every prefix search,
and only for names containing hyphens or apostrophes — so most of your tests
would pass. `index.cpp` sorts with `std::wmemcmp` on the pre-folded names for
exactly this reason.

## The trie

A trie (from re**trie**val, usually pronounced "try") stores strings by their
characters. Each edge is one character, so a path from the root spells a prefix:

```
    (root)
      |
      +--- R --- E --- P --+--- O --- R --- T      [report.pdf, report2.pdf]
      |                    |
      |                    +--- A --- I --- R      [repair.c]
      |
      +--- M --- A --- I --- N                     [main.cpp]
```

Typing `REP` means walking three edges and taking everything below. The cost
depends on **how much you typed**, not on how many files exist.

### Our version stores almost nothing

A textbook trie keeps a list of matching items at every node. With 433,198 files
that is enormous — the list at the root alone would hold everything.

We store no lists at all. Since the array is sorted, a prefix already
corresponds to a contiguous range, so a node only has to remember where that
range starts and ends:

```cpp
struct TrieNode {
    uint32_t rangeBegin, rangeEnd;    // a slice of sortedByName
    uint32_t firstChild, childCount;  // a slice of the children array
};
```

Sixteen bytes per node, no lists, nothing duplicated. The trie becomes a pure
shortcut: it jumps directly to the answer the binary search would have
computed.

Depth is capped at 12 characters. Beyond that it stops branching and hands off
to a binary search within whatever range it reached — by then a prefix has
usually narrowed to a handful of files, so narrowing those by direct comparison
costs nothing. The cap also stops a folder full of long, similarly-named files
from creating enormous chains of single-child nodes.

## The measurement

`fsearch-cli.exe --bench` times both paths on 1,993 prefixes drawn from real
filenames:

```
  Binary search :    1.187 ms total     0.595 us per lookup
  Trie          :    0.544 ms total     0.273 us per lookup
  Trie memory   : 20 MB for 496,494 nodes
```

**The trie is about 2.2× faster.** It also costs 20 MB and 110 ms to build.

## The honest verdict

The trie is not worth it, and understanding why is more useful than the trie
itself.

Binary search takes **0.595 microseconds**. A person perceives delay at around
100,000 microseconds. The trie saves 0.32 µs per keystroke — roughly
0.0003% of the threshold where anyone could possibly notice.

Meanwhile a full search including the substring pass takes **20,000 µs**. The
trie optimises something that accounts for 0.003% of the total.

The theory says the trie should win more decisively: O(length of prefix) versus
O(log n). In practice each trie hop is a jump to an unrelated place in memory
(a cache miss), while a binary search over a compact array mostly stays in
cache. Asymptotic advantage, hardware disadvantage — they roughly cancel.

**It is kept anyway**, for two reasons: it is the classic autocomplete structure
and worth knowing, and having two independent implementations of the same
question is a genuinely valuable correctness check. `--selftest` runs 5,000 real
prefixes through both and requires identical answers:

```
selftest PASSED - trie and binary search agree on 5000 queries
```

Two decisions came out of weighing it honestly:

**It is rebuilt at load time and never saved to disk.** A saved trie holds
ranges that become silently wrong the moment `sortedByName` changes, with
nothing to detect the mismatch. Rebuilding costs 110 ms and makes that entire
class of bug impossible.

**The binary search stays the authority.** The trie is the optimisation; the
simple thing is the reference.

## Why not a B+ tree?

B+ trees are what databases use, so they seem like the obvious answer for
"indexing". They are the wrong tool here, and the reason is worth knowing.

A B+ tree exists to minimise **disk reads**. Its nodes are sized to a disk page
and hold hundreds of keys each, so the tree stays very shallow and a lookup
touches only three or four pages. When a page costs 10 ms to fetch, reducing
twenty page reads to four is a 5× win.

Our index is entirely in RAM. There are no page reads to minimise. The
advantage a B+ tree is built to deliver simply does not apply — you would carry
all the complexity of node splitting and rebalancing and get nothing back.

B+ trees are excellent. They solve a problem this program does not have.

## Substring search: the one that scans everything

Typing `port` should match `report.pdf`. No prefix structure can help: sorting
by the *start* of a name tells you nothing about its *middle*.

So it checks every file:

```cpp
for (uint32_t fileId = 0; fileId < fileCount; ++fileId) {
    if (containsFolded(idx.nameFolded(fileId), idx.nameLen(fileId), q, qlen))
        results.push_back({ fileId, MatchKind::Substring });
}
```

This is the naive approach, and it is the right one. About 14 million characters
in one contiguous block, scanned in **15–40 ms**.

It is fast because of the data model. The names are a straight run of memory, so
the CPU's prefetcher keeps the pipeline fed. The same loop over 433,198
separately-allocated `std::wstring`s would be perhaps 10× slower.

Two refinements:

**`wmemchr` finds candidates.** Rather than comparing the needle at every
position, jump to the next occurrence of its first character. `wmemchr` is
implemented with vector instructions and scans many characters per cycle.

**The scan is skipped when it cannot matter.** Every substring match ranks below
every prefix match, so if the prefix step already produced enough results to
fill the list, nothing the scan found would survive ranking:

```cpp
const bool needSubstring =
    options.useSubstring && results.size() < options.maxResults;
```

That is why typing a common prefix stays instantaneous while a rare one takes
20 ms.

### What we are not building

The "proper" answer is an n-gram index: for every three-character sequence,
store the list of files containing it. It would cut substring search to
microseconds — and cost several times the memory of everything else combined,
plus a large amount of code.

It would save 20 milliseconds that nobody can perceive.

Knowing when *not* to build the clever thing is the more useful skill, and this
is a clear case.

## Ranking

Returning the right file 40 rows down feels broken even though it technically
worked. Results are ordered:

1. **Exact match** — you typed `report` and a file is called exactly that
2. **Prefix match** — `report.pdf`
3. **Substring match** — `q3-report.pdf`
4. Within a group, **shorter names first** — the shorter the name, the more of
   it you typed, so the more likely it is what you meant
5. Then **most recently modified**
6. Then by file ID, purely so identical searches never reshuffle

Sorting uses `std::partial_sort`, not `std::sort`. A query like `e` matches
hundreds of thousands of files, and fully ordering those would take longer than
the scan that found them. `partial_sort` orders only the first 1,000 and leaves
the rest arbitrary — which is exactly right, since everything past 1,000 is
discarded.
