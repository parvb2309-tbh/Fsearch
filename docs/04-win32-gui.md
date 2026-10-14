# 4. The Win32 GUI — how a Windows program actually works

If you have only written console programs, the shape of a GUI program is
genuinely surprising. There is no loop you write that draws the screen. This
document explains the model, then the four specific techniques this program
uses.

## Messages

Everything that happens is a **message**: a number, plus two parameters. Key
pressed, mouse moved, window resized, time to repaint — all messages.

You write a function that receives them:

```cpp
LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_SIZE:    /* the window was resized */   return 0;
    case WM_CLOSE:   /* the X was clicked      */   return 0;
    case WM_KEYDOWN: /* a key went down        */   return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);   // Windows handles the rest
}
```

That is called the **window procedure**. The whole program is one large switch
statement, and "doing something" means responding to the right message.

The final line matters more than it looks. There are hundreds of messages, and
handling all of them yourself would be absurd — `DefWindowProcW` provides the
default behaviour for everything you do not care about. Forget it and the window
cannot be moved, resized, or closed.

## The message loop

```cpp
MSG msg;
while (GetMessageW(&msg, nullptr, 0, 0) != 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
}
```

- `GetMessageW` **blocks** until a message arrives, so an idle program uses no
  CPU at all.
- `TranslateMessage` turns key-down events into character events, so typing
  produces `WM_CHAR` as well as `WM_KEYDOWN`.
- `DispatchMessageW` calls the window procedure.

The loop ends when `GetMessageW` returns 0, which happens when it receives
`WM_QUIT` — posted by `PostQuitMessage(0)` in the `WM_DESTROY` handler.

Note the comparison against `0` rather than a plain truth test.
`GetMessageW` returns **-1** on error, and `while (GetMessage(...))` would treat
-1 as true and spin forever.

## Bridging from C to C++

Windows predates widespread C++ and only knows about plain functions, so the
window procedure has to be `static` — it cannot be a normal member function
that knows which object it belongs to.

The standard solution stores the object pointer in the window's own storage:

```cpp
LRESULT CALLBACK MainWindow::wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    MainWindow* self = nullptr;

    if (msg == WM_NCCREATE) {
        // The very first message a window receives. CreateWindowExW passed
        // our 'this' pointer through in the CREATESTRUCT.
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<MainWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self) return self->handleMessage(msg, wp, lp);   // now in C++ land
    return DefWindowProcW(hwnd, msg, wp, lp);
}
```

`GWLP_USERDATA` is a pointer-sized slot Windows sets aside per window for
exactly this. After `WM_NCCREATE`, every message reaches a real member function.

The null check is not defensive padding — a few messages genuinely arrive before
`WM_NCCREATE` has run.

## Controls are just windows

The search box, the results list and the status bar are child windows of
pre-registered classes:

```cpp
m_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                         WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                         0, 0, 0, 0, m_hwnd, (HMENU)ID_SEARCH_EDIT, hInst, nullptr);
```

You talk to them by sending messages (`SendMessageW`), and they talk back by
sending `WM_COMMAND` or `WM_NOTIFY` to their parent.

They do not lay themselves out. `WM_SIZE` arrives on the parent, and it must
reposition each child with `MoveWindow`.

---

# The four techniques that matter here

## 1. The virtual list

The results list could hold 1,000 rows, and inserting them one at a time is
slow enough to see. `LVS_OWNERDATA` makes it **virtual**: you tell the control
how many rows exist, and it asks for the text of the ~30 currently on screen.

```cpp
// Tell it the count...
ListView_SetItemCountEx(m_list, m_results.size(),
                        LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);

// ...and answer LVN_GETDISPINFOW for each visible cell.
```

Scrolling is instant no matter how many results there are, because the work is
proportional to what is visible rather than what exists.

Three rules come with it, and each one is a real bug if broken.

**Replace the data before lowering the count.** Shrink the vector first and
there is a window where the control believes there are more rows than exist. A
repaint arriving in that gap asks for row 900 of a 12-row vector and reads far
past the end — an access violation inside a paint handler, which is miserable
to debug. The code does it in the safe order and bounds-checks anyway.

**Copy into the buffer the control gives you.** `LVN_GETDISPINFOW` supplies
`pszText` and `cchTextMax`:

```cpp
copyToBuffer(item.pszText, item.cchTextMax, idx.name(fileId), idx.nameLen(fileId));
```

It is tempting to point `pszText` at your own string instead. That is allowed,
but the buffer must stay alive until *after* the handler returns — and a local
`std::wstring` is destroyed on return, so its memory is freed before the control
reads it. The text comes out as garbage or crashes, depending on what reused the
memory.

**`ListView_SortItems` does not work.** There are no items for it to sort. Sort
your own vector and invalidate.

## 2. Never touch the UI from another thread

> Only the thread that created a window may touch it.

The scan takes 25 seconds. Running it on the UI thread would freeze the window
solid, so it runs on its own thread — and it would be natural to have that
thread update the status bar as it goes.

That will appear to work, and will then deadlock or corrupt the display at
random intervals, usually on someone else's machine.

`PostMessage` is the thread-safe exception. It drops a note in the window's
queue and returns immediately:

```cpp
// On the scan thread - this is the ONLY thing it does to the UI.
PostMessageW(m_hwnd, MSG_SCAN_PROGRESS, filesFound, dirsFound);
```

```cpp
// On the UI thread.
case MSG_SCAN_PROGRESS:
    m_progressFiles = wParam;
    updateStatus();
    return 0;
```

Two details:

**Throttle.** Posting per file would queue 433,198 messages faster than they
could be drained. Progress is reported about every 100 ms.

**Send numbers, not pointers.** `PostMessage` can fail — it returns 0 when the
queue is full — and a failed post of a heap pointer leaks it with no way to
tell. Numbers cannot leak.

For the finished index, which is too big to pass in a message, the worker puts
it in a member guarded by a mutex and posts a bare "done" notification. The
mutex is held only for the handover, never during the scan.

## 3. Debouncing the search

`EN_CHANGE` fires on every keystroke, and a full search is 15–40 ms. Typing
`report` would queue six back-to-back searches, and the window would be stuck in
the last one while you were already typing the next word.

So each keystroke **restarts a timer** instead:

```cpp
void MainWindow::scheduleSearch() {
    SetTimer(m_hwnd, TIMER_SEARCH, 120, nullptr);
}

case WM_TIMER:
    if (wParam == TIMER_SEARCH) {
        KillTimer(m_hwnd, TIMER_SEARCH);
        runSearch();
    }
```

The whole mechanism rests on one behaviour: `SetTimer` with an ID that already
exists **replaces** it, restarting the countdown. So the search runs only once
typing pauses — `report` costs one search rather than six.

120 ms is below the threshold where a UI feels sluggish.

## 4. Subclassing the search box

The search box should keep the caret and keep receiving typed characters, while
Up and Down move through the results. None of that is built in.

**Subclassing** means intercepting a control's messages before its own code sees
them:

```cpp
SetWindowSubclass(m_edit, &MainWindow::editSubclassProc, 1, (DWORD_PTR)this);
```

Three things to get right:

**Always fall through.** Anything you do not handle must go to
`DefSubclassProc`, or the control appears completely dead.

**Unhook in `WM_NCDESTROY`** — the control's own destruction message. Doing it
from the parent's `WM_DESTROY` is too late; the child is already gone.

**Handling `VK_RETURN` in `WM_KEYDOWN` is not enough.** This one is genuinely
confusing. A single-line edit control still receives character 13 as `WM_CHAR`,
and with no default button in the window it responds by **beeping at you** every
time you press Enter. The key works perfectly; only the sound is wrong, so you
go looking in entirely the wrong place. The fix is to swallow `WM_CHAR` for 13
and 27 as well:

```cpp
case WM_CHAR:
    if (wParam == 13 || wParam == 27) return 0;
    break;
```

## 5. The right-click menu

Two lines of code create it, and the interesting part is everything around them.

```cpp
HMENU menu = CreatePopupMenu();
AppendMenuW(menu, MF_STRING, ID_MENU_OPEN,          L"&Open\tEnter");
AppendMenuW(menu, MF_STRING, ID_MENU_OPEN_LOCATION, L"Open file &location\tCtrl+Enter");
SetMenuDefaultItem(menu, ID_MENU_OPEN, FALSE);
TrackPopupMenuEx(menu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON,
                 x, y, m_hwnd, nullptr);
DestroyMenu(menu);
```

The text after `\t` is right-aligned by Windows and is **only a label** — it
does not create the shortcut. Enter and Ctrl+Enter are implemented separately
in the edit subclass; the label just makes them discoverable, which is half the
reason to have a menu at all.

`TrackPopupMenuEx` blocks until the user picks something, then posts the choice
as `WM_COMMAND` to the window passed as its owner. Passing `m_hwnd` rather than
the list is why the main window's handler receives it.

Four things are less obvious:

**A list view does not reliably send `WM_CONTEXTMENU`.** It usually consumes
the right-click itself, so handling only that message gives you no menu at all
on a mouse click. The notification it does reliably send is **`NM_RCLICK`**.
Handle that, and return `TRUE` to suppress the default processing — otherwise
the default *also* raises `WM_CONTEXTMENU` and the menu appears twice.

**The keyboard route arrives somewhere else entirely.** Focus deliberately
stays in the search box, so the Menu key and Shift+F10 send `WM_CONTEXTMENU`
to the *edit control*, which answers with its own Cut/Copy/Paste menu. Left
alone, the results menu is simply unreachable from the keyboard. The edit
subclass therefore checks for `lParam == (-1, -1)` — which is how Windows
signals "no cursor was involved" — and redirects to the results menu when a
result is selected. A genuine right-click inside the box still gets the normal
editing menu.

**Select the row under the cursor first.** The row you right-clicked is not
necessarily the selected one. Without a hit-test and an explicit selection,
"Open" acts on whatever was selected beforehand — which looks, to a user, like
the program opening the wrong file at random.

**`DestroyMenu` is not optional.** Menus are a limited system resource, and one
leaked per right-click adds up.

---

## Two things deliberately not done

**`IsDialogMessage` is not called.** It would give automatic Tab navigation,
but it also swallows Enter and Escape and converts them into `IDOK`/`IDCANCEL`
commands — fighting the search box's own handling of exactly those keys. With
three controls it is not worth the conflict.

**DPI awareness is "system", not "PerMonitorV2".** PerMonitorV2 turns off
Windows' scaling entirely and makes you handle `WM_DPICHANGED`, recreate every
font and rescale every control whenever the window moves between monitors with
different scaling. System awareness reads the DPI once at startup:

```cpp
m_dpi = GetDpiForSystem();
int scaled(int px) const { return MulDiv(px, m_dpi, 96); }
```

Crisp on a single monitor, or on several at the same scaling — which covers
almost everyone. The trade is slight blurriness after dragging to a
differently-scaled monitor.

## The manifest

`res/app.manifest` is embedded into the .exe by `windres`, and two entries in it
are load-bearing:

**Common Controls version 6.** Windows ships two versions of the control
library, and you get the 1990s one *by default*. The manifest is the only way to
ask for the modern themed one.

This works together with `InitCommonControlsEx` in `main.cpp` — the manifest
makes the library available, that call loads the classes. Miss the manifest and
the window looks like Windows 95. Miss the call and `CreateWindowExW` for the
list returns **NULL**, with no error explaining why.

**`asInvoker` privileges** — run with the user's existing rights and show no
UAC prompt. This is a direct consequence of walking directories rather than
reading the NTFS Master File Table: the MFT approach would scan in 3 seconds
instead of 25, but needs administrator rights, meaning a UAC prompt on every
single launch.
