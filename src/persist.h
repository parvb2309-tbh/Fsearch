// =============================================================================
// persist.h - saving and loading the index so startup is instant
// =============================================================================
//
// WHY THIS IS SO SIMPLE, AND WHY THAT IS NOT AN ACCIDENT
// -----------------------------------------------------
// Saving a normal C++ object graph to disk is genuinely hard: pointers are
// memory addresses that mean nothing in the next process, and every
// std::string owns a separate allocation that has to be visited and written
// individually. That is what "serialization libraries" exist to solve.
//
// We sidestep all of it. Look at what the index actually contains:
//
//     std::vector<wchar_t>    - a flat run of characters
//     std::vector<FileEntry>  - fixed-size structs of plain numbers
//     std::vector<DirEntry>   - same
//     std::vector<uint32_t>   - plain numbers
//
// No pointers anywhere. Every "reference" is an integer INDEX, which means
// exactly the same thing when loaded back into a different process at a
// different address. So saving is just writing the bytes out, and loading is
// reading them back.
//
// This is a large part of why the flat-array design in index.h was worth the
// trouble, and it is the payoff for the discipline of using indices instead of
// pointers in the trie too.
//
//
// FOUR THINGS THAT WILL BITE YOU, AND THE GUARDS FOR EACH
// ------------------------------------------------------
//
// 1. TEXT MODE CORRUPTS BINARY DATA - SILENTLY, AND EVERY TIME.
//    Opening a file without "b" puts the C runtime in text mode on Windows,
//    where it rewrites byte 0x0A as 0x0D 0x0A on the way out, and treats
//    0x1A as end-of-file on the way in. Our data is full of both values.
//    The file would be quietly wrong 100% of the time.
//    GUARD: always "wb" / "rb".
//
// 2. A TRUNCATED OR CORRUPT FILE CRASHES YOU SOMEWHERE ELSE ENTIRELY.
//    Read a garbage count of 4 billion and the vector resize either throws
//    bad_alloc or tries to reserve 130 GB. Read plausible counts with garbage
//    offsets and nothing fails until much later, when the GUI asks for row 12
//    and reads memory a mile past the end of the blob - an access violation
//    inside a paint handler, with a stack trace pointing nowhere useful.
//    GUARD: a header carrying every count AND the exact expected file size,
//    checked before allocating, plus FileIndex::validate() afterwards.
//
// 3. A CRASH MID-WRITE LEAVES A FILE THAT LOOKS PERFECTLY VALID.
//    The magic number and version are written first, so they are fine. It is
//    the tail that is missing, and you will not find out until load time.
//    GUARD: write to a temporary file, flush it all the way to disk, then
//    atomically rename over the real one. The real file is only ever replaced
//    by something already known to be complete.
//
// 4. THE STRUCT LAYOUT COULD CHANGE UNDER YOU.
//    Add a field to FileEntry, rebuild, and the old index file is now being
//    read with the wrong stride - every entry after the first is nonsense.
//    GUARD: the header stores both the format version and sizeof() of each
//    struct. Any mismatch discards the file and rescans.
//
// In all failure cases the response is the same: throw the file away and
// rescan. A rescan costs tens of seconds; trusting a bad index costs a crash.
// =============================================================================
#pragma once

#include "index.h"

#include <string>

namespace persist {

// Bump this whenever the on-disk layout changes in any way. Old files are then
// rejected instead of being misread.
constexpr uint32_t FORMAT_VERSION = 1;

enum class LoadResult {
    Ok,
    FileNotFound,     // no saved index yet - normal on first run
    BadMagic,         // not one of our files at all
    VersionMismatch,  // written by a different build
    LayoutMismatch,   // FileEntry / DirEntry changed size
    SizeMismatch,     // file is truncated or has extra bytes
    ReadError,        // disk I/O failed part way through
    ChecksumFailed,   // right size, but the bytes have been altered
    ValidationFailed  // structurally readable, but contents are not sane
};

// Returns the full path of the index file, creating the containing folder if
// needed. Typically:
//     C:\Users\<you>\AppData\Local\FastSearch\index.bin
//
// Returns an empty string if the folder could not be determined or created.
std::wstring defaultIndexPath();

// Writes idx to path. Returns false on any failure, in which case any existing
// file at that path is left untouched.
bool save(const FileIndex& idx, const std::wstring& path);

// Reads path into idx. On anything other than Ok, idx is left cleared and the
// caller should rescan.
//
// NOTE: this does NOT rebuild the trie. The trie is deliberately never saved
// (see trie.h) - the caller rebuilds it, which takes about 100ms.
LoadResult load(FileIndex& idx, const std::wstring& path);

// Human-readable form of a LoadResult, for logging and the status bar.
const wchar_t* describe(LoadResult r);

} // namespace persist
