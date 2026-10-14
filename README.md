# FastSearch

A fast file-search tool for Windows, written in C++ from scratch with no
external libraries. Type part of a filename and matching files appear as you
type — across the whole drive, in under a millisecond.

```
+---------------------------------------------------------------------+
| ntdll                                                               |
+---------------------------------------------------------------------+
| Name              | Path                        |    Size | Modified |
|-------------------|-----------------------------|---------|----------|
| ntdll.h           | C:\MinGW\include            |   283 B | 2017-... |
| ntdll.dll         | C:\Windows\System32         |  2.4 MB | 2026-... |
| ntdll.dll         | C:\Windows\SysWOW64         |  1.7 MB | 2026-... |
| libntdll.a        | C:\msys64\ucrt64\lib        |  1.8 MB | 2024-... |
+---------------------------------------------------------------------+
| 27 results in 21.1 ms | 433,198 files indexed    | 52 MB RAM        |
+---------------------------------------------------------------------+
```

## Measured performance

On the machine this was built on — a 476 GB NTFS drive with 433,198 indexed
files, using a single scanning thread:

| Operation | Time |
|---|---|
| First scan of the whole C: drive | 24.5 s (cold) / 4.4 s (warm cache) |
| Loading the saved index at startup | **52 ms** |
| Prefix search (autocomplete) | **0.27 µs** |
| Full search including substring scan | 15–40 ms |
| Memory used | 52–72 MB |

Windows Search is slower because it maintains a heavyweight content index and
queries it through a database layer. This does one thing instead: it keeps
every filename in RAM in a compact form, and searches that.

## Building

Requires MSYS2 UCRT64 with `g++` (this was built with 14.2.0). No other
dependencies — no Visual Studio, no CMake, no Qt.

```sh
mingw32-make            # builds fsearch.exe      (the GUI app)
mingw32-make console    # builds fsearch-cli.exe  (console version, for testing)
mingw32-make debug      # unoptimised build with debug symbols
mingw32-make clean
```

## Using it

Run `fsearch.exe` and start typing.

| Key / action | What it does |
|---|---|
| Type anything | Search as you type |
| Up / Down / PageUp / PageDown | Move through results (caret stays in the box) |
| Enter, or double-click | Open the selected file |
| Ctrl + Enter | Show the file in Explorer |
| **Right-click a result** | **Menu: Open / Open file location** |
| Menu key, or Shift + F10 | Same menu, from the keyboard |
| Escape | Clear the search box |
| F5 | Rescan the drive |

Right-clicking a result gives a menu with **Open** and **Open file location**.
The keyboard shortcuts are listed beside each item, so you can discover them
without reading this file.

Right-clicking the *search box* still gives the normal Cut / Copy / Paste menu,
which is handy for pasting a path in.

The first run scans the drive (about 25 seconds) and saves the result. Every
run after that loads in ~50 ms.

Edit `config.txt` to control which folders are skipped.

### Console version

Useful for testing the engine without the GUI in the way:

```sh
fsearch-cli.exe                      # scan, then search interactively
fsearch-cli.exe --root C:\Users      # scan one folder only (much faster)
fsearch-cli.exe --query report       # run one search and exit
fsearch-cli.exe --selftest           # verify the trie against binary search
fsearch-cli.exe --bench              # time both prefix strategies
fsearch-cli.exe --rescan             # ignore the saved index
fsearch-cli.exe --no-trie            # use the binary-search path instead
```

---

## Reading the code

The code is heavily commented, and the comments explain *why* rather than
restating *what*. If you read it in this order each file only depends on ones
you have already seen:

| # | File | What it covers |
|---|---|---|
| 1 | `src/index.h` | **Start here.** The data model, and why it is shaped this way. Everything else follows from this file. |
| 2 | `src/index.cpp` | Building the index, reconstructing paths, the prefix binary search. |
| 3 | `src/scanner.h/.cpp` | Walking the filesystem. Long paths, reparse points, exclusions. |
| 4 | `src/search.h/.cpp` | Prefix vs substring matching, and how results are ranked. |
| 5 | `src/trie.h/.cpp` | The prefix tree — what it is, and an honest assessment of whether it is worth it. |
| 6 | `src/persist.h/.cpp` | Saving and loading, and the four ways binary file I/O goes wrong. |
| 7 | `src/mainwnd.h/.cpp` | The Win32 GUI: messages, the virtual list, background threads. |
| 8 | `src/main.cpp` | Entry point and message loop. Deliberately tiny. |

The longer explanations live in `docs/`:

- **[docs/01-architecture.md](docs/01-architecture.md)** — how the pieces fit together
- **[docs/02-data-model.md](docs/02-data-model.md)** — why flat arrays beat objects here
- **[docs/03-indexing.md](docs/03-indexing.md)** — sorted arrays, tries, and measured numbers
- **[docs/04-win32-gui.md](docs/04-win32-gui.md)** — how a Windows GUI actually works
- **[docs/05-gotchas.md](docs/05-gotchas.md)** — every trap hit while building this, with its symptom

**If something breaks, read `docs/05-gotchas.md` first.** It is organised by
symptom, because most of these failures point you at entirely the wrong place.

---

## How it works, in brief

**1. Scan.** Walk every directory with `FindFirstFileExW`, depth-first. About
25 seconds for a 433k-file drive.

**2. Store compactly.** Filenames go into one big character array; each file is
a fixed 32-byte record holding offsets and numbers. Full paths are never
stored — each folder is recorded once with a pointer to its parent, and paths
are rebuilt on demand for the ~30 rows actually on screen. That is the
difference between 500 MB and 70 MB.

**3. Index.** File IDs are sorted by name. Because they are sorted, every
prefix occupies one contiguous block, findable with two binary searches. A trie
sits on top as a shortcut.

**4. Search.** Prefix matches come from the sorted index instantly. Substring
matches need a linear scan of every name — which sounds bad and takes ~20 ms,
because the names are one contiguous run of memory and that is the single
fastest thing you can ask a CPU to do.

**5. Rank.** Exact matches, then prefix, then substring; shorter names first,
then most recently modified.

**6. Save.** The whole index writes to disk as raw bytes, because it contains
no pointers — every reference is an integer index, which means the same thing
in any process. That is why startup is 52 ms instead of 25 seconds.

---

## What this deliberately does not do

- **Search file contents.** Filenames only.
- **Update automatically.** Press F5 after adding files. Live updates would
  mean watching the NTFS change journal, which needs administrator rights.
- **Read the MFT directly.** The "Everything" tool scans a drive in ~3 seconds
  by reading the NTFS Master File Table. That requires running as
  administrator — a UAC prompt on every launch. This walks directories
  instead: 25 seconds once, and no special privileges ever.

The scanner sits behind a small interface, so an MFT-based scanner could be
added later without touching the index, search, or GUI code.

## Project layout

```
Makefile          build rules, with every flag explained
config.txt        which folders to skip
res/app.manifest  themed controls, DPI awareness, no UAC prompt
res/app.rc        embeds the manifest into the .exe
src/              the program (see the reading order above)
docs/             the longer explanations
build/            object files (created by the build)
```

The saved index lives at `%LOCALAPPDATA%\FastSearch\index.bin` (~35 MB).
Deleting it is always safe — the app just rescans.
