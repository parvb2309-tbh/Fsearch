// =============================================================================
// mainwnd.cpp - the window, its controls, and its message handling
// =============================================================================
#include "mainwnd.h"
#include "persist.h"
#include "util.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM

#include <algorithm>

namespace {

const wchar_t* kWindowClass = L"FastSearchMainWindow";
const wchar_t* kWindowTitle = L"FastSearch";

// How long to wait after the last keystroke before searching. Long enough to
// coalesce fast typing, short enough to feel immediate.
constexpr UINT SEARCH_DELAY_MS = 120;

// Columns in the results list.
enum Column { COL_NAME = 0, COL_PATH, COL_SIZE, COL_MODIFIED, COL_COUNT };

// Copies a string into a fixed buffer supplied by someone else, always
// NUL-terminating and never overrunning.
//
// Written out rather than using wcsncpy because wcsncpy does NOT terminate
// when the source is too long - a classic source of buffer overruns.
void copyToBuffer(wchar_t* dst, int dstCapacity, const wchar_t* src, size_t srcLen) {
    if (dst == nullptr || dstCapacity <= 0) return;
    const size_t maxChars = static_cast<size_t>(dstCapacity) - 1;
    const size_t n = (srcLen < maxChars) ? srcLen : maxChars;
    if (n > 0) wmemcpy(dst, src, n);
    dst[n] = L'\0';
}

void copyToBuffer(wchar_t* dst, int dstCapacity, const std::wstring& src) {
    copyToBuffer(dst, dstCapacity, src.c_str(), src.size());
}

} // anonymous namespace

// =============================================================================
// Construction and window class
// =============================================================================
MainWindow::~MainWindow() {
    stopScanAndWait();
    if (m_font) DeleteObject(m_font);
}

bool MainWindow::registerClass(HINSTANCE hInst) {
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = &MainWindow::wndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = kWindowClass;
    wc.hIcon         = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm       = LoadIconW(nullptr, IDI_APPLICATION);

    return RegisterClassExW(&wc) != 0;
}

bool MainWindow::create(HINSTANCE hInst, int nCmdShow) {
    m_hInst = hInst;

    // The last argument passes 'this' through to WM_NCCREATE, which is how the
    // static window procedure finds the object it belongs to. Windows only
    // knows about HWNDs and plain functions; this is the standard bridge from
    // that C-style world back to a C++ object.
    m_hwnd = CreateWindowExW(
        0,
        kWindowClass,
        kWindowTitle,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        900, 600,
        nullptr, nullptr, hInst,
        this);

    if (!m_hwnd) return false;

    ShowWindow(m_hwnd, nCmdShow);
    UpdateWindow(m_hwnd);
    return true;
}

// -----------------------------------------------------------------------------
// wndProc - the static entry point Windows calls
// -----------------------------------------------------------------------------
// Windows has no concept of objects, so it calls a plain function. We stash the
// MainWindow pointer in the window's own storage (GWLP_USERDATA) during
// WM_NCCREATE - the very first message a window receives - and retrieve it on
// every message after that.
LRESULT CALLBACK MainWindow::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    MainWindow* self = nullptr;

    if (msg == WM_NCCREATE) {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<MainWindow*>(cs->lpCreateParams);
        self->m_hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    // Messages can arrive before WM_NCCREATE has run, so self may legitimately
    // be null - hand those straight to the default handler.
    if (self) return self->handleMessage(msg, wParam, lParam);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// -----------------------------------------------------------------------------
// handleMessage - the real window procedure
// -----------------------------------------------------------------------------
LRESULT MainWindow::handleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {

    case WM_CREATE:
        return onCreate() ? 0 : -1;   // returning -1 aborts window creation

    case WM_SIZE:
        layoutControls();
        return 0;

    case WM_SETFOCUS:
        // Focus belongs in the search box essentially always.
        if (m_edit) SetFocus(m_edit);
        return 0;

    case WM_COMMAND:
        // WM_COMMAND arrives from two different sources, told apart by HIWORD:
        //
        //   HIWORD != 0  a child control notifying us of something
        //                (LOWORD = control ID, HIWORD = notification code)
        //   HIWORD == 0  a menu item was chosen
        //                (LOWORD = the menu command ID)
        //
        // Mixing the two up is a classic Win32 mistake, because a control ID
        // and a menu ID can easily collide if you are not careful.
        switch (LOWORD(wParam)) {

        case ID_SEARCH_EDIT:
            // EN_CHANGE fires on every edit to the search box: typing,
            // pasting, deleting. We do NOT search here - see scheduleSearch().
            if (HIWORD(wParam) == EN_CHANGE) {
                scheduleSearch();
                return 0;
            }
            break;

        case ID_MENU_OPEN:
            openSelected(false);
            return 0;

        case ID_MENU_OPEN_LOCATION:
            openSelected(true);
            return 0;
        }
        break;

    case WM_CONTEXTMENU:
        // Sent when the user right-clicks, or presses the Menu key / Shift+F10.
        //
        // wParam is the window that was clicked. Note this message reaches us
        // because the list view does not handle it itself - its default window
        // procedure passes it up to the parent, which is the normal way a
        // control lets its owner decide what the menu should contain.
        onContextMenu(reinterpret_cast<HWND>(wParam),
                      GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;

    case WM_TIMER:
        if (wParam == TIMER_SEARCH) {
            // One-shot: stop the timer, then do the work.
            KillTimer(m_hwnd, TIMER_SEARCH);
            runSearch();
            return 0;
        }
        break;

    case WM_NOTIFY: {
        NMHDR* hdr = reinterpret_cast<NMHDR*>(lParam);
        if (hdr->idFrom == ID_RESULT_LIST) {
            switch (hdr->code) {

            case LVN_GETDISPINFOW:
                // The virtual list asking us for the text of one cell.
                return onGetDispInfo(reinterpret_cast<NMLVDISPINFOW*>(lParam));

            case LVN_ODFINDITEMW:
                // Type-ahead inside the list. We do not support it (typing
                // goes to the search box instead), but the message MUST be
                // handled: returning -1 means "no match", whereas leaving it
                // unhandled can make the control misbehave.
                return -1;

            case NM_DBLCLK:
                openSelected(false);
                return 0;

            case NM_RCLICK: {
                // A list view usually consumes the right-click itself rather
                // than letting it turn into WM_CONTEXTMENU, so relying on that
                // message alone gives you no menu at all on a mouse click.
                // NM_RCLICK is the notification it does reliably send.
                //
                // GetMessagePos gives the cursor position at the time of the
                // message being handled, which is more correct than asking
                // where the cursor is *now* - by the time we run, it may have
                // moved.
                const DWORD pos = GetMessagePos();
                onContextMenu(m_list, GET_X_LPARAM(pos), GET_Y_LPARAM(pos));

                // Returning TRUE suppresses the control's default handling.
                // That matters: the default would ALSO raise WM_CONTEXTMENU,
                // and we would end up showing the menu twice.
                return TRUE;
            }
            }
        }
        break;
    }

    // ---- our own messages, posted by the scan thread ----------------------
    //
    // These carry plain numbers in wParam/lParam rather than a pointer to
    // heap-allocated data. That matters: PostMessage can fail (it returns 0
    // when the queue is full), and a failed post of a heap pointer leaks it
    // with no way to tell. Numbers cannot leak.
    case MSG_SCAN_PROGRESS:
        m_progressFiles = static_cast<uint64_t>(wParam);
        m_progressDirs  = static_cast<uint64_t>(lParam);
        updateStatus();
        return 0;

    case MSG_SCAN_DONE:
        onScanDone();
        return 0;

    case WM_CLOSE:
        // Stop and JOIN the scan thread before anything is torn down.
        //
        // Destroying a std::thread that is still running calls std::terminate
        // and kills the process instantly - no destructors, no cleanup. And
        // even without that, the worker holds references to members that are
        // about to be destroyed.
        stopScanAndWait();
        DestroyWindow(m_hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);   // ends the message loop in main.cpp
        return 0;
    }

    return DefWindowProcW(m_hwnd, msg, wParam, lParam);
}

// =============================================================================
// Setup
// =============================================================================
bool MainWindow::onCreate() {
    // GetDpiForSystem returns the scaling of the primary monitor: 96 at 100%,
    // 144 at 150%, and so on. Because the manifest declares system DPI
    // awareness, this value is fixed for the life of the process, so reading
    // it once here is enough.
    m_dpi = static_cast<int>(GetDpiForSystem());
    if (m_dpi <= 0) m_dpi = 96;

    createFont();
    if (!createControls()) return false;
    layoutControls();

    // Try the saved index first. Loading takes ~50ms versus ~25 seconds for a
    // scan, so the window is usable almost immediately on every run after the
    // first.
    auto bundle = std::make_shared<IndexBundle>();
    const std::wstring path = persist::defaultIndexPath();
    const persist::LoadResult r = persist::load(bundle->index, path);

    if (r == persist::LoadResult::Ok) {
        // The trie is never saved - it is rebuilt here, which takes ~110ms.
        // See the discussion in trie.h for why that is deliberate.
        bundle->trie.build(bundle->index);
        bundle->stats.filesFound = bundle->index.fileCount();
        bundle->stats.dirsFound  = bundle->index.dirCount();
        m_index = std::move(bundle);
        updateStatus();
    } else {
        // No usable index: scan now. The window is already up and responsive
        // while this runs.
        setStatusText(0, std::wstring(L"No saved index (") +
                         persist::describe(r) + L") - scanning...");
        startScan();
    }

    SetFocus(m_edit);
    return true;
}

void MainWindow::createFont() {
    // Without this every control uses a bitmap font from 1995. The correct
    // modern UI font is whatever the user has configured, which this reads.
    NONCLIENTMETRICSW ncm = {};
    ncm.cbSize = sizeof(ncm);

    if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm),
                                   &ncm, 0, static_cast<UINT>(m_dpi))) {
        m_font = CreateFontIndirectW(&ncm.lfMessageFont);
    }

    // Fall back to a stock font rather than leaving m_font null.
    if (!m_font) {
        m_font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    }
}

bool MainWindow::createControls() {
    // Search box.
    m_edit = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        0, 0, 0, 0,
        m_hwnd, reinterpret_cast<HMENU>(ID_SEARCH_EDIT), m_hInst, nullptr);
    if (!m_edit) return false;

    SendMessageW(m_edit, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
    SendMessageW(m_edit, EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(L"Search files..."));

    // Redirect the search box's keyboard handling through our own function so
    // arrow keys can drive the results list. See editSubclassProc.
    SetWindowSubclass(m_edit, &MainWindow::editSubclassProc, 1,
                      reinterpret_cast<DWORD_PTR>(this));

    // Results list.
    //
    // LVS_OWNERDATA makes this a "virtual" list: instead of us inserting rows,
    // the control asks for the text of whatever rows are currently on screen.
    // With up to a thousand results that difference is enormous - inserting
    // rows one by one takes seconds, while a virtual list is instant because
    // only the ~30 visible rows are ever built.
    m_list = CreateWindowExW(
        0, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP |
        LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS,
        0, 0, 0, 0,
        m_hwnd, reinterpret_cast<HMENU>(ID_RESULT_LIST), m_hInst, nullptr);

    // A null here almost always means InitCommonControlsEx was not called, or
    // the manifest is missing. Checking is worth it: the alternative is
    // sending messages to a null handle and wondering why nothing appears.
    if (!m_list) return false;

    SendMessageW(m_list, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);

    // FULLROWSELECT   - clicking anywhere in a row selects it, as users expect
    // DOUBLEBUFFER    - draws off-screen then blits. Virtual lists flicker
    //                   badly while scrolling without it.
    ListView_SetExtendedListViewStyle(
        m_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

    struct { const wchar_t* title; int width; } columns[] = {
        { L"Name",     280 },
        { L"Path",     380 },
        { L"Size",      90 },
        { L"Modified", 130 },
    };

    for (int i = 0; i < COL_COUNT; ++i) {
        LVCOLUMNW col = {};
        col.mask     = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        col.pszText  = const_cast<wchar_t*>(columns[i].title);
        col.cx       = scaled(columns[i].width);
        col.iSubItem = i;
        // Right-align the size column, as numbers should be.
        if (i == COL_SIZE) {
            col.mask |= LVCF_FMT;
            col.fmt   = LVCFMT_RIGHT;
        }
        ListView_InsertColumn(m_list, i, &col);
    }

    // Status bar. It positions itself along the bottom edge automatically,
    // provided we forward WM_SIZE to it (see layoutControls).
    m_status = CreateWindowExW(
        0, STATUSCLASSNAMEW, L"",
        WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
        0, 0, 0, 0,
        m_hwnd, reinterpret_cast<HMENU>(ID_STATUS_BAR), m_hInst, nullptr);
    if (!m_status) return false;

    SendMessageW(m_status, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);

    // Three sections: message, result count, index size. The -1 means "use all
    // remaining width".
    int parts[3] = { scaled(420), scaled(620), -1 };
    SendMessageW(m_status, SB_SETPARTS, 3, reinterpret_cast<LPARAM>(parts));

    return true;
}

void MainWindow::layoutControls() {
    if (!m_edit || !m_list || !m_status) return;

    RECT rc;
    GetClientRect(m_hwnd, &rc);
    const int width  = rc.right - rc.left;
    const int height = rc.bottom - rc.top;

    // The status bar resizes itself, but only if told the parent changed size.
    SendMessageW(m_status, WM_SIZE, 0, 0);

    RECT sb;
    GetWindowRect(m_status, &sb);
    const int statusHeight = sb.bottom - sb.top;

    const int margin     = scaled(8);
    const int editHeight = scaled(26);

    MoveWindow(m_edit, margin, margin,
               width - margin * 2, editHeight, TRUE);

    const int listTop = margin * 2 + editHeight;
    MoveWindow(m_list, margin, listTop,
               width - margin * 2,
               height - listTop - statusHeight - margin, TRUE);
}

// =============================================================================
// Scanning
// =============================================================================
void MainWindow::startScan() {
    if (m_scanning.exchange(true)) return;   // one scan at a time

    // If a previous thread object is still around, join it first - assigning
    // over a joinable std::thread calls std::terminate.
    if (m_scanThread.joinable()) m_scanThread.join();

    m_scanner.resetCancel();
    m_progressFiles = 0;
    m_progressDirs  = 0;

    // Capturing 'this' is safe because WM_CLOSE joins this thread before the
    // window - and therefore this object - is destroyed.
    m_scanThread = std::thread([this]() {
        auto bundle = std::make_shared<IndexBundle>();
        bundle->index.reserve(700000, 80000);

        // Resolved next to the .exe, not the current directory - see
        // util::exeDirectory() for why that distinction matters.
        m_scanner.loadExclusions(
            util::joinPath(util::exeDirectory(), L"config.txt"));

        // The worker touches NO window state. All it does is post numbers.
        m_scanner.onProgress = [this](const ScanStats& s) {
            PostMessageW(m_hwnd, MSG_SCAN_PROGRESS,
                         static_cast<WPARAM>(s.filesFound),
                         static_cast<LPARAM>(s.dirsFound));
        };

        m_scanner.scan(L"C:\\", bundle->index, bundle->stats);

        if (!m_scanner.isCancelled()) {
            bundle->index.buildSortIndex();
            bundle->trie.build(bundle->index);

            FILETIME now;
            GetSystemTimeAsFileTime(&now);
            ULARGE_INTEGER uli;
            uli.LowPart  = now.dwLowDateTime;
            uli.HighPart = now.dwHighDateTime;
            bundle->builtAt = uli.QuadPart;

            // Save for next launch. Failure here is not fatal - it only means
            // the next start will scan again.
            persist::save(bundle->index, persist::defaultIndexPath());

            // Hand the finished index over under the mutex, then notify.
            {
                std::lock_guard<std::mutex> lock(m_pendingMutex);
                m_pending = std::move(bundle);
            }
            PostMessageW(m_hwnd, MSG_SCAN_DONE, 0, 0);
        }

        m_scanning.store(false);
    });
}

void MainWindow::onScanDone() {
    std::shared_ptr<IndexBundle> incoming;
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        incoming = std::move(m_pending);
        m_pending.reset();
    }
    if (!incoming) return;

    // THE SWAP. Assigning the shared_ptr is the moment the new index becomes
    // live. Any search already in progress - there cannot be one, since we are
    // on the UI thread - would keep its own reference to the old bundle, which
    // is freed only when that reference goes away.
    m_index = std::move(incoming);

    // Re-run the current query against the new data.
    runSearch();
    updateStatus();
}

void MainWindow::stopScanAndWait() {
    m_scanner.requestCancel();
    if (m_scanThread.joinable()) m_scanThread.join();
    m_scanning.store(false);
}

// =============================================================================
// Searching
// =============================================================================
// WHY WE DO NOT SEARCH ON EVERY KEYSTROKE
// ---------------------------------------
// A prefix search is microseconds, but a substring search scans every name in
// the index - 10 to 40ms. EN_CHANGE fires on every character, so typing
// "report" at a normal speed would queue six full scans back to back, and the
// window would be blocked in the last of them while you were already typing
// the next word.
//
// Instead each keystroke restarts a 120ms timer. Only when typing pauses does
// the search actually run, so "report" costs one search rather than six. The
// delay is below the threshold where a person notices a UI feels sluggish.
void MainWindow::scheduleSearch() {
    // SetTimer on an ID that already exists REPLACES it, restarting the
    // countdown. That single behaviour is the whole debounce mechanism.
    SetTimer(m_hwnd, TIMER_SEARCH, SEARCH_DELAY_MS, nullptr);
}

void MainWindow::runSearch() {
    wchar_t buf[512];
    GetWindowTextW(m_edit, buf, 512);
    m_lastQuery = buf;

    if (!m_index || m_lastQuery.empty()) {
        setResults({});
        updateStatus();
        return;
    }

    SearchOptions opt;
    opt.maxResults = 1000;

    auto results = search::run(m_index->index, &m_index->trie,
                               m_lastQuery, opt, &m_lastStats);
    setResults(std::move(results));
    updateStatus();
}

void MainWindow::setResults(std::vector<SearchResult>&& results) {
    m_results = std::move(results);

    // ORDER MATTERS AND IS NOT OBVIOUS.
    //
    // The vector is replaced FIRST, then the control is told the new count.
    // Do it the other way around and there is a window in which the control
    // believes there are more rows than the vector holds; if a repaint happens
    // in that gap it asks for row 900 of a 12-row vector and reads far past
    // the end - an access violation inside a paint handler, which is a
    // miserable thing to debug.
    //
    // LVSICF_NOINVALIDATEALL / NOSCROLL stop the control redrawing everything
    // and jumping the scroll position on each update.
    ListView_SetItemCountEx(m_list, static_cast<int>(m_results.size()),
                            LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);

    // Back to the top for a new query, then repaint.
    ListView_EnsureVisible(m_list, 0, FALSE);
    InvalidateRect(m_list, nullptr, TRUE);
}

// -----------------------------------------------------------------------------
// onGetDispInfo - the virtual list asking for one cell
// -----------------------------------------------------------------------------
// Called for every visible cell on every repaint, so it must stay cheap - and
// it must never assume the requested row exists.
LRESULT MainWindow::onGetDispInfo(NMLVDISPINFOW* info) {
    LVITEMW& item = info->item;

    // BOUNDS CHECK. In theory the count we gave the control is always right;
    // in practice a stale repaint can arrive just after the results shrank.
    // One comparison here turns a crash into a blank cell.
    if (item.iItem < 0 ||
        static_cast<size_t>(item.iItem) >= m_results.size() ||
        !m_index) {
        if (item.mask & LVIF_TEXT) copyToBuffer(item.pszText, item.cchTextMax, L"", 0);
        return 0;
    }

    // Only fill in what was actually asked for.
    if (!(item.mask & LVIF_TEXT)) return 0;

    const FileIndex& idx = m_index->index;
    const uint32_t fileId = m_results[static_cast<size_t>(item.iItem)].fileId;
    const FileEntry& f = idx.files[fileId];

    // The control provides the buffer (pszText) and its capacity
    // (cchTextMax), and we COPY into it.
    //
    // It is tempting instead to point pszText at our own string. That is
    // allowed, but the buffer must then stay alive until the control has
    // finished with it - which is not until after this function returns. A
    // local std::wstring is destroyed on return, so its memory is freed before
    // the control ever reads it. The text comes out as garbage, or it crashes,
    // depending on what reused the memory. Copying avoids the whole question.
    switch (item.iSubItem) {

    case COL_NAME:
        copyToBuffer(item.pszText, item.cchTextMax,
                     idx.name(fileId), idx.nameLen(fileId));
        break;

    case COL_PATH:
        // Rebuilt from the parent chain on demand. Only ever called for rows
        // actually on screen, so at most a few dozen per repaint.
        copyToBuffer(item.pszText, item.cchTextMax, idx.dirPath(f.parentDir));
        break;

    case COL_SIZE:
        copyToBuffer(item.pszText, item.cchTextMax, util::formatSize(f.size));
        break;

    case COL_MODIFIED:
        copyToBuffer(item.pszText, item.cchTextMax, util::formatFileTime(f.mtime));
        break;

    default:
        copyToBuffer(item.pszText, item.cchTextMax, L"", 0);
        break;
    }

    return 0;
}

// -----------------------------------------------------------------------------
// onContextMenu - decide where the menu goes, and what it applies to
// -----------------------------------------------------------------------------
// There are two quite different ways this message arrives, and handling only
// the first is a common oversight:
//
//   MOUSE     lParam holds real screen coordinates. The row under the cursor
//             is not necessarily the selected one, so we select it first -
//             otherwise "Open" would act on whatever was selected before,
//             which is a genuinely confusing bug to hit as a user.
//
//   KEYBOARD  The Menu key or Shift+F10. lParam is (-1, -1), because there is
//             no cursor involved. We position the menu against the selected
//             row instead.
void MainWindow::onContextMenu(HWND from, int screenX, int screenY) {
    // Only the results list has a menu. Right-clicking the search box should
    // keep its own built-in Cut/Copy/Paste menu, so we let that fall through.
    if (from != m_list) {
        DefWindowProcW(m_hwnd, WM_CONTEXTMENU,
                       reinterpret_cast<WPARAM>(from),
                       MAKELPARAM(screenX, screenY));
        return;
    }

    if (screenX == -1 && screenY == -1) {
        // Keyboard-invoked: anchor the menu to the selected row.
        const int sel = ListView_GetNextItem(m_list, -1, LVNI_SELECTED);

        RECT rc;
        if (sel >= 0 && ListView_GetItemRect(m_list, sel, &rc, LVIR_BOUNDS)) {
            POINT pt = { rc.left + scaled(16), rc.bottom };
            ClientToScreen(m_list, &pt);
            screenX = pt.x;
            screenY = pt.y;
        } else {
            // Nothing selected - fall back to the list's top-left corner.
            POINT pt = { scaled(8), scaled(8) };
            ClientToScreen(m_list, &pt);
            screenX = pt.x;
            screenY = pt.y;
        }
    } else {
        // Mouse-invoked: select whatever row was actually clicked.
        POINT pt = { screenX, screenY };
        ScreenToClient(m_list, &pt);

        LVHITTESTINFO hit = {};
        hit.pt = pt;
        const int item = ListView_HitTest(m_list, &hit);

        if (item >= 0) {
            ListView_SetItemState(m_list, item,
                                  LVIS_SELECTED | LVIS_FOCUSED,
                                  LVIS_SELECTED | LVIS_FOCUSED);
        }
    }

    showContextMenu(screenX, screenY);
}

// -----------------------------------------------------------------------------
// showContextMenu
// -----------------------------------------------------------------------------
void MainWindow::showContextMenu(int screenX, int screenY) {
    // No row selected means every item would be meaningless, so show nothing
    // rather than a menu that cannot do anything.
    const int sel = ListView_GetNextItem(m_list, -1, LVNI_SELECTED);
    if (sel < 0 || static_cast<size_t>(sel) >= m_results.size()) return;

    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    // The text after the tab is right-aligned by Windows and is purely a
    // label - adding it here does NOT create the shortcut. Enter and
    // Ctrl+Enter are implemented separately in editSubclassProc; this just
    // makes them discoverable, which is half the point of having the menu.
    AppendMenuW(menu, MF_STRING, ID_MENU_OPEN,          L"&Open\tEnter");
    AppendMenuW(menu, MF_STRING, ID_MENU_OPEN_LOCATION, L"Open file &location\tCtrl+Enter");

    // Marks "Open" as the default, so it renders in bold - matching what a
    // double-click does.
    SetMenuDefaultItem(menu, ID_MENU_OPEN, FALSE);

    // TrackPopupMenuEx blocks until the user picks something or dismisses the
    // menu, then posts the chosen command back as WM_COMMAND to m_hwnd.
    //
    // Passing m_hwnd (not m_list) as the owner is deliberate: it is why the
    // WM_COMMAND handler above receives these, rather than them disappearing
    // into the list view's own window procedure.
    TrackPopupMenuEx(menu,
                     TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON,
                     screenX, screenY, m_hwnd, nullptr);

    // Menus are a limited system resource, so destroy it rather than leaking
    // one per right-click.
    DestroyMenu(menu);
}

// -----------------------------------------------------------------------------
// openSelected
// -----------------------------------------------------------------------------
void MainWindow::openSelected(bool revealInExplorer) {
    if (!m_index) return;

    const int sel = ListView_GetNextItem(m_list, -1, LVNI_SELECTED);
    if (sel < 0 || static_cast<size_t>(sel) >= m_results.size()) return;

    const uint32_t fileId = m_results[static_cast<size_t>(sel)].fileId;

    // NOTE: fullPath returns a plain path with NO long-path prefix, which is
    // exactly what the shell needs. Handing a prefixed path to ShellExecuteW
    // fails, and handing one to Explorer's select command silently opens the
    // wrong folder with no error at all.
    const std::wstring path = m_index->index.fullPath(fileId);
    if (path.empty()) return;

    if (revealInExplorer) {
        // Opens the containing folder with the file selected. Preferred over
        // launching "explorer.exe /select,<path>" because it reuses an
        // existing Explorer window and needs no quoting of odd characters.
        //
        // Both calls below are reported on failure rather than ignored. A
        // menu item that does nothing at all, with no explanation, is far
        // more confusing than one that says why - and the usual cause is
        // simply that the file has been deleted or renamed since the index
        // was built.
        PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(path.c_str());
        if (!pidl) {
            setStatusText(0, L"Could not locate: " + path);
            return;
        }

        const HRESULT hr = SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
        ILFree(pidl);   // we allocated it, we free it

        if (FAILED(hr)) {
            wchar_t buf[160];
            swprintf(buf, 160, L"Could not show in Explorer (0x%08lX)",
                     static_cast<unsigned long>(hr));
            setStatusText(0, buf);
        }
        return;
    }

    // ShellExecuteW returns an HINSTANCE for historical reasons. It is not a
    // real handle: any value GREATER THAN 32 means success, and anything else
    // is an error code. Comparing against NULL would be wrong.
    const HINSTANCE result = ShellExecuteW(m_hwnd, L"open", path.c_str(),
                                           nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        MessageBoxW(m_hwnd, (L"Could not open:\n" + path).c_str(),
                    kWindowTitle, MB_OK | MB_ICONWARNING);
    }
}

// =============================================================================
// Status bar
// =============================================================================
void MainWindow::setStatusText(int part, const std::wstring& text) {
    if (!m_status) return;
    SendMessageW(m_status, SB_SETTEXTW, static_cast<WPARAM>(part),
                 reinterpret_cast<LPARAM>(text.c_str()));
}

void MainWindow::updateStatus() {
    if (m_scanning.load()) {
        setStatusText(0, L"Scanning...  " +
                         util::formatCount(m_progressFiles) + L" files, " +
                         util::formatCount(m_progressDirs)  + L" folders");
        setStatusText(1, L"");
        setStatusText(2, L"");
        return;
    }

    if (!m_index) {
        setStatusText(0, L"No index");
        return;
    }

    if (m_lastQuery.empty()) {
        setStatusText(0, L"Ready - type to search");
    } else {
        wchar_t buf[128];
        swprintf(buf, 128, L"%ls results in %.1f ms",
                 util::formatCount(m_lastStats.totalMatches).c_str(),
                 m_lastStats.elapsedMs);
        setStatusText(0, buf);
    }

    setStatusText(1, util::formatCount(m_index->index.fileCount()) + L" files indexed");
    setStatusText(2, util::formatSize(m_index->index.memoryBytes()) + L" RAM");
}

// =============================================================================
// Edit control subclassing
// =============================================================================
// "Subclassing" here means intercepting a control's messages before its own
// code sees them. We want the search box to keep the caret and keep receiving
// typed characters, while the up/down arrows scroll the results list - the
// behaviour every search box has, and none of it is built in.
//
// SetWindowSubclass is the modern, safe way to do this. The older technique of
// swapping the window procedure pointer by hand breaks when two pieces of code
// try it on the same control.
LRESULT CALLBACK MainWindow::editSubclassProc(HWND hwnd, UINT msg,
                                              WPARAM wParam, LPARAM lParam,
                                              UINT_PTR /*idSubclass*/,
                                              DWORD_PTR refData) {
    MainWindow* self = reinterpret_cast<MainWindow*>(refData);

    switch (msg) {

    case WM_KEYDOWN:
        switch (wParam) {
        case VK_DOWN:
        case VK_UP:
        case VK_PRIOR:    // Page Up
        case VK_NEXT: {   // Page Down
            // Forward the keypress to the list so it moves the selection,
            // while focus stays here in the edit box.
            if (self && self->m_list) {
                int sel = ListView_GetNextItem(self->m_list, -1, LVNI_SELECTED);
                const int count = static_cast<int>(self->m_results.size());
                if (count > 0) {
                    int next = sel;
                    if (wParam == VK_DOWN)       next = (sel < 0) ? 0 : sel + 1;
                    else if (wParam == VK_UP)    next = (sel < 0) ? 0 : sel - 1;
                    else if (wParam == VK_NEXT)  next = (sel < 0) ? 0 : sel + 10;
                    else                         next = (sel < 0) ? 0 : sel - 10;

                    next = std::clamp(next, 0, count - 1);

                    ListView_SetItemState(self->m_list, next,
                                          LVIS_SELECTED | LVIS_FOCUSED,
                                          LVIS_SELECTED | LVIS_FOCUSED);
                    ListView_EnsureVisible(self->m_list, next, FALSE);
                }
            }
            return 0;   // handled - do not let the edit box move the caret
        }

        case VK_RETURN:
            if (self) {
                // Ctrl held: reveal in Explorer instead of opening.
                const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                self->openSelected(ctrl);
            }
            return 0;

        case VK_ESCAPE:
            if (self) SetWindowTextW(hwnd, L"");
            return 0;

        case VK_F5:
            if (self && !self->m_scanning.load()) self->startScan();
            return 0;
        }
        break;

    case WM_CONTEXTMENU: {
        // Focus deliberately stays in the search box while the arrow keys
        // drive the results list, so the Menu key / Shift+F10 arrives HERE
        // even when the user is looking at a selected result. Left alone,
        // the edit control answers with its own Cut/Copy/Paste menu and the
        // results menu is unreachable from the keyboard entirely.
        //
        // lParam of (-1, -1) means the keyboard raised it; a real right-click
        // carries actual screen coordinates. So: keyboard with a result
        // selected goes to the results menu, and anything else falls through
        // to the edit control's own menu, which is genuinely useful for
        // pasting a path into the search box.
        const int x = GET_X_LPARAM(lParam);
        const int y = GET_Y_LPARAM(lParam);

        if (self && x == -1 && y == -1 && self->m_list &&
            ListView_GetNextItem(self->m_list, -1, LVNI_SELECTED) >= 0) {
            self->onContextMenu(self->m_list, -1, -1);
            return 0;
        }
        break;
    }

    case WM_CHAR:
        // THE MYSTERIOUS BEEP.
        //
        // Handling VK_RETURN in WM_KEYDOWN above is not enough. A single-line
        // edit control still receives the character 13 as WM_CHAR, and since
        // this window has no default button it responds by beeping at you -
        // every time you press Enter.
        //
        // Swallowing 13 (Enter) and 27 (Escape) here is the fix. It is the
        // sort of thing that is thoroughly confusing until you know it, since
        // the key itself works perfectly and only the sound is wrong.
        if (wParam == 13 || wParam == 27) return 0;
        break;

    case WM_NCDESTROY:
        // Remove the subclass from within the control's OWN destruction
        // message. Doing it from the parent's WM_DESTROY is too late - the
        // child is already gone by then.
        RemoveWindowSubclass(hwnd, &MainWindow::editSubclassProc, 1);
        break;
    }

    // Everything we did not handle goes to the control's normal processing.
    // Forgetting this makes the control appear completely dead.
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}
