// screensaver.c - The black screen saver windows: per-monitor show/hide,
// hide/minimize vetoes, topmost reassertion, the watchdog that restores
// externally tampered windows, and shell-window (Start Menu etc.) handling.
//
// Part of OLED Aegis. See oled_aegis.h for the shared types/constants.

#include "oled_aegis.h"

LRESULT CALLBACK MonitorWindowProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    static DWORD ignoreInputUntil = 0;

    switch (message) {
        case WM_CREATE:
            ignoreInputUntil = GetTickCount() + INPUT_IGNORE_DELAY_MS;
            break;
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            RECT rect;
            GetClientRect(hWnd, &rect);
            FillRect(hdc, &rect, g_blackBrush);
            EndPaint(hWnd, &ps);
            break;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_WINDOWPOSCHANGING:
        {
            // The shell can externally hide topmost windows while showing
            // taskbar thumbnail previews (hovering over a taskbar icon).
            // While this monitor's screen saver is supposed to be active,
            // veto hide attempts so the black screen can't vanish without
            // the app knowing ("hidden but still active").
            WINDOWPOS* wpos = (WINDOWPOS*)lParam;
            int isActive = 0;
            for (int i = 0; i < g_monitorCount; i++) {
                if (g_monitorStates[i].hScreenSaverWnd == hWnd && g_monitorStates[i].screenSaverActive) {
                    isActive = 1;
                    break;
                }
            }
            if (isActive && (wpos->flags & SWP_HIDEWINDOW)) {
                LogMessage("Blocked external hide of monitor window (flags=0x%08X)", (DWORD)wpos->flags);
                wpos->flags &= ~SWP_HIDEWINDOW;
            }
            return 0;
        }
        case WM_STYLECHANGING:
        {
            // Same protection for minimize attempts (SW_MINIMIZE applies
            // WS_MINIMIZE via a style change).
            if (wParam == GWL_STYLE) {
                STYLESTRUCT* ss = (STYLESTRUCT*)lParam;
                int isActive = 0;
                for (int i = 0; i < g_monitorCount; i++) {
                    if (g_monitorStates[i].hScreenSaverWnd == hWnd && g_monitorStates[i].screenSaverActive) {
                        isActive = 1;
                        break;
                    }
                }
                if (isActive && (ss->styleNew & WS_MINIMIZE)) {
                    LogMessage("Blocked external minimize of monitor window (style=0x%08X)", (DWORD)ss->styleNew);
                    ss->styleNew &= ~WS_MINIMIZE;
                }
            }
            return 0;
        }
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (GetTickCount() < ignoreInputUntil) {
                break;
            }
            LogMessage("Input detected on monitor window (msg: %u)", message);
            if (g_app.config.perMonitorInputDetection) {
                for (int i = 0; i < g_monitorCount; i++) {
                    if (g_monitorStates[i].hScreenSaverWnd == hWnd) {
                        HideScreenSaverOnMonitor(i);
                        break;
                    }
                }
                if (!IsAnyMonitorActive()) {
                    g_app.screenSaverActive = 0;
                }
                UpdateTrayIcon(IsAnyMonitorActive() ? 1 : 0);
            } else {
                HideScreenSaver();
                UpdateTrayIcon(0);
            }
            break;
        default:
            return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

void HideScreenSaverOnMonitor(int monitorIndex) {
    if (monitorIndex < 0 || monitorIndex >= g_monitorCount) return;

    // Clear the active state BEFORE hiding the window: while a monitor is
    // active, MonitorWindowProc vetoes external hide/minimize attempts, and
    // our own SW_HIDE must not be blocked by that veto.
    g_monitorStates[monitorIndex].screenSaverActive = 0;

    if (g_monitorStates[monitorIndex].hScreenSaverWnd) {
        ShowWindow(g_monitorStates[monitorIndex].hScreenSaverWnd, SW_HIDE);
        LogMessage("Screen saver window hidden on monitor %d", monitorIndex);
    }

    g_monitorStates[monitorIndex].lastInputTime = time(NULL);
}

int IsShellWindowOpen() {
    HWND hFg = GetForegroundWindow();
    if (!hFg) {
        LogMessage("Shell detection: No foreground window");
        return 0;
    }

    char processName[MAX_PATH] = {0};
    if (!GetProcessNameFromHwnd(hFg, processName, sizeof(processName))) {
        LogMessage("Shell detection: Could not get process name for foreground window");
        return 0;
    }

    char className[256] = {0};
    GetClassNameA(hFg, className, sizeof(className));

    LogMessage("Shell detection: Foreground window - process='%s', class='%s'", processName, className);

    int shellWindowCount = 0;

    // Check for known shell host processes
    // ShellExperienceHost.exe - Start Menu, Action Center on Windows 11
    // SearchHost.exe - Windows Search/Start Menu
    // StartMenuExperienceHost.exe - Start Menu on Windows 10
    // ShellHost.exe - Action Center / Control Center on Windows 11
    if (_stricmp(processName, "ShellExperienceHost.exe") == 0 ||
        _stricmp(processName, "SearchHost.exe") == 0 ||
        _stricmp(processName, "StartMenuExperienceHost.exe") == 0 ||
        _stricmp(processName, "ShellHost.exe") == 0) {

        LogMessage("Shell detection: Shell host process detected: %s", processName);
        shellWindowCount = 1;
    }

    // Additional check: Task View is hosted by explorer.exe with specific window classes
    if (_stricmp(processName, "explorer.exe") == 0) {
        // Task View uses Windows.UI.Core.CoreWindow or XamlExplorerHostIslandWindow
        if (strstr(className, "Windows.UI.Core.CoreWindow") != NULL ||
            strstr(className, "XamlExplorerHostIslandWindow") != NULL) {

            LogMessage("Shell detection: Explorer shell window detected: class=%s", className);
            shellWindowCount = 1;
        }
    }

    if (shellWindowCount == 0) {
        LogMessage("Shell detection: No shell windows detected");
    }

    return shellWindowCount;
}

// Send Escape key(s) to close shell windows (Start Menu, Task View, Action Center)
void CloseShellWindows(int escapeCount) {
    LogMessage("Sending %d Escape key(s) to close shell windows", escapeCount);

    for (int i = 0; i < escapeCount; i++) {
        INPUT input[2] = {0};

        // Key down
        input[0].type = INPUT_KEYBOARD;
        input[0].ki.wVk = VK_ESCAPE;
        input[0].ki.dwFlags = 0;

        // Key up
        input[1].type = INPUT_KEYBOARD;
        input[1].ki.wVk = VK_ESCAPE;
        input[1].ki.dwFlags = KEYEVENTF_KEYUP;

        UINT sent = SendInput(2, input, sizeof(INPUT));
        LogMessage("SendInput returned %u (expected 2)", sent);

        // Small delay between escape presses if sending multiple
        if (i < escapeCount - 1) {
            Sleep(50);
        }
    }
}

void ShowScreenSaverOnMonitor(int monitorIndex, int isManual) {
    if (monitorIndex < 0 || monitorIndex >= g_monitorCount) return;
    if (!g_monitorStates[monitorIndex].enabled) return;
    if (g_monitorStates[monitorIndex].screenSaverActive) return;

    if (g_app.config.perMonitorInputDetection) {
        int wasInactiveCount = 0;
        for (int i = 0; i < g_monitorCount; i++) {
            if (g_monitorStates[i].enabled && !g_monitorStates[i].screenSaverActive) {
                wasInactiveCount++;
            }
        }

        if (wasInactiveCount == 1) {
            int sentEscapeKeys = 0;
            for (int attempt = 0; attempt < SHELL_CLOSE_MAX_ATTEMPTS; attempt++) {
                int shellWindowCount = IsShellWindowOpen();
                if (shellWindowCount > 0) {
                    LogMessage("Shell window(s) detected before last monitor activation (attempt %d), closing them", attempt + 1);
                    CloseShellWindows(1);
                    Sleep(SHELL_CLOSE_DELAY_MS);
                    sentEscapeKeys = 1;
                } else {
                    break;
                }
            }

            if (sentEscapeKeys) {
                // The Escape keys sent via SendInput update GetLastInputInfo,
                // which would make the next timer tick think the user is active
                // and deactivate the screen saver. Use the manual-activation
                // cooldown to suppress deactivation for a short period, instead
                // of resetting lastInputTime (which would also deactivate the
                // monitor we just activated).
                g_app.isManualActivation = 1;
                g_app.manualActivationTime = GetTickCount();
            }
        }
    } else {
        int sentEscapeKeys = 0;
        for (int attempt = 0; attempt < SHELL_CLOSE_MAX_ATTEMPTS; attempt++) {
            int shellWindowCount = IsShellWindowOpen();
            if (shellWindowCount > 0) {
                LogMessage("Shell window(s) detected before screen saver activation (attempt %d), closing them", attempt + 1);
                CloseShellWindows(1);
                Sleep(SHELL_CLOSE_DELAY_MS);
                sentEscapeKeys = 1;
            } else {
                break;
            }
        }

        if (sentEscapeKeys) {
            g_app.isManualActivation = 1;
            g_app.manualActivationTime = GetTickCount();
        }
    }

    if (g_monitorStates[monitorIndex].hScreenSaverWnd) {
        // Reposition and resize in case the pixel shift compensation setting changed since the
        // window was last created.
        LONG_PTR exStyle = GetWindowLongPtrW(g_monitorStates[monitorIndex].hScreenSaverWnd, GWL_EXSTYLE);
        if ((exStyle & WS_EX_NOACTIVATE) == 0) {
            SetWindowLongPtrW(g_monitorStates[monitorIndex].hScreenSaverWnd, GWL_EXSTYLE, exStyle | WS_EX_NOACTIVATE);
        }
        // Never let Aero Peek (e.g. hovering a taskbar thumbnail preview)
        // make the black screen transparent/invisible.
        BOOL excludeFromPeek = TRUE;
        DwmSetWindowAttribute(g_monitorStates[monitorIndex].hScreenSaverWnd, DWMWA_EXCLUDED_FROM_PEEK, &excludeFromPeek, sizeof(excludeFromPeek));
        int pad = g_app.config.pixelShiftCompensation;
        SetWindowPos(g_monitorStates[monitorIndex].hScreenSaverWnd, HWND_TOPMOST,
                     g_monitors[monitorIndex].rect.left   - pad,
                     g_monitors[monitorIndex].rect.top    - pad,
                     g_monitors[monitorIndex].rect.right  - g_monitors[monitorIndex].rect.left + pad * 2,
                     g_monitors[monitorIndex].rect.bottom - g_monitors[monitorIndex].rect.top  + pad * 2,
                     SWP_NOACTIVATE);
        ShowWindow(g_monitorStates[monitorIndex].hScreenSaverWnd, SW_SHOWNOACTIVATE);
        UpdateWindow(g_monitorStates[monitorIndex].hScreenSaverWnd);
        g_monitorStates[monitorIndex].screenSaverActive = 1;
        LogMessage("Screen saver window shown on monitor %d (reused)", monitorIndex);
    } else {
        // Expand the window beyond the monitor's reported bounds by the pixel shift compensation
        // amount on all four sides. This ensures hardware pixel shift (used by some OLED panels
        // to reduce burn-in) cannot expose the desktop behind the screen saver window.
        int pad = g_app.config.pixelShiftCompensation;
        HWND hWnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                    L"OLEDAegisScreen", L"",
                                   WS_POPUP,
                                   g_monitors[monitorIndex].rect.left   - pad,
                                   g_monitors[monitorIndex].rect.top    - pad,
                                   g_monitors[monitorIndex].rect.right  - g_monitors[monitorIndex].rect.left + pad * 2,
                                   g_monitors[monitorIndex].rect.bottom - g_monitors[monitorIndex].rect.top  + pad * 2,
                                   NULL, NULL, GetModuleHandle(NULL), NULL);

        if (hWnd) {
            // Never let Aero Peek (e.g. hovering a taskbar thumbnail preview)
            // make the black screen transparent/invisible.
            BOOL excludeFromPeek = TRUE;
            DwmSetWindowAttribute(hWnd, DWMWA_EXCLUDED_FROM_PEEK, &excludeFromPeek, sizeof(excludeFromPeek));

            ShowWindow(hWnd, SW_SHOWNOACTIVATE);
            UpdateWindow(hWnd);
            g_monitorStates[monitorIndex].hScreenSaverWnd = hWnd;
            g_monitorStates[monitorIndex].screenSaverActive = 1;
            LogMessage("Screen saver window created on monitor %d", monitorIndex);
        }
    }

    if (!g_app.config.perMonitorInputDetection) {
        HideCursorForScreenSaver("screen saver activation");
    }
}

void ShowScreenSaver(int isManual) {
    if (g_app.screenSaverActive && !g_app.config.perMonitorInputDetection) return;

    if (g_app.config.perMonitorInputDetection && !isManual) {
        return;
    }

    if (isManual) {
        g_app.isManualActivation = 1;
        g_app.manualActivationTime = GetTickCount();
        LogMessage("Showing screen saver (manual activation)");
    }

    if (!g_app.config.perMonitorInputDetection) {
        int sentEscapeKeys = 0;
        for (int attempt = 0; attempt < SHELL_CLOSE_MAX_ATTEMPTS; attempt++) {
            int shellWindowCount = IsShellWindowOpen();
            if (shellWindowCount > 0) {
                LogMessage("Shell window(s) detected before screen saver activation (attempt %d), closing them", attempt + 1);
                CloseShellWindows(1);
                Sleep(SHELL_CLOSE_DELAY_MS);
                sentEscapeKeys = 1;
            } else {
                break;
            }
        }

        if (sentEscapeKeys && !isManual) {
            g_app.isManualActivation = 1;
            g_app.manualActivationTime = GetTickCount();
            LogMessage("Showing screen saver (automatic activation, with input cooldown due to shell window closure)");
        } else if (!isManual) {
            g_app.isManualActivation = 0;
            g_app.manualActivationTime = 0;
            LogMessage("Showing screen saver (automatic activation)");
        }
    }

    LogMessage("%d monitors detected", g_monitorCount);

    int windowsCreated = 0;
    time_t now = time(NULL);

    for (int i = 0; i < g_monitorCount; i++) {
        if (g_monitorStates[i].enabled && !g_monitorStates[i].screenSaverActive) {
            if (g_app.config.perMonitorInputDetection) {
                g_monitorStates[i].lastInputTime = now;
            }
            ShowScreenSaverOnMonitor(i, isManual);
            windowsCreated++;
        }
    }

    LogMessage("Activated screen saver on %d monitors", windowsCreated);

    g_app.screenSaverActive = 1;
}

void HideScreenSaver() {
    if (!g_app.screenSaverActive && !IsAnyMonitorActive()) return;

    LogMessage("Hiding screen saver");

    for (int i = 0; i < g_monitorCount; i++) {
        if (g_monitorStates[i].screenSaverActive) {
            HideScreenSaverOnMonitor(i);
        }
    }

    g_app.screenSaverActive = 0;
    g_app.manualActivationTime = 0;
    g_app.isManualActivation = 0;

    EnsureCursorVisible("screen saver hidden");
}

// Returns 1 if hWnd is a taskbar/shell overlay window (the taskbar itself,
// thumbnail previews shown when hovering over taskbar icons, flyouts). These
// windows sit above the screen saver while the user hovers over the taskbar,
// and reasserting topmost would cover the hovered preview, so they are left
// alone by EnsureScreenSaverTopmost.
int IsShellOverlayWindow(HWND hWnd) {
    if (!hWnd) return 0;
    char className[256] = {0};
    GetClassNameA(hWnd, className, sizeof(className));
    return strstr(className, "Shell_TrayWnd") != NULL ||
           strstr(className, "Shell_SecondaryTrayWnd") != NULL ||
           strstr(className, "TaskListThumbnailWnd") != NULL ||
           strstr(className, "XamlExplorerHostIslandWindow") != NULL;
}

void EnsureScreenSaverTopmost() {
    for (int i = 0; i < g_monitorCount; i++) {
        if (g_monitorStates[i].screenSaverActive && g_monitorStates[i].hScreenSaverWnd) {
            HWND hAbove = GetWindow(g_monitorStates[i].hScreenSaverWnd, GW_HWNDPREV);
            if (hAbove && IsShellOverlayWindow(hAbove)) {
                continue;
            }
            SetWindowPos(g_monitorStates[i].hScreenSaverWnd, HWND_TOPMOST,
                        0, 0, 0, 0,
                        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    }
}

// Backstop for external interference with the screen saver windows (e.g. the
// shell hiding or minimizing them while showing taskbar thumbnail previews).
// The WM_WINDOWPOSCHANGING/WM_STYLECHANGING vetoes in MonitorWindowProc block
// most attempts; this check catches anything that slipped through (DWM
// cloaking, moves, etc.), restores the window, and logs what happened so the
// cause can be identified.
void VerifyScreenSaverWindows() {
    for (int i = 0; i < g_monitorCount; i++) {
        HWND hWnd = g_monitorStates[i].hScreenSaverWnd;
        if (!g_monitorStates[i].screenSaverActive || !hWnd) continue;

        if (!IsWindow(hWnd)) {
            LogMessage("Watchdog: screen saver window on monitor %d was destroyed externally, recreating", i);
            g_monitorStates[i].hScreenSaverWnd = NULL;
            g_monitorStates[i].screenSaverActive = 0;
            ShowScreenSaverOnMonitor(i, 0);
            continue;
        }

        DWORD style = (DWORD)GetWindowLongPtrW(hWnd, GWL_STYLE);
        int wasMinimized = IsIconic(hWnd) || (style & WS_MINIMIZE) != 0;
        int wasHidden = !IsWindowVisible(hWnd);

        DWORD cloaked = 0;
        int isCloaked = SUCCEEDED(DwmGetWindowAttribute(hWnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0;

        if (wasMinimized || wasHidden || isCloaked) {
            LogMessage("Watchdog: screen saver window on monitor %d was externally %s (minimized=%d hidden=%d cloaked=%d style=0x%08X), restoring",
                       i, wasMinimized ? "minimized" : wasHidden ? "hidden" : "cloaked",
                       wasMinimized, wasHidden, isCloaked, style);
            ShowWindow(hWnd, SW_SHOWNOACTIVATE);
            SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }

        // Also guard against the window being moved off its monitor (another
        // way the black screen can "vanish" without a state change).
        RECT wrect;
        if (GetWindowRect(hWnd, &wrect)) {
            int pad = g_app.config.pixelShiftCompensation;
            int expLeft = g_monitors[i].rect.left - pad;
            int expTop = g_monitors[i].rect.top - pad;
            int expRight = g_monitors[i].rect.right + pad;
            int expBottom = g_monitors[i].rect.bottom + pad;
            if (wrect.left != expLeft || wrect.top != expTop ||
                wrect.right != expRight || wrect.bottom != expBottom) {
                LogMessage("Watchdog: screen saver window on monitor %d was moved (rect=%d,%d,%d,%d vs %d,%d,%d,%d), restoring",
                           i, wrect.left, wrect.top, wrect.right, wrect.bottom,
                           expLeft, expTop, expRight, expBottom);
                SetWindowPos(hWnd, HWND_TOPMOST,
                             expLeft, expTop, expRight - expLeft, expBottom - expTop,
                             SWP_NOACTIVATE);
            }
        }

        // Log z-order changes: a new window appearing directly above the
        // screen saver (e.g. the taskbar thumbnail preview) is the prime
        // suspect when the black screen "vanishes" without any state change.
        static HWND lastAboveWindow[MAX_MONITOR_COUNT] = {0};
        HWND hAbove = GetWindow(hWnd, GW_HWNDPREV);
        if (hAbove != lastAboveWindow[i]) {
            lastAboveWindow[i] = hAbove;
            if (hAbove) {
                char className[256] = {0};
                char procName[MAX_PATH] = {0};
                GetClassNameA(hAbove, className, sizeof(className));
                GetProcessNameFromHwnd(hAbove, procName, sizeof(procName));
                RECT aboveRect;
                GetWindowRect(hAbove, &aboveRect);
                LogMessage("Watchdog: window above screen saver on monitor %d changed: class='%s' proc='%s' rect=%d,%d,%d,%d",
                           i, className, procName,
                           aboveRect.left, aboveRect.top, aboveRect.right, aboveRect.bottom);
            } else {
                LogMessage("Watchdog: window above screen saver on monitor %d is now none", i);
            }
        }

        // Pixel probe: the screen saver paints solid black, so sampling the
        // center of the monitor tells us whether the user can actually SEE
        // the black screen. This catches DWM-level hiding (Aero Peek, etc.)
        // that no window flag can detect. Throttled to every ~2 seconds.
        static DWORD lastProbeTick = 0;
        DWORD nowTick = GetTickCount();
        if ((DWORD)(nowTick - lastProbeTick) >= 2000) {
            lastProbeTick = nowTick;

            // Skip the probe while a fullscreen window is in the foreground
            // (e.g. a game on another monitor): GDI screen capture during
            // fullscreen-optimized rendering can cause a brief hitch, and a
            // game legitimately showing non-black content would trigger a
            // pointless restore attempt over it.
            HWND hFg = GetForegroundWindow();
            int fullscreenForeground = 0;
            if (hFg) {
                RECT fr;
                if (GetWindowRect(hFg, &fr)) {
                    for (int m = 0; m < g_monitorCount; m++) {
                        RECT mr = g_monitors[m].rect;
                        if (fr.left <= mr.left && fr.top <= mr.top &&
                            fr.right >= mr.right && fr.bottom >= mr.bottom) {
                            fullscreenForeground = 1;
                            break;
                        }
                    }
                }
            }

            static int probeFailed[MAX_MONITOR_COUNT] = {0};
            int screenShowsBlack = 1;
            if (!fullscreenForeground) {
                HDC hdcScreen = GetDC(NULL);
                if (hdcScreen) {
                    HDC hdcMem = CreateCompatibleDC(hdcScreen);
                    HBITMAP hbm = CreateCompatibleBitmap(hdcScreen, 64, 64);
                    if (hdcMem && hbm) {
                        HGDIOBJ hOld = SelectObject(hdcMem, hbm);
                        RECT mr = g_monitors[i].rect;
                        int cx = mr.left + (mr.right - mr.left) / 2 - 32;
                        int cy = mr.top + (mr.bottom - mr.top) / 2 - 32;
                        if (BitBlt(hdcMem, 0, 0, 64, 64, hdcScreen, cx, cy, SRCCOPY)) {
                            POINT probes[6] = { {8, 8}, {56, 8}, {8, 56}, {56, 56}, {32, 32}, {32, 8} };
                            for (int p = 0; p < 6; p++) {
                                COLORREF c = GetPixel(hdcMem, probes[p].x, probes[p].y);
                                if (c != CLR_INVALID &&
                                    (GetRValue(c) > 12 || GetGValue(c) > 12 || GetBValue(c) > 12)) {
                                    screenShowsBlack = 0;
                                    break;
                                }
                            }
                        }
                        SelectObject(hdcMem, hOld);
                    }
                    if (hbm) DeleteObject(hbm);
                    if (hdcMem) DeleteDC(hdcMem);
                    ReleaseDC(NULL, hdcScreen);
                }
            }

            if (!screenShowsBlack) {
                if (!probeFailed[i]) {
                    probeFailed[i] = 1;
                    HWND above = GetWindow(hWnd, GW_HWNDPREV);
                    char className[256] = {0};
                    char procName[MAX_PATH] = {0};
                    if (above) {
                        GetClassNameA(above, className, sizeof(className));
                        GetProcessNameFromHwnd(above, procName, sizeof(procName));
                    }
                    LogMessage("Watchdog: monitor %d screen is NOT black while screen saver active (above: class='%s' proc='%s'), attempting restore",
                               i, className, procName);
                    // Don't force the screen saver above shell overlays (start
                    // menu, previews, flyouts): those are legitimate. Only
                    // reassert over plain windows.
                    if (!(above && IsShellOverlayWindow(above))) {
                        SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, 0, 0,
                                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                    }
                }
            } else {
                probeFailed[i] = 0;
            }
        }
    }
}

