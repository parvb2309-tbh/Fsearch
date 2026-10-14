// =============================================================================
// util.h - small helpers used by every other module
// =============================================================================
//
// Nothing here is clever. It exists so that the tricky-but-boring Windows
// details (case folding, long paths, high-resolution timing) live in ONE place
// with an explanation attached, instead of being re-derived wrongly in five
// different files.
// =============================================================================
#pragma once

#include <windows.h>
#include <string>
#include <cstdint>

namespace util {

// -----------------------------------------------------------------------------
// CASE FOLDING
// -----------------------------------------------------------------------------
// "Case folding" means converting text to one consistent case so that
// comparisons ignore capitalisation. We fold everything to UPPERCASE once when
// building the index, and fold the user's query the same way, so matching is a
// plain character-for-character comparison afterwards - no per-character case
// logic in the hot loop.
//
// WHY NOT towlower()?
//   towlower() from <cwctype> respects the current C locale, which is "C" at
//   program start. In the "C" locale it only handles ASCII A-Z; every
//   non-ASCII character passes through UNCHANGED. So a file named "Resume.pdf"
//   would match fine, but the same name written with accents would silently
//   never match. The bug only appears on files with accented names - exactly
//   the kind of thing absent from your test folder and present in your real
//   Documents folder.
//
// WHY CharUpperBuffW() SPECIFICALLY?
//   1. It folds the way Windows itself does, so it agrees with how the
//      filesystem treats case.
//   2. Critically, it is a 1-to-1 mapping: N code units in, N code units out.
//      Our whole design depends on nameBlob and nameBlobFolded having
//      IDENTICAL offsets for every name. A folding function that changed
//      length would desync the two, and every file would start displaying some
//      other file's name.
//      (LCMapStringEx with LCMAP_LINGUISTIC_CASING does change length - the
//      German sharp-s folds to "SS" - which is why we do not use it.)
//
// Folds buf in place. Always apply this to BOTH the stored names and the
// query, or matching breaks.
void foldCaseInPlace(wchar_t* buf, size_t len);

// Convenience wrapper: returns a folded copy.
std::wstring foldCase(const std::wstring& s);

// -----------------------------------------------------------------------------
// PATH HELPERS
// -----------------------------------------------------------------------------
// Windows has a legacy limit of 260 characters (MAX_PATH) for a path. Prefixing
// a path with the four characters backslash-backslash-question-backslash lifts
// that to ~32767. But it also tells Windows "pass this through to the
// filesystem almost raw", which disables ALL normalisation:
//
//   * must be fully qualified   - no relative paths, no drive-relative "C:foo"
//   * backslashes only          - forward slashes are normally translated for
//                                 you, but NOT under this prefix
//   * no "." or ".." components - they become literal folder names
//   * no doubled separators     - and this is the one that bites
//
// That last rule is the trap. A drive root is spelled "C:" plus a backslash,
// so it already ends in a separator. Naively building a search pattern as
// path + separator + "*" produces a doubled separator and the call fails with
// a confusing error. joinPath() below exists specifically to get this right
// every single time.
//
// IMPORTANT: we store paths WITHOUT the prefix and add it only at the moment
// of calling a filesystem API. The Windows shell does NOT understand the
// prefix: ShellExecuteW fails outright on a prefixed path, and passing one to
// explorer /select, silently opens the wrong folder with no error at all.

// Adds the long-path prefix. Handles UNC paths (which start with two
// backslashes) correctly: those need the different form with UNC inserted,
// because blindly prepending would produce an invalid triple separator.
std::wstring toExtendedPath(const std::wstring& path);

// Removes the long-path prefix, giving a path safe to hand to the shell.
std::wstring fromExtendedPath(const std::wstring& path);

// Joins a directory path and a child name with exactly one separator between
// them, no matter whether dir already ends in one.
std::wstring joinPath(const std::wstring& dir, const wchar_t* name, size_t nameLen);
std::wstring joinPath(const std::wstring& dir, const std::wstring& name);

// The folder containing the running .exe.
//
// Needed because the "current directory" is NOT where the program lives - it is
// wherever the user happened to be when they launched it, and for a shortcut or
// a Start-menu entry it is something else again. Opening "config.txt" by its
// bare name therefore works when you run the program from its own folder during
// development, and silently fails to find it everywhere else.
std::wstring exeDirectory();

// -----------------------------------------------------------------------------
// TIMING
// -----------------------------------------------------------------------------
// QueryPerformanceCounter is the high-resolution timer on Windows (sub
// microsecond). GetTickCount only has ~15ms resolution, which would report our
// sub-millisecond searches as a flat "0 ms".
class Timer {
public:
    Timer();
    void   reset();
    double elapsedMs() const;   // milliseconds since construction or reset()
    double elapsedSec() const;
private:
    int64_t m_start;
    static int64_t s_frequency;
};

// -----------------------------------------------------------------------------
// FORMATTING (for display only)
// -----------------------------------------------------------------------------
std::wstring formatSize(uint64_t bytes);          // 1536    -> "1.5 KB"
std::wstring formatFileTime(uint64_t filetime);   //         -> "2026-09-19 14:32"
std::wstring formatCount(uint64_t n);             // 1234567 -> "1,234,567"

// Writes a message to the debugger (DebugView, or your IDE's output window).
// Needed because the -mwindows link flag detaches the console, so printf in
// the GUI build goes nowhere at all.
void debugLog(const wchar_t* fmt, ...);

} // namespace util
