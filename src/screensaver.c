// screensaver.c - Black screen saver windows: show/hide, hide/minimize vetoes, topmost reassertion, watchdog, shell-window (Start Menu etc.) handling.

#include "oled_aegis.h"

// --- Fade-to-black support ---
// fadeDurationMs > 0: layered windows, alpha animated on TIMER_FADE; click-through while fading.

// Current alpha of a monitor's window, computed from its in-flight fade (used to reverse a fade instead of flashing). 255 when not fading.
static BYTE GetMonitorCurrentAlpha(int monitorIndex)
{
    MonitorState* st = &g_monitorStates[monitorIndex];
    if (!st->fadeActive) return 255;
    DWORD elapsed = GetTickCount() - st->fadeStartTick;
    float t = (elapsed >= st->fadeDurationMs) ? 1.0f : (float)elapsed / (float)st->fadeDurationMs;
    t = t * t * (3.0f - 2.0f * t);  // smoothstep, matches UpdateFades
    int alpha = (int)(st->fadeFromAlpha + (st->fadeToAlpha - st->fadeFromAlpha) * t + 0.5f);
    if (alpha < 0) alpha = 0;
    if (alpha > 255) alpha = 255;
    return (BYTE)alpha;
}

// Toggle click-through: layered windows are hit-tested over their whole rect even at alpha 0, so WS_EX_TRANSPARENT is required while fading.
static void SetMonitorWindowClickThrough(HWND hWnd, BOOL clickThrough)
{
    LONG_PTR exStyle = GetWindowLongPtrW(hWnd, GWL_EXSTYLE);
    LONG_PTR newStyle = clickThrough ? (exStyle | WS_EX_TRANSPARENT)
                                     : (exStyle & ~WS_EX_TRANSPARENT);
    if (newStyle != exStyle)
    {
        SetWindowLongPtrW(hWnd, GWL_EXSTYLE, newStyle);
    }
}

// Start (or restart) an alpha animation on one monitor and make sure the fade timer is running.
static void StartMonitorFade(int monitorIndex, BYTE fromAlpha, BYTE toAlpha)
{
    MonitorState* st = &g_monitorStates[monitorIndex];
    st->fadeFromAlpha = fromAlpha;
    st->fadeToAlpha = toAlpha;
    st->fadeStartTick = GetTickCount();
    st->fadeDurationMs = (DWORD)g_app.config.fadeDurationMs;
    st->fadeActive = 1;
    if (st->hScreenSaverWnd)
    {
        SetMonitorWindowClickThrough(st->hScreenSaverWnd, TRUE);
    }
    SetTimer(g_app.hWnd, TIMER_FADE, FADE_TIMER_INTERVAL_MS, NULL);
}

// TIMER_FADE handler: step every fading window's alpha toward its target, finalize on completion (hide after fade-out), kill timer when idle.
void UpdateFades()
{
    int anyFading = 0;
    for (int i = 0; i < g_monitorCount; i++)
    {
        MonitorState* st = &g_monitorStates[i];
        if (!st->fadeActive || !st->hScreenSaverWnd)
        {
            st->fadeActive = 0;  // Safety net: window destroyed mid-fade
            continue;
        }
        anyFading = 1;

        DWORD elapsed = GetTickCount() - st->fadeStartTick;
        float t = (elapsed >= st->fadeDurationMs) ? 1.0f : (float)elapsed / (float)st->fadeDurationMs;
        t = t * t * (3.0f - 2.0f * t);  // smoothstep: gentle start/end, no harsh pop
        int alpha = (int)(st->fadeFromAlpha + (st->fadeToAlpha - st->fadeFromAlpha) * t + 0.5f);
        if (alpha < 0) alpha = 0;
        if (alpha > 255) alpha = 255;

        SetLayeredWindowAttributes(st->hScreenSaverWnd, 0, (BYTE)alpha, LWA_ALPHA);

        if (t >= 1.0f)
        {
            st->fadeActive = 0;
            if (st->fadeToAlpha == 0)
            {
                // Fade-out complete: hide the window and reset alpha so a later show starts opaque.
                ShowWindow(st->hScreenSaverWnd, SW_HIDE);
                SetLayeredWindowAttributes(st->hScreenSaverWnd, 0, 255, LWA_ALPHA);
                SetMonitorWindowClickThrough(st->hScreenSaverWnd, FALSE);
                LogMessage("Fade-out complete on monitor %d", i);
            }
            else
            {
                // Fade-in complete: window fully opaque, so it must capture clicks again (to dismiss the screen saver).
                SetMonitorWindowClickThrough(st->hScreenSaverWnd, FALSE);
                LogMessage("Fade-in complete on monitor %d", i);
            }
        }
    }

    if (!anyFading)
    {
        KillTimer(g_app.hWnd, TIMER_FADE);
    }
}

LRESULT CALLBACK MonitorWindowProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    static DWORD ignoreInputUntil = 0;

    switch (message)
    {
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
            // The shell hides windows for thumbnail previews; veto hide attempts while active so the saver can't vanish unnoticed.
            WINDOWPOS* wpos = (WINDOWPOS*)lParam;
            int isActive = 0;
            for (int i = 0; i < g_monitorCount; i++)
            {
                if (g_monitorStates[i].hScreenSaverWnd == hWnd && g_monitorStates[i].screenSaverActive)
                {
                    isActive = 1;
                    break;
                }
            }
            if (isActive && (wpos->flags & SWP_HIDEWINDOW))
            {
                LogMessage("Blocked external hide of monitor window (flags=0x%08X)", (DWORD)wpos->flags);
                wpos->flags &= ~SWP_HIDEWINDOW;
            }
            return 0;
        }
        case WM_STYLECHANGING:
        {
            // Same protection for minimize attempts (SW_MINIMIZE applies WS_MINIMIZE via a style change).
            if (wParam == GWL_STYLE)
            {
                STYLESTRUCT* ss = (STYLESTRUCT*)lParam;
                int isActive = 0;
                for (int i = 0; i < g_monitorCount; i++)
                {
                    if (g_monitorStates[i].hScreenSaverWnd == hWnd && g_monitorStates[i].screenSaverActive)
                    {
                        isActive = 1;
                        break;
                    }
                }
                if (isActive && (ss->styleNew & WS_MINIMIZE))
                {
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
            if (GetTickCount() < ignoreInputUntil)
            {
                break;
            }
            LogMessage("Input detected on monitor window (msg: %u)", message);
            if (g_app.config.perMonitorInputDetection)
            {
                for (int i = 0; i < g_monitorCount; i++)
                {
                    if (g_monitorStates[i].hScreenSaverWnd == hWnd)
                    {
                        HideScreenSaverOnMonitor(i);
                        break;
                    }
                }
                if (!IsAnyMonitorActive())
                {
                    g_app.screenSaverActive = 0;
                }
                UpdateTrayIcon(IsAnyMonitorActive() ? 1 : 0);
            }
            else
            {
                HideScreenSaver();
                UpdateTrayIcon(0);
            }
            break;
        default:
            return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

void HideScreenSaverOnMonitor(int monitorIndex)
{
    if (monitorIndex < 0 || monitorIndex >= g_monitorCount) return;

    // Clear active state BEFORE hiding: while active, MonitorWindowProc vetoes hide/minimize, and our own SW_HIDE must not be blocked.
    g_monitorStates[monitorIndex].screenSaverActive = 0;

    HWND hWnd = g_monitorStates[monitorIndex].hScreenSaverWnd;

    // Fade-out: animate alpha to transparent, hide on completion in UpdateFades, so the black screen recedes smoothly.
    if (g_app.config.fadeDurationMs > 0 && hWnd && IsWindowVisible(hWnd))
    {
        if (g_monitorStates[monitorIndex].fadeActive)
        {
            if (g_monitorStates[monitorIndex].fadeToAlpha == 255)
            {
                // Fade-in in progress: reverse it from the current alpha instead of abruptly hiding.
                StartMonitorFade(monitorIndex, GetMonitorCurrentAlpha(monitorIndex), 0);
            }
            // Already fading out: leave the running fade alone.
        }
        else
        {
            LONG_PTR exStyle = GetWindowLongPtrW(hWnd, GWL_EXSTYLE);
            if ((exStyle & WS_EX_LAYERED) == 0)
            {
                SetWindowLongPtrW(hWnd, GWL_EXSTYLE, exStyle | WS_EX_LAYERED);
            }
            SetLayeredWindowAttributes(hWnd, 0, 255, LWA_ALPHA);
            StartMonitorFade(monitorIndex, 255, 0);
            LogMessage("Screen saver window fading out on monitor %d", monitorIndex);
        }
        g_monitorStates[monitorIndex].lastInputTime = time(NULL);
        return;
    }

    if (hWnd)
    {
        ShowWindow(hWnd, SW_HIDE);
        LogMessage("Screen saver window hidden on monitor %d", monitorIndex);
    }

    g_monitorStates[monitorIndex].lastInputTime = time(NULL);
}

int IsShellWindowOpen()
{
    HWND hFg = GetForegroundWindow();
    if (!hFg)
    {
        LogMessage("Shell detection: No foreground window");
        return 0;
    }

    char processName[MAX_PATH] = {0};
    if (!GetProcessNameFromHwnd(hFg, processName, sizeof(processName)))
    {
        LogMessage("Shell detection: Could not get process name for foreground window");
        return 0;
    }

    char className[256] = {0};
    GetClassNameA(hFg, className, sizeof(className));

    LogMessage("Shell detection: Foreground window - process='%s', class='%s'", processName, className);

    int shellWindowCount = 0;

    // Shell host processes: ShellExperienceHost.exe (Start Menu, Action Center), SearchHost.exe, StartMenuExperienceHost.exe, ShellHost.exe.
    if (_stricmp(processName, "ShellExperienceHost.exe") == 0 ||
        _stricmp(processName, "SearchHost.exe") == 0 ||
        _stricmp(processName, "StartMenuExperienceHost.exe") == 0 ||
        _stricmp(processName, "ShellHost.exe") == 0)
    {

        LogMessage("Shell detection: Shell host process detected: %s", processName);
        shellWindowCount = 1;
    }

    // Task View lives in explorer.exe with these window classes:
    if (_stricmp(processName, "explorer.exe") == 0)
    {
        if (strstr(className, "Windows.UI.Core.CoreWindow") != NULL ||
            strstr(className, "XamlExplorerHostIslandWindow") != NULL)
        {

            LogMessage("Shell detection: Explorer shell window detected: class=%s", className);
            shellWindowCount = 1;
        }
    }

    if (shellWindowCount == 0)
    {
        LogMessage("Shell detection: No shell windows detected");
    }

    return shellWindowCount;
}

// Send Escape key(s) to close shell windows (Start Menu, Task View, Action Center)
void CloseShellWindows(int escapeCount)
{
    LogMessage("Sending %d Escape key(s) to close shell windows", escapeCount);

    for (int i = 0; i < escapeCount; i++)
    {
        INPUT input[2] = {0};

        input[0].type = INPUT_KEYBOARD;
        input[0].ki.wVk = VK_ESCAPE;
        input[0].ki.dwFlags = 0;

        input[1].type = INPUT_KEYBOARD;
        input[1].ki.wVk = VK_ESCAPE;
        input[1].ki.dwFlags = KEYEVENTF_KEYUP;

        UINT sent = SendInput(2, input, sizeof(INPUT));
        LogMessage("SendInput returned %u (expected 2)", sent);

        if (i < escapeCount - 1)
        {
            Sleep(50);
        }
    }
}

void ShowScreenSaverOnMonitor(int monitorIndex, int isManual)
{
    if (monitorIndex < 0 || monitorIndex >= g_monitorCount) return;
    if (!g_monitorStates[monitorIndex].enabled) return;
    if (g_monitorStates[monitorIndex].screenSaverActive) return;

    if (g_app.config.perMonitorInputDetection)
    {
        int wasInactiveCount = 0;
        for (int i = 0; i < g_monitorCount; i++)
        {
            if (g_monitorStates[i].enabled && !g_monitorStates[i].screenSaverActive)
            {
                wasInactiveCount++;
            }
        }

        if (wasInactiveCount == 1)
        {
            int sentEscapeKeys = 0;
            for (int attempt = 0; attempt < SHELL_CLOSE_MAX_ATTEMPTS; attempt++)
            {
                int shellWindowCount = IsShellWindowOpen();
                if (shellWindowCount > 0)
                {
                    LogMessage("Shell window(s) detected before last monitor activation (attempt %d), closing them", attempt + 1);
                    CloseShellWindows(1);
                    Sleep(SHELL_CLOSE_DELAY_MS);
                    sentEscapeKeys = 1;
                }
                else
                {
                    break;
                }
            }

            if (sentEscapeKeys)
            {
                // SendInput Escapes update GetLastInputInfo (would deactivate next tick); suppress via the manual-activation cooldown.
                g_app.isManualActivation = 1;
                g_app.manualActivationTime = GetTickCount();
            }
        }
    }
    else
    {
        int sentEscapeKeys = 0;
        for (int attempt = 0; attempt < SHELL_CLOSE_MAX_ATTEMPTS; attempt++)
        {
            int shellWindowCount = IsShellWindowOpen();
            if (shellWindowCount > 0)
            {
                LogMessage("Shell window(s) detected before screen saver activation (attempt %d), closing them", attempt + 1);
                CloseShellWindows(1);
                Sleep(SHELL_CLOSE_DELAY_MS);
                sentEscapeKeys = 1;
            }
            else
            {
                break;
            }
        }

        if (sentEscapeKeys)
        {
            g_app.isManualActivation = 1;
            g_app.manualActivationTime = GetTickCount();
        }
    }

    if (g_monitorStates[monitorIndex].hScreenSaverWnd)
    {
        // Reposition/resize in case the pixel shift compensation changed since creation.
        LONG_PTR exStyle = GetWindowLongPtrW(g_monitorStates[monitorIndex].hScreenSaverWnd, GWL_EXSTYLE);
        if ((exStyle & WS_EX_NOACTIVATE) == 0)
        {
            SetWindowLongPtrW(g_monitorStates[monitorIndex].hScreenSaverWnd, GWL_EXSTYLE, exStyle | WS_EX_NOACTIVATE);
        }
        // Layer + fully transparent before showing so no solid-black frame flashes; skipped when a fade is already running (reversed below).
        if (g_app.config.fadeDurationMs > 0 && !g_monitorStates[monitorIndex].fadeActive)
        {
            // Re-fetch the style: the NOACTIVATE fix-up above may have just changed it; don't drop it.
            exStyle = GetWindowLongPtrW(g_monitorStates[monitorIndex].hScreenSaverWnd, GWL_EXSTYLE);
            if ((exStyle & WS_EX_LAYERED) == 0)
            {
                SetWindowLongPtrW(g_monitorStates[monitorIndex].hScreenSaverWnd, GWL_EXSTYLE, exStyle | WS_EX_LAYERED);
            }
            SetLayeredWindowAttributes(g_monitorStates[monitorIndex].hScreenSaverWnd, 0, 0, LWA_ALPHA);
        }
        // Never let Aero Peek (taskbar thumbnail previews) make the black screen transparent/invisible.
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
    }
    else
    {
        // Expand past the monitor bounds by pixelShiftCompensation so hardware pixel shift can't expose the desktop behind the saver.
        int pad = g_app.config.pixelShiftCompensation;
        // Layered from birth so fade-in starts transparent; alpha set before first show so no solid-black frame can flash.
        LONG_PTR createExStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
        if (g_app.config.fadeDurationMs > 0)
        {
            createExStyle |= WS_EX_LAYERED;
        }
        HWND hWnd = CreateWindowExW(createExStyle,
                                    L"OLEDAegisScreen", L"",
                                   WS_POPUP,
                                   g_monitors[monitorIndex].rect.left   - pad,
                                   g_monitors[monitorIndex].rect.top    - pad,
                                   g_monitors[monitorIndex].rect.right  - g_monitors[monitorIndex].rect.left + pad * 2,
                                   g_monitors[monitorIndex].rect.bottom - g_monitors[monitorIndex].rect.top  + pad * 2,
                                   NULL, NULL, GetModuleHandle(NULL), NULL);

        if (hWnd)
        {
            // Never let Aero Peek (taskbar thumbnail previews) make the black screen transparent/invisible.
            BOOL excludeFromPeek = TRUE;
            DwmSetWindowAttribute(hWnd, DWMWA_EXCLUDED_FROM_PEEK, &excludeFromPeek, sizeof(excludeFromPeek));

            if (g_app.config.fadeDurationMs > 0)
            {
                SetLayeredWindowAttributes(hWnd, 0, 0, LWA_ALPHA);
            }

            ShowWindow(hWnd, SW_SHOWNOACTIVATE);
            UpdateWindow(hWnd);
            g_monitorStates[monitorIndex].hScreenSaverWnd = hWnd;
            g_monitorStates[monitorIndex].screenSaverActive = 1;
            LogMessage("Screen saver window created on monitor %d", monitorIndex);
        }
    }

    // Start the alpha animation; reverse an in-flight fade from its current alpha instead of flashing.
    if (g_app.config.fadeDurationMs > 0 && g_monitorStates[monitorIndex].hScreenSaverWnd)
    {
        if (g_monitorStates[monitorIndex].fadeActive)
        {
            StartMonitorFade(monitorIndex, GetMonitorCurrentAlpha(monitorIndex), 255);
        }
        else
        {
            StartMonitorFade(monitorIndex, 0, 255);
        }
    }

    if (!g_app.config.perMonitorInputDetection)
    {
        HideCursorForScreenSaver("screen saver activation");
    }
}

void ShowScreenSaver(int isManual)
{
    if (g_app.screenSaverActive && !g_app.config.perMonitorInputDetection) return;

    if (g_app.config.perMonitorInputDetection && !isManual)
    {
        return;
    }

    if (isManual)
    {
        g_app.isManualActivation = 1;
        g_app.manualActivationTime = GetTickCount();
        LogMessage("Showing screen saver (manual activation)");
    }

    if (!g_app.config.perMonitorInputDetection)
    {
        int sentEscapeKeys = 0;
        for (int attempt = 0; attempt < SHELL_CLOSE_MAX_ATTEMPTS; attempt++)
        {
            int shellWindowCount = IsShellWindowOpen();
            if (shellWindowCount > 0)
            {
                LogMessage("Shell window(s) detected before screen saver activation (attempt %d), closing them", attempt + 1);
                CloseShellWindows(1);
                Sleep(SHELL_CLOSE_DELAY_MS);
                sentEscapeKeys = 1;
            }
            else
            {
                break;
            }
        }

        if (sentEscapeKeys && !isManual)
        {
            g_app.isManualActivation = 1;
            g_app.manualActivationTime = GetTickCount();
            LogMessage("Showing screen saver (automatic activation, with input cooldown due to shell window closure)");
        }
        else if (!isManual)
        {
            g_app.isManualActivation = 0;
            g_app.manualActivationTime = 0;
            LogMessage("Showing screen saver (automatic activation)");
        }
    }

    LogMessage("%d monitors detected", g_monitorCount);

    int windowsCreated = 0;
    time_t now = time(NULL);

    for (int i = 0; i < g_monitorCount; i++)
    {
        if (g_monitorStates[i].enabled && !g_monitorStates[i].screenSaverActive)
        {
            if (g_app.config.perMonitorInputDetection)
            {
                g_monitorStates[i].lastInputTime = now;
            }
            ShowScreenSaverOnMonitor(i, isManual);
            windowsCreated++;
        }
    }

    LogMessage("Activated screen saver on %d monitors", windowsCreated);

    g_app.screenSaverActive = 1;
}

void HideScreenSaver()
{
    if (!g_app.screenSaverActive && !IsAnyMonitorActive()) return;

    LogMessage("Hiding screen saver");

    for (int i = 0; i < g_monitorCount; i++)
    {
        if (g_monitorStates[i].screenSaverActive)
        {
            HideScreenSaverOnMonitor(i);
        }
    }

    g_app.screenSaverActive = 0;
    g_app.manualActivationTime = 0;
    g_app.isManualActivation = 0;

    EnsureCursorVisible("screen saver hidden");
}

// 1 if hWnd is a taskbar/shell overlay (taskbar, thumbnail previews, flyouts): they sit above the saver while hovered, so topmost reassertion skips them.
int IsShellOverlayWindow(HWND hWnd)
{
    if (!hWnd) return 0;
    char className[256] = {0};
    GetClassNameA(hWnd, className, sizeof(className));
    return strstr(className, "Shell_TrayWnd") != NULL ||
           strstr(className, "Shell_SecondaryTrayWnd") != NULL ||
           strstr(className, "TaskListThumbnailWnd") != NULL ||
           strstr(className, "XamlExplorerHostIslandWindow") != NULL;
}

void EnsureScreenSaverTopmost()
{
    for (int i = 0; i < g_monitorCount; i++)
    {
        if (g_monitorStates[i].screenSaverActive && g_monitorStates[i].hScreenSaverWnd)
        {
            HWND hAbove = GetWindow(g_monitorStates[i].hScreenSaverWnd, GW_HWNDPREV);
            if (hAbove && IsShellOverlayWindow(hAbove))
            {
                continue;
            }
            SetWindowPos(g_monitorStates[i].hScreenSaverWnd, HWND_TOPMOST,
                        0, 0, 0, 0,
                        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    }
}

// Backstop for external interference: the MonitorWindowProc vetoes block most; this catches what slipped through (DWM cloaking, moves), restores.
void VerifyScreenSaverWindows()
{
    for (int i = 0; i < g_monitorCount; i++)
    {
        HWND hWnd = g_monitorStates[i].hScreenSaverWnd;
        if (!g_monitorStates[i].screenSaverActive || !hWnd) continue;

        if (!IsWindow(hWnd))
        {
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

        if (wasMinimized || wasHidden || isCloaked)
        {
            LogMessage("Watchdog: screen saver window on monitor %d was externally %s (minimized=%d hidden=%d cloaked=%d style=0x%08X), restoring",
                       i, wasMinimized ? "minimized" : wasHidden ? "hidden" : "cloaked",
                       wasMinimized, wasHidden, isCloaked, style);
            ShowWindow(hWnd, SW_SHOWNOACTIVATE);
            SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }

        // Also guard against the window being moved off its monitor (another way the black screen can "vanish" without a state change).
        RECT wrect;
        if (GetWindowRect(hWnd, &wrect))
        {
            int pad = g_app.config.pixelShiftCompensation;
            int expLeft = g_monitors[i].rect.left - pad;
            int expTop = g_monitors[i].rect.top - pad;
            int expRight = g_monitors[i].rect.right + pad;
            int expBottom = g_monitors[i].rect.bottom + pad;
            if (wrect.left != expLeft || wrect.top != expTop ||
                wrect.right != expRight || wrect.bottom != expBottom)
            {
                LogMessage("Watchdog: screen saver window on monitor %d was moved (rect=%d,%d,%d,%d vs %d,%d,%d,%d), restoring",
                           i, wrect.left, wrect.top, wrect.right, wrect.bottom,
                           expLeft, expTop, expRight, expBottom);
                SetWindowPos(hWnd, HWND_TOPMOST,
                             expLeft, expTop, expRight - expLeft, expBottom - expTop,
                             SWP_NOACTIVATE);
            }
        }

        // Log z-order changes: a window above the saver (thumbnail preview) is the prime suspect when the black screen vanishes unnoticed.
        static HWND lastAboveWindow[MAX_MONITOR_COUNT] = {0};
        HWND hAbove = GetWindow(hWnd, GW_HWNDPREV);
        if (hAbove != lastAboveWindow[i])
        {
            lastAboveWindow[i] = hAbove;
            if (hAbove)
            {
                char className[256] = {0};
                char procName[MAX_PATH] = {0};
                GetClassNameA(hAbove, className, sizeof(className));
                GetProcessNameFromHwnd(hAbove, procName, sizeof(procName));
                RECT aboveRect;
                GetWindowRect(hAbove, &aboveRect);
                LogMessage("Watchdog: window above screen saver on monitor %d changed: class='%s' proc='%s' rect=%d,%d,%d,%d",
                           i, className, procName,
                           aboveRect.left, aboveRect.top, aboveRect.right, aboveRect.bottom);
            }
            else
            {
                LogMessage("Watchdog: window above screen saver on monitor %d is now none", i);
            }
        }

        // While a fade is in progress the screen is legitimately not fully black yet, so skip the pixel probe until the transition completes.
        if (g_monitorStates[i].fadeActive)
        {
            continue;
        }

        // Pixel probe: black paint at the monitor center proves the user sees black (catches Aero Peek); throttled to ~2 seconds.
        static DWORD lastProbeTick = 0;
        DWORD nowTick = GetTickCount();
        if ((DWORD)(nowTick - lastProbeTick) >= 2000)
        {
            lastProbeTick = nowTick;

            // Skip the probe under a fullscreen foreground window (GDI capture can hitch games; a game would trigger a pointless restore).
            HWND hFg = GetForegroundWindow();
            int fullscreenForeground = 0;
            if (hFg)
            {
                RECT fr;
                if (GetWindowRect(hFg, &fr))
                {
                    for (int m = 0; m < g_monitorCount; m++)
                    {
                        RECT mr = g_monitors[m].rect;
                        // Fraction of this monitor's area covered by the foreground window.
                        LONG il = fr.left   > mr.left   ? fr.left   : mr.left;
                        LONG it = fr.top    > mr.top    ? fr.top    : mr.top;
                        LONG ir = fr.right  < mr.right  ? fr.right  : mr.right;
                        LONG ib = fr.bottom < mr.bottom ? fr.bottom : mr.bottom;
                        if (ir <= il || ib <= it) continue;
                        LONGLONG covered = (LONGLONG)(ir - il) * (ib - it);
                        LONGLONG monArea  = (LONGLONG)(mr.right - mr.left) * (mr.bottom - mr.top);
                        if (monArea > 0 && covered * 100 >= monArea * FULLSCREEN_FOREGROUND_MIN_COVERAGE_PCT)
                        {
                            fullscreenForeground = 1;
                            break;
                        }
                    }
                }
            }

            static int probeFailed[MAX_MONITOR_COUNT] = {0};
            int screenShowsBlack = 1;
            if (!fullscreenForeground)
            {
                HDC hdcScreen = GetDC(NULL);
                if (hdcScreen)
                {
                    HDC hdcMem = CreateCompatibleDC(hdcScreen);
                    HBITMAP hbm = CreateCompatibleBitmap(hdcScreen, 64, 64);
                    if (hdcMem && hbm)
                    {
                        HGDIOBJ hOld = SelectObject(hdcMem, hbm);
                        RECT mr = g_monitors[i].rect;
                        int cx = mr.left + (mr.right - mr.left) / 2 - 32;
                        int cy = mr.top + (mr.bottom - mr.top) / 2 - 32;
                        if (BitBlt(hdcMem, 0, 0, 64, 64, hdcScreen, cx, cy, SRCCOPY))
                        {
                            POINT probes[6] = { {8, 8}, {56, 8}, {8, 56}, {56, 56}, {32, 32}, {32, 8} };
                            for (int p = 0; p < 6; p++)
                            {
                                COLORREF c = GetPixel(hdcMem, probes[p].x, probes[p].y);
                                if (c != CLR_INVALID &&
                                    (GetRValue(c) > 12 || GetGValue(c) > 12 || GetBValue(c) > 12))
                                {
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

            if (!screenShowsBlack)
            {
                if (!probeFailed[i])
                {
                    probeFailed[i] = 1;
                    HWND above = GetWindow(hWnd, GW_HWNDPREV);
                    char className[256] = {0};
                    char procName[MAX_PATH] = {0};
                    if (above)
                    {
                        GetClassNameA(above, className, sizeof(className));
                        GetProcessNameFromHwnd(above, procName, sizeof(procName));
                    }
                    LogMessage("Watchdog: monitor %d screen is NOT black while screen saver active (above: class='%s' proc='%s'), attempting restore",
                               i, className, procName);
                    // Don't reassert over legitimate shell overlays (start menu, previews, flyouts); only over plain windows.
                    if (!(above && IsShellOverlayWindow(above)))
                    {
                        SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, 0, 0,
                                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                    }
                }
            }
            else
            {
                probeFailed[i] = 0;
            }
        }
    }
}

