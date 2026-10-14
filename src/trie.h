// =============================================================================
// trie.h - a prefix tree over the sorted filename list
// =============================================================================
//
// WHAT A TRIE IS
// --------------
// A trie (rhymes with "try", from re-TRIE-val) stores strings by their
// characters rather than whole. Every edge is one character, so following a
// path from the root spells out a prefix:
//
//     (root)
//       |
//       +--- r --- e --- p --+--- o --- r --- t   [report.pdf]
//       |                    |
//       |                    +--- a --- i --- r   [repair.c]
//       |
//       +--- m --- a --- i --- n                  [main.cpp]
//
// To find everything starting with "rep" you walk three edges and take
// whatever hangs below. The cost depends only on how much you TYPED, not on
// how many files exist - which is the property that makes it feel instant.
//
//
// OUR VERSION IS UNUSUALLY SMALL, AND HERE IS THE TRICK
// -----------------------------------------------------
// A textbook trie stores a list of matching items at every node. With 600,000
// files that is enormous - the lists at shallow nodes hold nearly everything.
//
// We store no lists at all. Because sortedByName is SORTED, every prefix
// already occupies one contiguous range in it (see FileIndex::prefixRange).
// So a node only needs to remember where that range starts and ends:
//
//   sortedByName:  [...] [rep.txt] [repair.c] [report.pdf] [reset.ini] [...]
//                         ^                                ^
//     node for "rep" = { rangeBegin: 41, rangeEnd: 44 }
//
// Four numbers per node, no lists, no duplication. The trie becomes a pure
// shortcut: it jumps straight to the answer the binary search would have
// computed.
//
//
// AN HONEST NOTE ON WHETHER THIS IS WORTH IT
// ------------------------------------------
// It is not, and it is worth understanding why rather than taking it on faith.
//
// Binary search over a million entries costs about 21 comparisons - already
// well under a microsecond. The trie replaces that with ~5 pointer hops, which
// is faster in theory but unmeasurable in practice, and each hop is a jump to
// a random place in memory (a cache miss) whereas binary search on a small
// array stays largely in cache.
//
// Run the program with --bench to see both timed on your own files.
//
// The structure is kept because it is the classic autocomplete data structure
// and worth knowing. But note two deliberate decisions that came out of
// weighing it up:
//
//   1. It is REBUILT AT LOAD TIME, never saved to disk. A saved trie would
//      hold ranges that silently become wrong the moment sortedByName changes,
//      with nothing to detect the mismatch. Rebuilding takes ~100ms and makes
//      that entire class of bug impossible.
//   2. FileIndex::prefixRange stays the authority on correctness. The trie is
//      checked against it by --selftest.
//
// A B+ tree would be the wrong choice here, for the record. B+ trees exist to
// minimise DISK reads - their high fan-out keeps the tree shallow so a lookup
// touches few pages. Our index lives entirely in RAM, so that advantage buys
// nothing, and the added complexity is real.
// =============================================================================
#pragma once

#include "index.h"

#include <cstdint>
#include <utility>
#include <vector>

class Trie {
public:
    // How many characters deep the tree goes. Past this depth we stop
    // branching and fall back to a binary search within the node's range.
    //
    // Without a cap, a folder full of very long similarly-named files would
    // create a chain of nodes as long as the filenames, and the node count
    // could balloon. 12 characters is plenty: by then a prefix has usually
    // narrowed to a handful of files, and narrowing those few by direct
    // comparison costs nothing.
    static constexpr uint32_t MAX_DEPTH = 12;

    static constexpr uint32_t NO_NODE = 0xFFFFFFFFu;

    // Builds the tree from idx.sortedByName, which must already be built.
    // Any previous contents are discarded.
    void build(const FileIndex& idx);

    void clear();

    // Returns the half-open range of sortedByName whose names start with the
    // folded query, or {0, 0} if nothing matches.
    //
    // idx is needed because a query longer than MAX_DEPTH (or one that reaches
    // a leaf early) is finished off with a binary search inside the range the
    // trie narrowed things to.
    std::pair<size_t, size_t> findPrefix(const FileIndex& idx,
                                         const wchar_t* foldedQuery,
                                         size_t queryLen) const;

    bool   empty()       const { return m_nodes.empty(); }
    size_t nodeCount()   const { return m_nodes.size(); }
    size_t memoryBytes() const;

private:
    // One node. 16 bytes, all plain numbers - no pointers anywhere.
    //
    // Using indices instead of pointers is a deliberate habit worth picking
    // up: the nodes live in one contiguous vector (fast to walk, one
    // allocation), and the whole structure could be written to disk as raw
    // bytes if we ever wanted to, because there is nothing machine-specific
    // inside it.
    struct Node {
        uint32_t rangeBegin;   // first index into sortedByName
        uint32_t rangeEnd;     // one past the last (half-open, like STL)
        uint32_t firstChild;   // start of this node's slice of m_children
        uint32_t childCount;   // how many children, 0 for a leaf
    };

    // A child edge: the character on the edge, and the node it leads to.
    // Children of one node sit next to each other and are sorted by character,
    // so finding the right one is a small binary search.
    struct Child {
        wchar_t  ch;
        uint32_t node;
    };

    std::vector<Node>  m_nodes;      // m_nodes[0] is the root
    std::vector<Child> m_children;

    // Recursively builds the node covering sortedByName[begin, end) at the
    // given depth, and returns its index. Recursion depth is bounded by
    // MAX_DEPTH, so there is no stack-overflow risk.
    uint32_t buildNode(const FileIndex& idx, uint32_t depth,
                       uint32_t begin, uint32_t end);
};
