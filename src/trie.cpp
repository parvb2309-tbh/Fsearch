// =============================================================================
// trie.cpp - building and querying the prefix tree. See trie.h.
// =============================================================================
#include "trie.h"

#include <algorithm>

// -----------------------------------------------------------------------------
// build
// -----------------------------------------------------------------------------
void Trie::build(const FileIndex& idx) {
    clear();
    if (idx.sortedByName.empty()) return;

    // Rough guesses to avoid repeated reallocation while building. Being
    // wrong just costs one more reallocation, so no need to be precise.
    m_nodes.reserve(idx.sortedByName.size() / 2 + 16);
    m_children.reserve(idx.sortedByName.size() / 2 + 16);

    buildNode(idx, 0, 0, static_cast<uint32_t>(idx.sortedByName.size()));
}

void Trie::clear() {
    std::vector<Node>().swap(m_nodes);
    std::vector<Child>().swap(m_children);
}

// -----------------------------------------------------------------------------
// buildNode
// -----------------------------------------------------------------------------
// Covers sortedByName[begin, end) - a set of names already known to share the
// first `depth` characters. Groups them by their NEXT character and recurses
// once per distinct character.
//
// This works in a single pass because the list is sorted: names sharing a
// character at position `depth` are necessarily adjacent, so we just walk
// forward and cut whenever that character changes.
uint32_t Trie::buildNode(const FileIndex& idx, uint32_t depth,
                         uint32_t begin, uint32_t end) {
    const uint32_t nodeIdx = static_cast<uint32_t>(m_nodes.size());

    Node n{};
    n.rangeBegin = begin;
    n.rangeEnd   = end;
    n.firstChild = 0;
    n.childCount = 0;
    m_nodes.push_back(n);

    // Stop descending when:
    //   * we have hit the depth cap, or
    //   * there is at most one name left - splitting one name into a chain of
    //     single-child nodes would be pure overhead, and the query path
    //     already falls back to comparison for anything deeper.
    if (depth >= MAX_DEPTH || (end - begin) <= 1) return nodeIdx;

    // Names too SHORT to have a character at this depth belong to this node
    // but produce no child edge. Because shorter names sort first (see
    // compareNames), they are all bunched at the start of the range, so a
    // single skip loop handles them.
    uint32_t i = begin;
    while (i < end && idx.nameLen(idx.sortedByName[i]) <= depth) ++i;

    // Collect the child groups first, without recursing. We must not recurse
    // yet: recursion appends to m_children, and we need our own children to
    // occupy one contiguous block.
    struct Group { wchar_t ch; uint32_t b, e; };
    std::vector<Group> groups;

    while (i < end) {
        const uint32_t fid = idx.sortedByName[i];
        const wchar_t  ch  = idx.nameFolded(fid)[depth];
        const uint32_t gStart = i;

        // Advance while the character at this depth stays the same.
        while (i < end) {
            const uint32_t f = idx.sortedByName[i];
            if (idx.nameLen(f) <= depth) break;
            if (idx.nameFolded(f)[depth] != ch) break;
            ++i;
        }
        groups.push_back({ ch, gStart, i });
    }

    if (groups.empty()) return nodeIdx;

    // Reserve our contiguous slice NOW, before any recursion. Recursive calls
    // will append their own children after ours, which is fine - our slice is
    // already claimed and will not move within the vector's index space.
    const uint32_t firstChild = static_cast<uint32_t>(m_children.size());
    m_children.resize(firstChild + groups.size());

    // Note we index m_nodes rather than holding a reference to it. The
    // recursion below pushes new nodes, which can REALLOCATE the vector and
    // invalidate any reference or pointer we were holding - a classic and
    // very confusing bug. Indexing re-reads the current buffer each time.
    m_nodes[nodeIdx].firstChild = firstChild;
    m_nodes[nodeIdx].childCount = static_cast<uint32_t>(groups.size());

    for (size_t k = 0; k < groups.size(); ++k) {
        const uint32_t childNode =
            buildNode(idx, depth + 1, groups[k].b, groups[k].e);

        // Written after the recursive call returns, for the same
        // reallocation reason: m_children may have grown meanwhile, so we
        // index it fresh rather than caching a pointer into it.
        m_children[firstChild + k].ch   = groups[k].ch;
        m_children[firstChild + k].node = childNode;
    }

    // Groups came from a sorted list, so they are already in character order -
    // which is what lets findPrefix binary-search them.
    return nodeIdx;
}

// -----------------------------------------------------------------------------
// findPrefix
// -----------------------------------------------------------------------------
std::pair<size_t, size_t> Trie::findPrefix(const FileIndex& idx,
                                           const wchar_t* q,
                                           size_t qlen) const {
    if (m_nodes.empty() || qlen == 0) return { 0, 0 };

    uint32_t node  = 0;   // start at the root
    size_t   depth = 0;

    // Walk down one character at a time.
    while (depth < qlen && depth < MAX_DEPTH) {
        const Node& n = m_nodes[node];
        if (n.childCount == 0) break;     // leaf - finish by comparison below

        // Binary search this node's children for the character we want.
        // Children are stored contiguously and in sorted character order.
        uint32_t lo = n.firstChild;
        uint32_t hi = n.firstChild + n.childCount;
        uint32_t found = NO_NODE;

        while (lo < hi) {
            const uint32_t mid = lo + (hi - lo) / 2;
            const wchar_t  c   = m_children[mid].ch;
            if (c == q[depth])      { found = m_children[mid].node; break; }
            else if (c < q[depth])  { lo = mid + 1; }
            else                    { hi = mid; }
        }

        // No edge for this character means nothing in the index starts with
        // the query. This is the common case while typing a word that does
        // not exist, and it exits immediately.
        if (found == NO_NODE) return { 0, 0 };

        node = found;
        ++depth;
    }

    const Node& n = m_nodes[node];

    // If the tree consumed the entire query, its range is already exact.
    if (depth == qlen) {
        return { n.rangeBegin, n.rangeEnd };
    }

    // Otherwise we stopped early - either at the depth cap or at a leaf - and
    // the node's range is a superset of what we want. Narrow it with the same
    // binary search the baseline uses. The range is tiny by now, so this is
    // cheap, and reusing prefixRange means there is only one piece of code
    // that has to be correct.
    return idx.prefixRange(n.rangeBegin, n.rangeEnd, q, qlen);
}

// -----------------------------------------------------------------------------
size_t Trie::memoryBytes() const {
    return m_nodes.capacity()    * sizeof(Node)
         + m_children.capacity() * sizeof(Child);
}
