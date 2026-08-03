// oled_aegis.c - OLED Aegis entry point and main window: WinMain, WndProc,
// app initialization (HandleCreation), the idle-check timer state machine
// (HandleTimeout), and the tray icon. The other subsystems live in the
// sibling files listed in oled_aegis.h.

#include "oled_aegis.h"

AppState g_app;
static HANDLE g_hInstanceMutex = NULL;  // Single-instance mutex (kept for app lifetime)

HBRUSH g_blackBrush = NULL;
HICON g_hIconActive = NULL;
HICON g_hIconInactive = NULL;
static UINT g_uTaskbarRestart = 0;  // Registered "TaskbarCreated" message ID (0 if not registered)

// Idle-countdown "pause" bookkeeping: while media plays, the elapsed playback
// time is excluded from the idle calculation, so stopping playback does not
// immediately trigger the screen saver. Per-monitor offsets are used in mixed
// mode (global input + per-monitor media); the global offset is used in plain
// global mode. Both are cleared as soon as real user input is detected.
static DWORD g_mediaPauseOffsetMs = 0;
static DWORD g_lastTimerTickMs = 0;  // GetTickCount() of the previous timer tick

void LoadTrayIcons() {
    HINSTANCE hInstance = GetModuleHandle(NULL);

    // Load icons from embedded resources
    g_hIconActive = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_ICON_ACTIVE));
    g_hIconInactive = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_ICON_INACTIVE));

    // Fall back to system icons if resource icons not found
    if (!g_hIconActive) {
        g_hIconActive = LoadIcon(NULL, IDI_APPLICATION);
    }
    if (!g_hIconInactive) {
        g_hIconInactive = LoadIcon(NULL, IDI_INFORMATION);
    }
}

DWORD GetIdleTime() {
    LASTINPUTINFO lii;
    lii.cbSize = sizeof(LASTINPUTINFO);
    GetLastInputInfo(&lii);
    return GetTickCount() - lii.dwTime;
}

void UpdateTrayIcon(int active) {
    active = active ? 1 : 0;
    if (g_app.trayIconActive == active) {
        return;
    }

    g_app.nid.hIcon = active ? g_hIconActive : g_hIconInactive;
    lstrcpyA(g_app.nid.szTip, active ? "OLED Aegis - Active" : "OLED Aegis - Idle");
    Shell_NotifyIconA(NIM_MODIFY, (PNOTIFYICONDATAA)&g_app.nid);
    g_app.trayIconActive = active;
}

// Handle WM_CREATE: initialize application state, tray icon, config, monitors,
// and the idle-check timer. Returns 0 on success, -1 to abort window creation
// (used when another instance is already running).
int HandleCreation(HWND hWnd) {
    memset(&g_app, 0, sizeof(g_app));
    g_app.hWnd = hWnd;
    g_app.isShuttingDown = 0;
    g_app.trayIconActive = -1;  // Force first UpdateTrayIcon call to fire

    g_hInstanceMutex = CreateMutexW(NULL, TRUE, L"OLEDAegis_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(NULL, L"OLED Aegis is already running", L"OLED Aegis", MB_OK | MB_ICONINFORMATION);
        if (g_hInstanceMutex) {
            CloseHandle(g_hInstanceMutex);
            g_hInstanceMutex = NULL;
        }
        PostQuitMessage(0);
        return -1;
    }
    // Keep mutex handle open for app lifetime to maintain single-instance lock

    LoadTrayIcons();

    g_app.nid.cbSize = sizeof(NOTIFYICONDATAA);
    g_app.nid.hWnd = hWnd;
    g_app.nid.uID = 1;
    g_app.nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_app.nid.uCallbackMessage = WM_TRAYICON;
    g_app.nid.hIcon = g_hIconInactive;
    lstrcpyA(g_app.nid.szTip, "OLED Aegis - Idle");
    Shell_NotifyIconA(NIM_ADD, &g_app.nid);

    g_app.config.idleTimeout = DEFAULT_IDLE_TIMEOUT;
    g_app.config.checkInterval = 1000;
    g_app.config.mediaDetectionEnabled = 1;
    g_app.config.startupEnabled = 0;
    g_app.config.debugMode = 0;
    g_app.config.perMonitorInputDetection = 0;
            g_app.config.perMonitorMediaDetection = 1;
            g_app.config.blockOnMutedMedia = 0;
            g_app.config.fadeDurationMs = DEFAULT_FADE_DURATION_MS;
            for (int i = 0; i < MAX_MONITOR_COUNT; i++) {
        g_app.config.monitorsEnabled[i] = 1;
    }

    EnumerateMonitors();

    for (int i = 0; i < g_monitorCount; i++) {
        g_monitorStates[i].lastInputTime = time(NULL);
        g_monitorStates[i].screenSaverActive = 0;
        g_monitorStates[i].enabled = g_app.config.monitorsEnabled[i];
        g_monitorStates[i].mediaPauseOffsetMs = 0;
    }

    if (!ConfigFileExists()) {
        SaveConfig();
    }

    LoadConfig();

    for (int i = 0; i < g_monitorCount; i++) {
        g_monitorStates[i].enabled = g_app.config.monitorsEnabled[i];
    }

    UpdateStartupRegistry();

    LogMessage("Application started. Timeout: %ds, Media: %d, Debug: %d",
             g_app.config.idleTimeout, g_app.config.mediaDetectionEnabled, g_app.config.debugMode);

    g_blackBrush = CreateSolidBrush(RGB(0, 0, 0));

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = MonitorWindowProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.hbrBackground = g_blackBrush;
    wc.lpszClassName = L"OLEDAegisScreen";
    RegisterClassW(&wc);

    SetTimer(hWnd, TIMER_IDLE_CHECK, g_app.config.checkInterval, NULL);

    return 0;
}

void HandleTimeout(WPARAM wParam) {
    if (wParam != TIMER_IDLE_CHECK) {
        return;
    }

    // Skip all processing if no monitors have screen saver enabled
    if (!IsAnyMonitorEnabled()) {
        return;
    }

    // Media-pause bookkeeping (shared by all modes below):
    //   Track the time elapsed since the previous tick and the raw Windows
    //   idle time. Real user input restarts Windows' idle counter, so any
    //   accumulated media-pause offset no longer applies and is cleared.
    DWORD nowTick = GetTickCount();
    DWORD deltaSinceLastTick = (g_lastTimerTickMs != 0) ? (nowTick - g_lastTimerTickMs) : 0;
    g_lastTimerTickMs = nowTick;

    DWORD rawIdleTime = GetIdleTime();
    if (rawIdleTime < IDLE_ACTIVITY_THRESHOLD_MS) {
        g_mediaPauseOffsetMs = 0;
        for (int i = 0; i < g_monitorCount; i++) {
            g_monitorStates[i].mediaPauseOffsetMs = 0;
        }
    }

    // Per-monitor input detection mode:
    //   Each monitor has its own idle timer, updated by tracking cursor position
    //   and focused-window location. This lets the screen saver activate on
    //   unused monitors while the user continues working on others. Media is
    //   checked per-monitor (if perMonitorMediaDetection is on) or globally.
    //   Each monitor's screen saver is activated/deactivated independently.
    if (g_app.config.perMonitorInputDetection) {
        DWORD idleTime = rawIdleTime;
        time_t now = time(NULL);

        int usePerMonitorMedia = (g_app.config.perMonitorMediaDetection && g_app.config.mediaDetectionEnabled);
        int mediaOnMonitor[MAX_MONITOR_COUNT] = {0};
        int mediaPlaying = 0;
        if (usePerMonitorMedia) {
            UpdateMediaMonitorStates(mediaOnMonitor);
        } else {
            mediaPlaying = IsMediaPlaying();
        }

        int inManualCooldown = 0;
        if (g_app.isManualActivation) {
            DWORD timeSinceActivation = GetTickCount() - g_app.manualActivationTime;
            if (timeSinceActivation < MANUAL_ACTIVATION_COOLDOWN_MS) {
                inManualCooldown = 1;
            } else {
                g_app.isManualActivation = 0;
                g_app.manualActivationTime = 0;
            }
        }

        if (idleTime < IDLE_ACTIVITY_THRESHOLD_MS && !inManualCooldown) {
            POINT pt;
            GetCursorPos(&pt);
            int cursorMonitorIndex = GetMonitorIndexFromPoint(pt);

            if (cursorMonitorIndex >= 0 && cursorMonitorIndex < g_monitorCount) {
                g_monitorStates[cursorMonitorIndex].lastInputTime = now;
            }

            HWND hFg = GetForegroundWindow();
            if (hFg) {
                int isOledWindow = 0;
                for (int i = 0; i < g_monitorCount; i++) {
                    if (g_monitorStates[i].hScreenSaverWnd == hFg) {
                        isOledWindow = 1;
                        break;
                    }
                }

                if (!isOledWindow) {
                    RECT rect;
                    GetWindowRect(hFg, &rect);
                    int fgMonitorIndex = GetMonitorIndexFromRect(rect);
                    if (fgMonitorIndex >= 0 && fgMonitorIndex < g_monitorCount && fgMonitorIndex != cursorMonitorIndex) {
                        g_monitorStates[fgMonitorIndex].lastInputTime = now;
                    }
                }
            }
        }

        for (int i = 0; i < g_monitorCount; i++) {
            if (!g_monitorStates[i].enabled) continue;

            int monitorHasMedia = usePerMonitorMedia ? mediaOnMonitor[i] : mediaPlaying;

            if (monitorHasMedia) {
                // Pause this monitor's idle countdown while media plays on it:
                // the countdown resumes from (nearly) zero when playback stops
                // instead of firing the screen saver immediately.
                g_monitorStates[i].lastInputTime = now;
            }

            int idleSeconds = (int)(now - g_monitorStates[i].lastInputTime);

            if (!monitorHasMedia && idleSeconds >= g_app.config.idleTimeout) {
                if (!g_monitorStates[i].screenSaverActive) {
                    LogMessage("Timer: Activating screen saver on monitor %d (idle: %ds)", i, idleSeconds);
                    ShowScreenSaverOnMonitor(i, 0);
                }
            } else if (g_monitorStates[i].screenSaverActive && !inManualCooldown) {
                if (monitorHasMedia) {
                    LogMessage("Timer: Deactivating screen saver on monitor %d (media detected)", i);
                    HideScreenSaverOnMonitor(i);
                } else if (idleSeconds < IDLE_DEACTIVATE_THRESHOLD_SEC) {
                    LogMessage("Timer: Deactivating screen saver on monitor %d (input detected)", i);
                    HideScreenSaverOnMonitor(i);
                }
            }
        }

        if (!IsAnyMonitorActive()) {
            g_app.screenSaverActive = 0;
        }

        POINT cursorPt;
        GetCursorPos(&cursorPt);
        int cursorMonitorIndex = GetMonitorIndexFromPoint(cursorPt);
        int cursorOnActiveMonitor = (cursorMonitorIndex >= 0 && cursorMonitorIndex < g_monitorCount && g_monitorStates[cursorMonitorIndex].screenSaverActive);

        if (cursorOnActiveMonitor) {
            HideCursorForScreenSaver("cursor on active monitor");
        } else {
            if (g_app.cursorHidden) {
                EnsureCursorVisible("cursor left active monitor");
            }
        }

        UpdateTrayIcon(IsAnyMonitorActive() ? 1 : 0);
    } else {
        // Global input detection mode:
        //   A single idle timer (GetIdleTime) covers all monitors. When per-
        //   monitor media detection is on, each monitor is still activated/
        //   deactivated independently based on whether media is playing on it,
        //   but idle time is global. Without per-monitor media, the original
        //   all-on/all-off behavior is used.
        if (g_app.config.perMonitorMediaDetection && g_app.config.mediaDetectionEnabled) {
            // Per-monitor media with global input:
            //   Each monitor's idle countdown is "paused" while media plays on
            //   it (playback time is excluded from that monitor's idle
            //   calculation). When a monitor's paused-adjusted idle time is
            //   beyond the timeout, activate its screen saver if no media is
            //   playing there, or deactivate it if media is detected. Fresh
            //   input deactivates everything (preserving the manual-activation
            //   cooldown logic).
            int mediaOnMonitor[MAX_MONITOR_COUNT] = {0};
            UpdateMediaMonitorStates(mediaOnMonitor);

            // Pause the countdown on monitors with media: accumulate elapsed
            // time while media plays, clamped so the effective idle time can
            // never go negative.
            if (rawIdleTime >= IDLE_ACTIVITY_THRESHOLD_MS) {
                for (int i = 0; i < g_monitorCount; i++) {
                    if (!mediaOnMonitor[i]) continue;
                    g_monitorStates[i].mediaPauseOffsetMs += deltaSinceLastTick;
                    if (g_monitorStates[i].mediaPauseOffsetMs > rawIdleTime) {
                        g_monitorStates[i].mediaPauseOffsetMs = rawIdleTime;
                    }
                }
            }

            for (int i = 0; i < g_monitorCount; i++) {
                if (!g_monitorStates[i].enabled) continue;

                DWORD effectiveIdle = (rawIdleTime > g_monitorStates[i].mediaPauseOffsetMs)
                    ? (rawIdleTime - g_monitorStates[i].mediaPauseOffsetMs) : 0;

                if (effectiveIdle > (DWORD)(g_app.config.idleTimeout * 1000)) {
                    if (mediaOnMonitor[i]) {
                        if (g_monitorStates[i].screenSaverActive) {
                            LogMessage("Timer: Deactivating screen saver on monitor %d (media detected)", i);
                            HideScreenSaverOnMonitor(i);
                        }
                    } else if (!g_monitorStates[i].screenSaverActive) {
                        LogMessage("Timer: Activating screen saver on monitor %d (idle: %lums)", i, effectiveIdle);
                        ShowScreenSaverOnMonitor(i, 0);
                    }
                } else if (g_monitorStates[i].screenSaverActive) {
                    // Below the timeout: only fresh input can have brought the
                    // countdown down (input clears the pause offsets), so
                    // deactivate (preserving the manual-activation cooldown).
                    if (g_app.isManualActivation) {
                        DWORD timeSinceActivation = GetTickCount() - g_app.manualActivationTime;
                        if (timeSinceActivation < MANUAL_ACTIVATION_COOLDOWN_MS) {
                            LogMessage("Timer: Skipping deactivation (manual cooldown: %lums/%dms)",
                                     timeSinceActivation, MANUAL_ACTIVATION_COOLDOWN_MS);
                        } else if (effectiveIdle < IDLE_DEACTIVATE_THRESHOLD_MS) {
                            LogMessage("Timer: Deactivating screen saver on monitor %d (new input detected after cooldown)", i);
                            HideScreenSaverOnMonitor(i);
                        }
                    } else if (effectiveIdle < IDLE_DEACTIVATE_THRESHOLD_MS) {
                        LogMessage("Timer: Deactivating screen saver on monitor %d (idle: %lums)", i, effectiveIdle);
                        HideScreenSaverOnMonitor(i);
                    }
                }
            }

            g_app.screenSaverActive = IsAnyMonitorActive() ? 1 : 0;

            if (!g_app.screenSaverActive && g_app.cursorHidden) {
                EnsureCursorVisible("no active monitors");
            }

            UpdateTrayIcon(g_app.screenSaverActive);
        } else {
            // Original global behavior:
            //   When the paused-adjusted idle time is beyond the timeout and no
            //   media is playing, activate the screen saver on all enabled
            //   monitors at once. Media playback pauses the countdown, so
            //   stopping playback resumes it from where it left off instead of
            //   firing immediately. When the user is active or media starts
            //   playing, deactivate everything. Manual-activation cooldown
            //   logic is preserved.
            int mediaPlaying = IsMediaPlaying();

            // Pause the countdown while media plays: accumulate the elapsed
            // time, clamped so the effective idle time never goes negative.
            if (mediaPlaying && rawIdleTime >= IDLE_ACTIVITY_THRESHOLD_MS) {
                g_mediaPauseOffsetMs += deltaSinceLastTick;
                if (g_mediaPauseOffsetMs > rawIdleTime) {
                    g_mediaPauseOffsetMs = rawIdleTime;
                }
            }
            DWORD idleTime = (rawIdleTime > g_mediaPauseOffsetMs)
                ? (rawIdleTime - g_mediaPauseOffsetMs) : 0;

            if (!mediaPlaying && idleTime > (DWORD)(g_app.config.idleTimeout * 1000)) {
                if (!g_app.screenSaverActive) {
                    LogMessage("Timer: Activating screen saver (idle: %lums)", idleTime);
                    ShowScreenSaver(0);
                    UpdateTrayIcon(1);
                }
            } else {
                if (g_app.screenSaverActive) {
                    if (g_app.isManualActivation) {
                        DWORD timeSinceActivation = GetTickCount() - g_app.manualActivationTime;
                        if (timeSinceActivation < MANUAL_ACTIVATION_COOLDOWN_MS) {
                            LogMessage("Timer: Skipping deactivation (manual cooldown: %lums/%dms)",
                                     timeSinceActivation, MANUAL_ACTIVATION_COOLDOWN_MS);
                        } else {
                            if (idleTime < IDLE_DEACTIVATE_THRESHOLD_MS) {
                                LogMessage("Timer: Deactivating screen saver (new input detected after cooldown)");
                                HideScreenSaver();
                                UpdateTrayIcon(0);
                            }
                        }
                    } else {
                        LogMessage("Timer: Deactivating screen saver (idle: %lums, media: %d)", idleTime, mediaPlaying);
                        HideScreenSaver();
                        UpdateTrayIcon(0);
                    }
                }
            }
        }
    }

    // Ensure screen saver windows stay on top (handles notifications like MS
    // Teams, Steam friends, etc.), but throttle to avoid a SetWindowPos call
    // every timer tick.
    static DWORD lastTopmostRefresh = 0;
    if (IsAnyMonitorActive()) {
        DWORD nowTick = GetTickCount();
        if ((DWORD)(nowTick - lastTopmostRefresh) >= TOPMOST_REFRESH_INTERVAL_MS) {
            EnsureScreenSaverTopmost();
            lastTopmostRefresh = nowTick;
        }
    } else {
        lastTopmostRefresh = 0;
    }

    // Backstop: the shell can externally hide, minimize, or move the screen
    // saver windows (e.g. while showing taskbar thumbnail previews). Verify
    // and restore, logging anything that was externally altered.
    VerifyScreenSaverWindows();
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == g_uTaskbarRestart && g_uTaskbarRestart != 0 && g_app.nid.cbSize != 0) {
        LogMessage("Taskbar recreated (Explorer restart) - restoring tray icon");
        g_app.nid.hIcon = g_app.screenSaverActive ? g_hIconActive : g_hIconInactive;
        lstrcpyA(g_app.nid.szTip, g_app.screenSaverActive ? "OLED Aegis - Active" : "OLED Aegis - Idle");
        Shell_NotifyIconA(NIM_ADD, &g_app.nid);
        g_app.trayIconActive = g_app.screenSaverActive ? 1 : 0;
        return 0;
    }

    switch (message) {
        case WM_CREATE:
            return HandleCreation(hWnd);

        case WM_TIMER:
            if (wParam == TIMER_FADE) {
                UpdateFades();
                break;
            }
            HandleTimeout(wParam);
            break;

        case WM_POWERBROADCAST:
            if (wParam == PBT_APMRESUMESUSPEND || wParam == PBT_APMRESUMEAUTOMATIC) {
                LogMessage("System resumed from sleep - resetting media detection cache");
                ResetMediaDetectionCache();
            }
            break;

        case WM_DISPLAYCHANGE:
            LogMessage("Display configuration changed - re-enumerating monitors");

            // Destroy all screen saver windows first
            for (int i = 0; i < MAX_MONITOR_COUNT; i++) {
                if (g_monitorStates[i].hScreenSaverWnd) {
                    DestroyWindow(g_monitorStates[i].hScreenSaverWnd);
                    g_monitorStates[i].hScreenSaverWnd = NULL;
                }
                g_monitorStates[i].screenSaverActive = 0;
                g_monitorStates[i].fadeActive = 0;
            }
            g_app.screenSaverActive = 0;
            UpdateTrayIcon(0);

            EnsureCursorVisible("display configuration changed");

            // Re-enumerate monitors to detect added/removed displays
            int oldMonitorCount = g_monitorCount;
            EnumerateMonitors();

            // Load config to get per-device settings
            LoadConfig();

            // Reinitialize monitor states
            time_t now = time(NULL);
            for (int i = 0; i < g_monitorCount; i++) {
                g_monitorStates[i].lastInputTime = now;
                g_monitorStates[i].screenSaverActive = 0;
                g_monitorStates[i].fadeActive = 0;
                g_monitorStates[i].enabled = g_app.config.monitorsEnabled[i];
                g_monitorStates[i].hScreenSaverWnd = NULL;
                g_monitorStates[i].mediaPauseOffsetMs = 0;
            }

            // If settings dialog is open, close and reopen to refresh monitor list
            if (g_hSettingsDialog) {
                LogMessage("Refreshing settings dialog for new monitor configuration");
                DestroyWindow(g_hSettingsDialog);
                g_hSettingsDialog = NULL;
                ShowSettingsDialog();
            }

            LogMessage("Monitor configuration updated: %d -> %d monitors", oldMonitorCount, g_monitorCount);
            break;

        case WM_TRAYICON:
            if (lParam == WM_RBUTTONUP) {
                LogMessage("User: Right-clicked tray icon - opening context menu");
                POINT pt;
                GetCursorPos(&pt);
                SetForegroundWindow(hWnd);
                g_app.trayMenuActive = 1;
                EnsureCursorVisible("tray menu opened");

                HMENU hMenu = CreatePopupMenu();
                AppendMenuA(hMenu, MF_STRING, IDM_SETTINGS, "Settings...");
                AppendMenuA(hMenu, MF_SEPARATOR, 0, NULL);
                AppendMenuA(hMenu, MF_STRING, IDM_EXIT, "Exit");

                TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, NULL);
                DestroyMenu(hMenu);
                g_app.trayMenuActive = 0;
                EnsureCursorVisible("tray menu closed");
            } else if (lParam == WM_LBUTTONDOWN) {
                EnsureCursorVisible("tray icon clicked");
                if (g_hSettingsDialog) {
                    LogMessage("User: Left-clicked tray icon - settings dialog already open, bringing to foreground");
                    SetForegroundWindow(g_hSettingsDialog);
                } else {
                    if (g_app.screenSaverActive) {
                        LogMessage("User: Left-clicked tray icon - deactivating screen saver");
                        HideScreenSaver();
                        UpdateTrayIcon(0);
                    } else {
                        LogMessage("User: Left-clicked tray icon - activating screen saver (manual)");
                        ShowScreenSaver(1);
                        UpdateTrayIcon(1);
                    }
                }
            }
            break;

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDM_SETTINGS:
                    LogMessage("User: Selected 'Settings' from tray menu");
                    ShowSettingsDialog();
                    break;
                case IDM_EXIT:
                    LogMessage("User: Selected 'Exit' from tray menu - shutting down");
                    HideScreenSaver();
                    Shell_NotifyIconA(NIM_DELETE, &g_app.nid);
                    PostQuitMessage(0);
                    break;
            }
            break;

        case WM_DESTROY:
            LogMessage("Application shutting down");

            EnsureCursorVisible("shutdown");

            for (int i = 0; i < MAX_MONITOR_COUNT; i++) {
                if (g_monitorStates[i].hScreenSaverWnd) {
                    DestroyWindow(g_monitorStates[i].hScreenSaverWnd);
                    g_monitorStates[i].hScreenSaverWnd = NULL;
                }
                g_monitorStates[i].screenSaverActive = 0;
                g_monitorStates[i].fadeActive = 0;
            }

            Shell_NotifyIconA(NIM_DELETE, &g_app.nid);

            if (g_hSettingsDialog) {
                DestroyWindow(g_hSettingsDialog);
                g_hSettingsDialog = NULL;
            }

            if (g_logFile) {
                fclose(g_logFile);
                g_logFile = NULL;
            }

            if (g_blackBrush) {
                DeleteObject(g_blackBrush);
                g_blackBrush = NULL;
            }

            // Note: Icons loaded via LoadIcon from resources don't need DestroyIcon
            g_hIconActive = NULL;
            g_hIconInactive = NULL;

            if (g_hInstanceMutex) {
                CloseHandle(g_hInstanceMutex);
                g_hInstanceMutex = NULL;
            }

            PostQuitMessage(0);
            break;

        default:
            return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    SetProcessDPIAware();

    HRESULT hrCom = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    // hrCom may be S_OK or S_FALSE (already initialized); either is fine to proceed.

    g_uTaskbarRestart = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"OLEDAegisWindow";

    RegisterClassW(&wc);

    CreateWindowExW(0, L"OLEDAegisWindow", APP_NAME, 0, 0, 0, 0, 0, NULL, NULL, hInstance, NULL);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (SUCCEEDED(hrCom)) {
        CoUninitialize();
    }

    return (int)msg.wParam;
}
