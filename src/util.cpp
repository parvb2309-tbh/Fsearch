// =============================================================================
// util.cpp - implementation of the small helpers. See util.h for the "why".
// =============================================================================
#include "util.h"

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <algorithm>

namespace util {

// -----------------------------------------------------------------------------
// Case folding
// -----------------------------------------------------------------------------
void foldCaseInPlace(wchar_t* buf, size_t len) {
    if (len == 0 || buf == nullptr) return;

    // CharUpperBuffW uppercases in place and returns how many characters it
    // processed. Unlike the C library's towupper it does not stop at ASCII,
    // and unlike LCMapStringEx it never changes the length - which our
    // shared-offset design depends on.
    CharUpperBuffW(buf, static_cast<DWORD>(len));
}

std::wstring foldCase(const std::wstring& s) {
    std::wstring out = s;
    if (!out.empty()) {
        foldCaseInPlace(&out[0], out.size());
    }
    return out;
}

// -----------------------------------------------------------------------------
// Path helpers
// -----------------------------------------------------------------------------
// These two literals are easy to misread. Written out character by character:
//
//   kExtPrefix    is  backslash backslash question backslash
//   kExtUncPrefix is  backslash backslash question backslash U N C backslash
//
// In C++ source each backslash must itself be escaped, which is why they look
// twice as long as they are.
static const wchar_t* kExtPrefix    = L"\\\\?\\";
static const wchar_t* kExtUncPrefix = L"\\\\?\\UNC\\";

static bool startsWith(const std::wstring& s, const wchar_t* prefix) {
    const size_t n = wcslen(prefix);
    return s.size() >= n && wcsncmp(s.c_str(), prefix, n) == 0;
}

std::wstring toExtendedPath(const std::wstring& path) {
    // Already prefixed: leave it alone. Double-prefixing is invalid.
    if (startsWith(path, kExtPrefix)) return path;

    // A UNC path starts with two backslashes, e.g. a network share.
    // Its prefixed form REPLACES those two leading separators with the UNC
    // form rather than appending to them. Getting this wrong yields three
    // separators in a row, which fails with an unhelpful error.
    if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\') {
        return std::wstring(kExtUncPrefix) + path.substr(2);
    }

    return std::wstring(kExtPrefix) + path;
}

std::wstring fromExtendedPath(const std::wstring& path) {
    if (startsWith(path, kExtUncPrefix)) {
        // Restore the two leading separators a UNC path needs.
        return std::wstring(L"\\\\") + path.substr(wcslen(kExtUncPrefix));
    }
    if (startsWith(path, kExtPrefix)) {
        return path.substr(wcslen(kExtPrefix));
    }
    return path;
}

std::wstring joinPath(const std::wstring& dir, const wchar_t* name, size_t nameLen) {
    std::wstring out;
    out.reserve(dir.size() + 1 + nameLen);
    out = dir;

    // Only add a separator if there is not one already. This single check is
    // what stops a drive root (which already ends in a separator) from
    // producing a doubled separator - fatal once the long-path prefix is
    // applied, because that prefix disables the cleanup Windows would
    // otherwise do for us.
    if (!out.empty() && out.back() != L'\\') {
        out.push_back(L'\\');
    }
    out.append(name, nameLen);
    return out;
}

std::wstring joinPath(const std::wstring& dir, const std::wstring& name) {
    return joinPath(dir, name.c_str(), name.size());
}

std::wstring exeDirectory() {
    // GetModuleFileNameW with a null module handle gives the path of the
    // running .exe. It does not tell us how much room it needed, so we grow
    // the buffer until the result fits.
    std::wstring buf(MAX_PATH, L'\0');

    for (;;) {
        const DWORD written =
            GetModuleFileNameW(nullptr, &buf[0], static_cast<DWORD>(buf.size()));

        if (written == 0) return std::wstring();          // genuine failure

        // A return value equal to the buffer size means it was truncated.
        // (Older Windows versions do not set ERROR_INSUFFICIENT_BUFFER here,
        // so comparing against the size is the reliable test.)
        if (written < buf.size()) {
            buf.resize(written);
            break;
        }

        if (buf.size() > 65536) return std::wstring();    // sanity limit
        buf.resize(buf.size() * 2);
    }

    // Strip the filename, leaving the directory.
    const size_t slash = buf.find_last_of(L'\\');
    if (slash == std::wstring::npos) return std::wstring();
    return buf.substr(0, slash);
}

// -----------------------------------------------------------------------------
// Timer
// -----------------------------------------------------------------------------
int64_t Timer::s_frequency = 0;

Timer::Timer() : m_start(0) {
    if (s_frequency == 0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);   // ticks per second; fixed per boot
        s_frequency = f.QuadPart;
    }
    reset();
}

void Timer::reset() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    m_start = c.QuadPart;
}

double Timer::elapsedMs() const {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart - m_start) * 1000.0 /
           static_cast<double>(s_frequency);
}

double Timer::elapsedSec() const {
    return elapsedMs() / 1000.0;
}

// -----------------------------------------------------------------------------
// Formatting
// -----------------------------------------------------------------------------
std::wstring formatSize(uint64_t bytes) {
    const wchar_t* units[] = { L"B", L"KB", L"MB", L"GB", L"TB" };

    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; ++u; }

    wchar_t buf[64];
    if (u == 0) {
        swprintf(buf, 64, L"%llu B", static_cast<unsigned long long>(bytes));
    } else {
        // One decimal place below 10 units ("1.5 KB"), none above ("245 KB").
        swprintf(buf, 64, v < 10.0 ? L"%.1f %ls" : L"%.0f %ls", v, units[u]);
    }
    return buf;
}

std::wstring formatFileTime(uint64_t filetime) {
    if (filetime == 0) return std::wstring();

    // A FILETIME counts 100-nanosecond intervals since 1601-01-01 UTC.
    // Split it back into the two 32-bit halves the API expects.
    ULARGE_INTEGER uli;
    uli.QuadPart = filetime;

    FILETIME ft;
    ft.dwLowDateTime  = uli.LowPart;
    ft.dwHighDateTime = uli.HighPart;

    // Convert UTC to local time BEFORE formatting, or every timestamp is off
    // by the user's timezone offset.
    FILETIME local;
    if (!FileTimeToLocalFileTime(&ft, &local)) return std::wstring();

    SYSTEMTIME st;
    if (!FileTimeToSystemTime(&local, &st)) return std::wstring();

    wchar_t buf[64];
    swprintf(buf, 64, L"%04d-%02d-%02d %02d:%02d",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    return buf;
}

std::wstring formatCount(uint64_t n) {
    wchar_t raw[32];
    swprintf(raw, 32, L"%llu", static_cast<unsigned long long>(n));

    const std::wstring digits(raw);
    std::wstring out;
    out.reserve(digits.size() + digits.size() / 3);

    // Walk from the right, inserting a comma every three digits.
    int since = 0;
    for (size_t i = digits.size(); i-- > 0; ) {
        out.push_back(digits[i]);
        if (++since == 3 && i != 0) { out.push_back(L','); since = 0; }
    }
    std::reverse(out.begin(), out.end());
    return out;
}

void debugLog(const wchar_t* fmt, ...) {
    wchar_t buf[1024];
    va_list args;
    va_start(args, fmt);
    vswprintf(buf, 1024, fmt, args);
    va_end(args);
    OutputDebugStringW(buf);
}

} // namespace util
