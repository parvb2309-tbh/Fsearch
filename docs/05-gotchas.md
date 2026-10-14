# 5. Gotchas — every trap in this project, by symptom

Organised by **what you would actually see**, because nearly all of these point
you at completely the wrong place. Several of them were hit while building this.

---

## 1. Every string prints as only its first character

**Symptom.** `C:\Users\parvb` prints as `C`. `433,198` prints as `4`. It looks
exactly as though the scanner truncated all your data, so you go hunting in the
scanner.

**Cause.** `%s` versus `%ls` in wide printf. The two compilers disagree:

| | `%s` in `wprintf` means | `%ls` means |
|---|---|---|
| Microsoft's compiler | `wchar_t*` | `wchar_t*` |
| **MinGW / ISO C** | **`char*`** | `wchar_t*` |

Pass a `wchar_t*` to `%s` under MinGW and it reads your UTF-16 as bytes. Since
ASCII in UTF-16 is "letter, zero byte, letter, zero byte...", and a zero byte
terminates a C string, **every string stops after one character**.

**Fix.** Always `%ls` for wide strings. It means `wchar_t*` under both
compilers.

```cpp
wprintf(L"Path: %ls\n", path.c_str());    // correct
wprintf(L"Path: %s\n",  path.c_str());    // prints one character
```

---

## 2. The results list is completely empty, with no error

**Symptom.** Everything builds and runs, the window appears, the search finds
matches (the status bar even says so) — but the list shows nothing at all.

**Cause.** `UNICODE` and `_UNICODE` were not defined.

Visual Studio's project templates define these. **MinGW does not.** And
`-municode` does *not* define them either — it only selects the Unicode startup
object that makes `wWinMain` the entry point.

Without them, `LVN_GETDISPINFO` expands to the ANSI variant (`-77`) while the
Unicode list view you created sends the Unicode one (`-177`). Your handler is
waiting for a message that never arrives.

**Fix.** In the Makefile, on the compile line:

```make
CXXFLAGS := -municode -DUNICODE -D_UNICODE ...
```

**Related:** if `CreateWindowExW(L"SysListView32", ...)` returns **NULL**
instead, the cause is a missing `InitCommonControlsEx` call or a missing
manifest — see #12.

---

## 3. Accented filenames never match

**Symptom.** Searching `resume` finds `Resume.pdf`. Searching `résumé` finds
nothing, even though `Résumé.pdf` is right there. Your test folder works fine;
your real Documents folder does not.

**Cause.** `towlower()` respects the current C locale, which is `"C"` at program
start. In that locale it handles ASCII A–Z and **passes every non-ASCII
character through unchanged**.

**Fix.** `CharUpperBuffW`. It folds the way Windows itself does, and — critically
— it is a 1-to-1 mapping, so the string length never changes.

That length-preservation is not incidental. `nameBlob` and `nameBlobFolded`
share the same offsets, so they must stay exactly the same length. This rules
out `LCMapStringEx` with `LCMAP_LINGUISTIC_CASING`, which *does* change length
(the German sharp-s folds to `SS`).

**Verify with:**

```
fsearch-cli.exe --root C:\test --query résumé
```

Should find `Résumé.pdf`. If it finds nothing, `towlower` has crept back in.

---

## 4. The saved index is corrupt every single time

**Symptom.** Saving works, loading fails immediately or produces nonsense. 100%
reproducible.

**Cause.** The file was opened in text mode. On Windows the C runtime then
rewrites byte `0x0A` as `0x0D 0x0A` on the way out, and treats `0x1A` as
end-of-file on the way in. A binary index is full of both.

**Fix.** Always `"wb"` and `"rb"`:

```cpp
FILE* f = _wfopen(path.c_str(), L"wb");    // binary
FILE* f = _wfopen(path.c_str(), L"w");     // silent corruption
```

Use `_wfopen`, not `fopen`, so non-ASCII user names in the path survive.

---

## 5. A corrupt index file crashes somewhere completely unrelated

**Symptom.** An access violation deep inside a paint handler, with a stack trace
pointing at drawing code that is perfectly correct.

**Cause.** A damaged index file loaded without being checked. A garbage count
becomes a 130 GB allocation; a garbage offset becomes a read a mile past the end
of the blob — which only faults later, when the GUI asks for a row.

**Fix.** Four layers, in `persist.cpp`, cheapest first:

1. **Magic number and version** — is this even our file, from this build?
2. **Expected size** — the header records the exact byte count the file must
   have. Checked *before* allocating anything, so a garbage count can never
   become a huge allocation.
3. **Checksum** — catches a right-sized file whose bytes have rotted. Without
   it, a single flipped byte loads happily. *(Verified: flipping one byte in a
   35 MB index is detected and triggers a rescan.)*
4. **`validate()`** — every offset inside the blob, every parent link a real
   directory, every sorted entry a real file ID.

On any failure: discard and rescan. A rescan costs 25 seconds; trusting a bad
index costs a crash.

---

## 6. A crash mid-save leaves a file that looks perfectly valid

**Symptom.** After a crash or power cut, the index loads "successfully" but is
missing data.

**Cause.** Magic and version are written *first*, so they are always fine. It is
the tail that is missing.

**Fix.** Write to `index.bin.tmp`, flush all the way to the physical disk, then
rename atomically:

```cpp
fflush(f);
_commit(_fileno(f));          // force the OS cache out to disk
fclose(f);
MoveFileExW(tmp, final, MOVEFILE_REPLACE_EXISTING);
```

`_commit` is not optional — without it the data can still be in RAM when the
rename completes.

---

## 7. Results are occasionally wrong, but it never crashes

**Symptom.** Search results include names that do not match. Intermittent, hard
to reproduce.

**Cause.** A C string function used on the name blob. Names are packed with **no
separators**, so they are **not null-terminated**. These all read into the next
name and past the end of the last one:

```
wcscmp   wcsncmp   _wcsicmp   _wcsnicmp
wcslen   wcscpy    wcsstr     StringCchCopyW
```

It rarely crashes, because reading past a name normally lands in the next
name — perfectly valid memory, just the wrong data.

**Fix.** Always carry an explicit length. Use `compareNames()` from `index.h`,
or `std::wmemcmp` with a count.

---

## 8. The scan stops part way through with a strange error

**Symptom.** The scan runs for a while, then fails or returns far fewer files
than expected. `ERROR_TOO_MANY_OPEN_FILES`.

**Cause.** A leaked `FindClose`. Every successful `FindFirstFileExW` needs
exactly one. The loop has several early exits — cancelled, excluded, error — and
missing the close on any one of them leaks a handle per directory. Over 56,000
directories the process runs out.

**Fix.** An RAII wrapper (`FindHandle` in `scanner.cpp`). A destructor cannot be
forgotten. This is the one place in this project where a small class genuinely
earns its keep.

---

## 9. The scan never finishes

**Symptom.** It runs forever, or the file count climbs past anything plausible.

**Cause.** A reparse point — a junction or symlink pointing at one of its own
ancestors. `C:\Users\All Users` is a junction to `C:\ProgramData`. Following
them loops forever.

**Fix.**

```cpp
if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
```

There is a second reason to skip them: OneDrive "files on demand" folders are
reparse points whose contents are not local. Enumerating is safe, but anything
that *opened* those files would trigger gigabytes of downloads.

---

## 10. Long paths fail with a confusing error

**Symptom.** `FindFirstFileExW` fails on some folders. Paths over 260
characters do not work.

**Cause and fix.** The long-path prefix (backslash backslash question
backslash) lifts the 260-character limit, but it also disables **all** path
normalisation. Under it:

- paths must be fully qualified — no relative, no `C:foo`
- **backslashes only** — forward slashes are normally translated for you, but
  not here
- no `.` or `..` — they become literal folder names
- **no doubled separators** — and this is the one that bites

The last one is the trap. A drive root already ends in a separator, so building
a pattern as `path + separator + "*"` produces a doubled separator and fails.
`util::joinPath()` exists solely to get this right every time.

UNC paths need a different form again — the two leading separators are
*replaced*, not appended to, or you get three in a row.

*(Verified: a 615-character path is found and reconstructed correctly.)*

---

## 11. "Show in Explorer" silently opens the wrong folder

**Symptom.** No error, no dialog. Explorer opens Documents instead of selecting
your file. Or `ShellExecuteW` just fails.

**Cause.** A long-path-prefixed path was handed to the shell. Filesystem APIs
accept the prefix; **the shell does not**.

**Fix.** Store paths *without* the prefix and add it only at the filesystem call
site. `FileIndex::fullPath()` returns a plain path, which is what the shell
needs.

**Related:** `ShellExecuteW` returns an `HINSTANCE` for historical reasons. It
is not a real handle — success is **any value greater than 32**. Comparing
against `NULL` is wrong.

---

## 12. The window looks like Windows 95, or the list is NULL

**Symptom.** Flat grey unthemed controls. Or `CreateWindowExW` for the list
returns NULL and every `SendMessage` to it does nothing.

**Cause.** Windows ships two versions of the common controls library, and you
get version 5 **by default**. Two separate things are required to get version 6,
and they fail differently:

| Missing | Symptom |
|---|---|
| Manifest entry | Windows 95 appearance |
| `InitCommonControlsEx` call | `CreateWindowExW` returns **NULL** |

**Fix.** Both. The manifest makes the library available; the call loads the
classes.

Also check the manifest resource ID is **1** in `app.rc`. ID 2 is for DLLs, and
Windows silently ignores a manifest with the wrong ID.

---

## 13. It beeps every time you press Enter

**Symptom.** Enter works correctly — the file opens — but Windows beeps. Deeply
confusing, because the *key* is fine and only the sound is wrong, so you look
everywhere except the right place.

**Cause.** Handling `VK_RETURN` in `WM_KEYDOWN` is not enough. A single-line
edit control still receives character 13 as `WM_CHAR`, and with no default
button in the window it responds by beeping.

**Fix.** Swallow it in `WM_CHAR` too:

```cpp
case WM_CHAR:
    if (wParam == 13 || wParam == 27) return 0;   // Enter, Escape
    break;
```

---

## 14. The program vanishes instantly when you close it mid-scan

**Symptom.** No crash dialog, no error. The process is just gone.

**Cause.** Destroying a `std::thread` that is still running calls
`std::terminate()`, which kills the process immediately.

**Fix.** Cancel and **join** before anything is destroyed:

```cpp
case WM_CLOSE:
    stopScanAndWait();      // requestCancel() then join()
    DestroyWindow(m_hwnd);
    return 0;
```

---

## 15. Rare, unreproducible crashes during a scan

**Symptom.** Crashes at random, usually on a bigger drive, never in testing.

**Cause.** The UI thread searching a `std::vector` while the scan thread appends
to it. Growing a vector allocates a bigger block, copies, and **frees the old
one** — so the reader is walking memory that has just been returned to the
allocator.

**Fix.** Never modify a live index. The scanner builds a **separate** one and
hands it over when finished; the UI swaps a `shared_ptr`. The old index stays
alive until nothing is using it. The search path then needs no locks at all.

---

## 16. The UI freezes solid during a scan

**Symptom.** The window stops repainting and Windows greys it out.

**Cause.** Either the scan is on the UI thread, or a worker is posting a message
per file — 433,198 messages queued faster than they can be drained.

**Fix.** Scan on a worker thread, and **throttle progress to roughly every
100 ms**. Send plain numbers, not pointers: `PostMessage` can fail when the
queue is full, and a failed post of a heap pointer leaks it with no way to tell.

---

## 17. Access violation inside a paint handler

**Symptom.** A crash in drawing code that is obviously correct.

**Cause.** With a virtual list (`LVS_OWNERDATA`), the results vector was shrunk
*before* the control was told the new count. In that gap the control still
believes there are more rows, and a repaint asks for row 900 of a 12-row vector.

**Fix.** Order matters: **replace the data, then set the count, then
invalidate.** And bounds-check `iItem` in the handler anyway.

---

## 18. Cell text is garbage or the list crashes while scrolling

**Symptom.** Random characters in cells, or crashes only when scrolling fast.

**Cause.** `pszText` was pointed at a local `std::wstring` in
`LVN_GETDISPINFOW`. That string is destroyed when the handler returns — before
the control reads it.

**Fix.** **Copy** into the buffer the control supplies:

```cpp
copyToBuffer(item.pszText, item.cchTextMax, name, nameLen);
```

---

## 19. Prefix search breaks, but only for names with hyphens

**Symptom.** Searching `co-` misses some files. Everything else works.

**Cause.** The sort used a linguistic comparison — `CompareStringW`,
`lstrcmpiW` or `wcscoll`. Those treat punctuation as a tiebreaker rather than a
character, so `coop`, `co-op` and `cool` interleave and the `co-` group is **no
longer contiguous**.

Contiguity is the assumption both the binary search and the trie rest on.

**Fix.** Sort with `std::wmemcmp` on the pre-folded names. Never use collation
for this array.

---

## 20. "libstdc++-6.dll was not found"

**Symptom.** Runs fine from an MSYS2 shell, fails when double-clicked from
Explorer.

**Cause.** The MSYS2 shell has the GCC runtime DLLs on `PATH`. Explorer does
not.

**Fix.** Link them in:

```make
LDFLAGS := -static -static-libgcc -static-libstdc++
```

---

## 21. A file called `NUL` appears in the project folder

**Symptom.** A stray `NUL` file that ordinary `rm` cannot delete.

**Cause.** `2>NUL` in the Makefile. That is cmd.exe syntax; when
`mingw32-make` uses `sh.exe` instead (which it does when MSYS2 is installed), it
creates a literal file with that name.

**Fix.** Do not redirect at all. A leading `-` already tells make to ignore
failure, and the rule only runs when the directory is genuinely missing.

To delete one that already exists, you need the long-path prefix — normal file
APIs treat `NUL` as the null device.

---

## 22. Undefined reference to `FOLDERID_LocalAppData`

**Symptom.** Compiles fine, fails at link.

**Cause.** The `FOLDERID_*` GUID constants live in their own library.

**Fix.** Add `-luuid`. And remember GNU `ld` is order-sensitive: **object files
first, libraries last.**

---

## 23. A comment silently swallows the next line

**Symptom.** `warning: multi-line comment`, and code that mysteriously does not
run.

**Cause.** A `//` comment ending with a backslash continues onto the next line —
including in ASCII-art diagrams:

```cpp
//        /  \        <-- this backslash continues the comment
//       A    B       <-- so this line is also a comment
```

**Fix.** Do not end a comment line with a backslash. Draw diagrams with `+`,
`-` and `|`.

---

## 24. `*(uint64_t*)&fileTime`

Not a symptom, a latent bug. `FILETIME` is only guaranteed 4-byte aligned, so
reading it as a `uint64_t` is undefined behaviour and can genuinely fault.

**Fix.** Go through `ULARGE_INTEGER`:

```cpp
ULARGE_INTEGER uli;
uli.LowPart  = ft.dwLowDateTime;
uli.HighPart = ft.dwHighDateTime;
uint64_t t = uli.QuadPart;
```

The same applies to the checksum, which uses `memcpy` rather than casting a
possibly-unaligned pointer to `uint64_t*`. Every compiler turns that `memcpy`
into a single load instruction, so it costs nothing.

---

## 25. Right-clicking a list view shows no menu

**Symptom.** You handle `WM_CONTEXTMENU`, the keyboard Menu key works, but
right-clicking with the mouse does nothing at all.

**Cause.** A list view normally consumes the right-click itself instead of
letting it become `WM_CONTEXTMENU`.

**Fix.** Handle the **`NM_RCLICK`** notification, which it does reliably send,
and return `TRUE` to suppress default processing — otherwise the default
handling raises `WM_CONTEXTMENU` as well and you get two menus stacked on top
of each other.

Related: if focus lives somewhere other than the list (here it deliberately
stays in the search box), the keyboard Menu key sends `WM_CONTEXTMENU` to
*that* control instead, and you must redirect it yourself. `lParam` of
`(-1, -1)` is how Windows says "this came from the keyboard, not a cursor".

---

## 26. Another process crashes when your test script pokes it

**Symptom.** An app that works perfectly by hand crashes instantly when driven
by a test script. Exit code `0xC0000409` (STATUS_STACK_BUFFER_OVERRUN), or an
access violation somewhere in comctl32 with a stack trace that makes no sense.

**Cause.** Sending a window message **across a process boundary** where the
message carries a **pointer**.

`SendMessage` copies wParam and lParam, which are just numbers. Windows
marshals the buffers behind a handful of well-known messages (`WM_SETTEXT`,
`WM_GETTEXT` and friends), but it does **not** marshal the rest. So this:

```powershell
# from PowerShell, aimed at another process
[Api]::SendMessage($list, LVM_SETITEMSTATE, 0, [ref]$lvitem)
```

hands the target an address that is only valid in *your* address space. It
dereferences it and dies — and the crash appears to be in the app, not the
test. It is very easy to conclude you have found a real bug.

This cost real time on this project: a context-menu command looked like it was
crashing the app, and a gdb backtrace showing the fault arriving through
`KiUserCallbackDispatcher` — the kernel dispatching a *sent* message — was what
gave it away.

**Fix.** From outside the process, use only pointer-free messages:

| Safe | Not safe across processes |
|---|---|
| `WM_LBUTTONDOWN` / `WM_RBUTTONDOWN` (coords packed in lParam) | `LVM_SETITEMSTATE`, `LVM_GETITEM` (pointer to a struct) |
| `WM_KEYDOWN` / `WM_CHAR` | `LVM_GETITEMTEXT` (pointer to a buffer) |
| `WM_COMMAND` | Anything taking a `RECT*`, `POINT*` or your own struct |
| `WM_SETTEXT` / `WM_GETTEXT` (marshalled by Windows) | |
| `LVM_GETITEMCOUNT`, `LVM_GETNEXTITEM` (integers only) | |

Driving the control with synthetic mouse and key messages exercises the real
code path anyway, which makes for a better test than poking its internal state.

---

## Quick reference

| You see | Go to |
|---|---|
| Strings print as one character | [#1](#1-every-string-prints-as-only-its-first-character) |
| List completely empty | [#2](#2-the-results-list-is-completely-empty-with-no-error) |
| Accented names never match | [#3](#3-accented-filenames-never-match) |
| Index always corrupt | [#4](#4-the-saved-index-is-corrupt-every-single-time) |
| Crash in paint handler | [#5](#5-a-corrupt-index-file-crashes-somewhere-completely-unrelated), [#17](#17-access-violation-inside-a-paint-handler) |
| Occasionally wrong results | [#7](#7-results-are-occasionally-wrong-but-it-never-crashes) |
| Scan stops early | [#8](#8-the-scan-stops-part-way-through-with-a-strange-error) |
| Scan never finishes | [#9](#9-the-scan-never-finishes) |
| Long paths fail | [#10](#10-long-paths-fail-with-a-confusing-error) |
| Explorer opens wrong folder | [#11](#11-show-in-explorer-silently-opens-the-wrong-folder) |
| Windows 95 look, or NULL control | [#12](#12-the-window-looks-like-windows-95-or-the-list-is-null) |
| Beeps on Enter | [#13](#13-it-beeps-every-time-you-press-enter) |
| Process vanishes on close | [#14](#14-the-program-vanishes-instantly-when-you-close-it-mid-scan) |
| Rare random crashes | [#15](#15-rare-unreproducible-crashes-during-a-scan) |
| UI freezes | [#16](#16-the-ui-freezes-solid-during-a-scan) |
| Garbage in cells | [#18](#18-cell-text-is-garbage-or-the-list-crashes-while-scrolling) |
| Hyphenated names missing | [#19](#19-prefix-search-breaks-but-only-for-names-with-hyphens) |
| Missing DLL on launch | [#20](#20-libstdc-6dll-was-not-found) |
| Link errors | [#22](#22-undefined-reference-to-folderid_localappdata) |
| Right-click shows no menu | [#25](#25-right-clicking-a-list-view-shows-no-menu) |
| Crashes only under a test script | [#26](#26-another-process-crashes-when-your-test-script-pokes-it) |
