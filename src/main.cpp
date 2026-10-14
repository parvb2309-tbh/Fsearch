// =============================================================================
// main.cpp - program entry point and message loop
// =============================================================================
//
// This file is deliberately tiny. All it does is:
//   1. initialise the libraries we depend on
//   2. create the window
//   3. run the message loop until the window closes
//
// Everything else lives in mainwnd.cpp.
// =============================================================================
#include "mainwnd.h"

#include <windows.h>
#include <commctrl.h>
#include <objbase.h>

// -----------------------------------------------------------------------------
// wWinMain - where a Windows GUI program starts
// -----------------------------------------------------------------------------
// Four different entry points exist and the linker picks one based on build
// flags, which is a common source of "undefined reference to WinMain" errors:
//
//     main      - console program, 8-bit arguments
//     wmain     - console program, UTF-16 arguments        (-municode)
//     WinMain   - GUI program, 8-bit arguments             (-mwindows)
//     wWinMain  - GUI program, UTF-16 arguments            (-mwindows -municode)
//
// We use the last one. Both flags are therefore required, and both must appear
// on the LINK line as well as the compile line.
int WINAPI wWinMain(HINSTANCE hInstance,
                    HINSTANCE /*hPrevInstance*/,   // always null; a 16-bit relic
                    PWSTR     /*lpCmdLine*/,
                    int       nCmdShow) {

    // COM is needed by the shell functions we use to open files and reveal
    // them in Explorer. APARTMENTTHREADED is what shell APIs expect.
    //
    // We do not abort if this fails - the search half of the program works
    // perfectly well without it; only opening files would stop working.
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // Loads the version 6 common control classes, which is what actually makes
    // WC_LISTVIEW and the status bar available.
    //
    // BOTH halves are required: this call AND the manifest entry in
    // res/app.manifest. With the manifest but no call, creating the list view
    // returns NULL. With the call but no manifest, you get the unthemed
    // 1990s appearance. Neither failure says anything useful.
    INITCOMMONCONTROLSEX icc = {};
    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_LISTVIEW_CLASSES   // the results list
               | ICC_BAR_CLASSES        // the status bar
               | ICC_STANDARD_CLASSES;
    if (!InitCommonControlsEx(&icc)) {
        MessageBoxW(nullptr,
                    L"Failed to initialise common controls.",
                    L"FastSearch", MB_OK | MB_ICONERROR);
        return 1;
    }

    if (!MainWindow::registerClass(hInstance)) {
        MessageBoxW(nullptr, L"Failed to register the window class.",
                    L"FastSearch", MB_OK | MB_ICONERROR);
        return 1;
    }

    // The window is a local object, so its destructor runs when this function
    // returns - which is what stops and joins the scan thread cleanly.
    MainWindow window;
    if (!window.create(hInstance, nCmdShow)) {
        MessageBoxW(nullptr, L"Failed to create the main window.",
                    L"FastSearch", MB_OK | MB_ICONERROR);
        return 1;
    }

    // -------------------------------------------------------------------------
    // The message loop
    // -------------------------------------------------------------------------
    // GetMessageW BLOCKS until a message arrives, so an idle program uses no
    // CPU at all. It returns 0 when it receives WM_QUIT (posted by
    // PostQuitMessage in the window's WM_DESTROY handler), which ends the loop.
    //
    // It returns -1 on error, which is why the result is compared against 0
    // rather than just tested for truth - a plain "while (GetMessage(...))"
    // would treat -1 as true and loop forever on an error.
    //
    // TranslateMessage turns raw key-down events into character messages, so
    // typing produces WM_CHAR as well as WM_KEYDOWN.
    //
    // Note there is deliberately no IsDialogMessage call here. It would give
    // automatic Tab navigation between controls, but it also swallows Enter
    // and Escape and turns them into IDOK / IDCANCEL commands, which fights
    // the search box's own handling of those keys. With three controls it is
    // not worth the conflict.
    MSG msg;
    BOOL result;
    while ((result = GetMessageW(&msg, nullptr, 0, 0)) != 0) {
        if (result == -1) break;   // error; bail out rather than spin

        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (SUCCEEDED(comResult)) CoUninitialize();

    return static_cast<int>(msg.wParam);
}
