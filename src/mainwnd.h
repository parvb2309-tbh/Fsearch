// =============================================================================
// mainwnd.h - the application window
// =============================================================================
//
// HOW A WINDOWS GUI PROGRAM IS SHAPED
// -----------------------------------
// There is no "main loop" you write yourself that draws things. Instead:
//
//   1. You register a window CLASS, which is mostly a pointer to a function.
//   2. You create a window from that class.
//   3. Windows sends that function a MESSAGE whenever anything happens -
//      a key pressed, the mouse moved, the window resized, a redraw needed.
//   4. Your message loop pulls messages from a queue and dispatches them.
//
// So the whole program is one big switch statement on a message number, and
// "doing something" means responding to the right message. That function is
// the "window procedure", or WndProc.
//
//
// THE ONE RULE YOU MUST NOT BREAK
// -------------------------------
// ONLY the thread that created a window may touch it.
//
// The scan runs on a background thread so the window stays responsive, and it
// would be natural to have that thread update the status bar directly as it
// goes. That will appear to work, and will then deadlock or corrupt the UI at
// random intervals, usually on someone else's machine.
//
// The safe pattern, used here, is that the worker thread only ever calls
// PostMessage - which is thread-safe by design. It drops a note in the
// window's queue and returns immediately. The UI thread picks it up later and
// does the actual updating. All the data crossing between them is either a
// plain number in the message, or an object handed over under a mutex.
//
//
// WHY THE INDEX IS REPLACED RATHER THAN UPDATED
// ---------------------------------------------
// The obvious approach - have the scan thread append into the FileIndex the UI
// is searching - is a use-after-free waiting to happen. std::vector grows by
// allocating a bigger block, copying, and FREEING THE OLD ONE. If the UI
// thread is midway through a search when that happens, it is reading memory
// that has just been handed back to the allocator.
//
// So the scanner builds an entirely separate IndexBundle and hands over the
// finished article. The UI swaps its shared_ptr to point at the new one; the
// old one is freed automatically once the last search using it has finished.
// The search path needs no locks at all, and cannot see a half-built index.
// =============================================================================
#pragma once

#include "index.h"
#include "scanner.h"
#include "search.h"
#include "trie.h"

#include <windows.h>
#include <commctrl.h>   // NMLVDISPINFOW and the list-view / status-bar APIs

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// The index and the trie built from it are only ever valid together, so they
// travel as one unit and are replaced as one unit.
struct IndexBundle {
    FileIndex index;
    Trie      trie;
    uint64_t  builtAt = 0;   // FILETIME, for "indexed 2 hours ago"
    ScanStats stats;
};

// const, because once published an index is never modified - only replaced.
// That constness is what makes it safe to read without a lock.
using IndexPtr = std::shared_ptr<const IndexBundle>;

class MainWindow {
public:
    MainWindow() = default;
    ~MainWindow();

    // Non-copyable: it owns a window handle and a thread.
    MainWindow(const MainWindow&)            = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    // Registers the window class. Call once at startup.
    static bool registerClass(HINSTANCE hInst);

    // Creates and shows the window. Returns false on failure.
    bool create(HINSTANCE hInst, int nCmdShow);

    HWND handle() const { return m_hwnd; }

private:
    // ---- messages we invent for ourselves --------------------------------
    // WM_APP is the first message number Windows guarantees it will never use
    // for its own purposes, so applications number their private messages
    // from there.
    enum : UINT {
        MSG_SCAN_PROGRESS = WM_APP + 1,   // wParam = files, lParam = dirs
        MSG_SCAN_DONE     = WM_APP + 2
    };

    // Child control IDs. Any distinct numbers will do; they identify which
    // child sent a notification.
    enum : int {
        ID_SEARCH_EDIT = 1001,
        ID_RESULT_LIST = 1002,
        ID_STATUS_BAR  = 1003
    };

    // Context-menu command IDs.
    //
    // These live in a different number range from the control IDs above for
    // one practical reason: both arrive as WM_COMMAND, and keeping them
    // clearly apart makes it obvious at a glance which kind you are looking
    // at. Windows does not care, but you will when reading the handler.
    enum : int {
        ID_MENU_OPEN          = 2001,
        ID_MENU_OPEN_LOCATION = 2002
    };

    // Timer used to delay searching until typing pauses. See onTimer().
    enum : UINT_PTR { TIMER_SEARCH = 1 };

    // ---- window plumbing ---------------------------------------------------
    static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    // Intercepts keys in the search box so the arrow keys drive the results
    // list without the caret ever leaving the box.
    static LRESULT CALLBACK editSubclassProc(HWND, UINT, WPARAM, LPARAM,
                                             UINT_PTR, DWORD_PTR);

    // ---- setup -------------------------------------------------------------
    bool onCreate();
    bool createControls();
    void createFont();
    void layoutControls();

    // ---- scanning ----------------------------------------------------------
    void startScan();
    void onScanDone();
    void stopScanAndWait();

    // ---- searching ---------------------------------------------------------
    void scheduleSearch();   // restarts the debounce timer
    void runSearch();        // actually performs the search
    void setResults(std::vector<SearchResult>&& results);

    // ---- results list ------------------------------------------------------
    LRESULT onGetDispInfo(NMLVDISPINFOW* info);
    void    openSelected(bool revealInExplorer);

    // Pops up the right-click menu at the given SCREEN coordinates.
    // Does nothing when no row is selected - a menu whose every item is
    // greyed out is just noise.
    void showContextMenu(int screenX, int screenY);

    // Works out where a context menu should appear and selects the row it
    // applies to. Handles both the mouse case and the keyboard case
    // (the Menu key or Shift+F10), which report no cursor position.
    void onContextMenu(HWND from, int screenX, int screenY);

    // ---- status bar --------------------------------------------------------
    void updateStatus();
    void setStatusText(int part, const std::wstring& text);

    // ---- scaling -----------------------------------------------------------
    // Converts a size written for a 96-DPI screen into actual pixels.
    int scaled(int px) const { return MulDiv(px, m_dpi, 96); }

    // ---- state -------------------------------------------------------------
    HINSTANCE m_hInst  = nullptr;
    HWND      m_hwnd   = nullptr;
    HWND      m_edit   = nullptr;
    HWND      m_list   = nullptr;
    HWND      m_status = nullptr;
    HFONT     m_font   = nullptr;
    int       m_dpi    = 96;

    // Owned and read by the UI thread only.
    IndexPtr                  m_index;
    std::vector<SearchResult> m_results;
    std::wstring              m_lastQuery;
    SearchStats               m_lastStats;

    // ---- worker handover ---------------------------------------------------
    // m_pending is the one piece of state both threads touch, so it is the one
    // piece guarded by a mutex. The worker fills it, posts MSG_SCAN_DONE, and
    // never looks at it again; the UI thread takes it and clears it.
    Scanner                      m_scanner;
    std::thread                  m_scanThread;
    std::mutex                   m_pendingMutex;
    std::shared_ptr<IndexBundle> m_pending;
    std::atomic<bool>            m_scanning{false};

    // Progress figures, written only by the UI thread from posted messages.
    uint64_t m_progressFiles = 0;
    uint64_t m_progressDirs  = 0;
};
