// settings.c - Settings dialog UI: layout, tooltips, DPI scaling, ApplySettings. Part of OLED Aegis. Owns the dialog window and font handles.

#include "oled_aegis.h"

static UINT g_settingsDpi = 96;
HWND g_hSettingsDialog = NULL;
static HFONT g_hSettingsFont = NULL;
static HWND g_hTooltipControl = NULL;

static int ScaleDPI(int value)
{
    return MulDiv(value, g_settingsDpi, 96);
}

static UINT GetDpiForWindowCompat(HWND hWnd)
{
    // GetDpiForWindow requires Windows 10 1607+
    typedef UINT (WINAPI *PFN_GetDpiForWindow)(HWND);
    static PFN_GetDpiForWindow pfnGetDpiForWindow = NULL;
    static int checked = 0;

    if (!checked)
    {
        HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
        if (hUser32)
        {
            pfnGetDpiForWindow = (PFN_GetDpiForWindow)GetProcAddress(hUser32, "GetDpiForWindow");
        }
        checked = 1;
    }

    if (pfnGetDpiForWindow && hWnd)
    {
        return pfnGetDpiForWindow(hWnd);
    }

    // Fallback for older Windows versions
    HDC hdc = GetDC(hWnd);
    UINT dpi = GetDeviceCaps(hdc, LOGPIXELSX);
    ReleaseDC(hWnd, hdc);
    return dpi ? dpi : 96;
}

LRESULT CALLBACK SettingsDialogProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
        case WM_COMMAND:
        {
            int wmId = LOWORD(wParam);
            switch (wmId)
            {
                case IDC_APPLY_BTN:
                    ApplySettings(hWnd);
                    break;
                case IDC_CONFIG_BTN:
                    LogMessage("Settings: Opening config file location");
                    OpenConfigFileLocation();
                    break;
                case IDC_CLOSE_BTN:
                    LogMessage("Settings: Dialog closed via 'Close' button");
                    DestroyWindow(hWnd);
                    g_hSettingsDialog = NULL;
                    break;
            }
            return 0;
        }
        case WM_CLOSE:
            LogMessage("Settings: Dialog closed via WM_CLOSE");
            DestroyWindow(hWnd);
            g_hSettingsDialog = NULL;
            return 0;
        case WM_DESTROY:
            if (g_hSettingsFont)
            {
                DeleteObject(g_hSettingsFont);
                g_hSettingsFont = NULL;
            }
            return 0;
    }
    return DefWindowProc(hWnd, message, wParam, lParam);
}

static void AddTooltip(HWND hParent, HWND hControl, const char* text)
{
    TOOLINFOA ti = {0};
    ti.cbSize = sizeof(TOOLINFOA);
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = hParent;
    ti.uId = (UINT_PTR)hControl;
    ti.hinst = GetModuleHandle(NULL);
    ti.lpszText = (LPSTR)text;

    SendMessageA(g_hTooltipControl, TTM_ADDTOOLA, 0, (LPARAM)&ti);
}

void ShowSettingsDialog()
{
    EnsureCursorVisible("settings dialog opened");

    if (g_hSettingsDialog)
    {
        SetForegroundWindow(g_hSettingsDialog);
        return;
    }

    // Get DPI before creating the window (use primary monitor DPI)
    HDC hdc = GetDC(NULL);
    g_settingsDpi = GetDeviceCaps(hdc, LOGPIXELSX);
    ReleaseDC(NULL, hdc);
    if (g_settingsDpi == 0) g_settingsDpi = 96;

    // Create DPI-scaled font
    NONCLIENTMETRICSA ncm = {0};
    ncm.cbSize = sizeof(NONCLIENTMETRICSA);
    SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof(NONCLIENTMETRICSA), &ncm, 0);
    // Scale the font height for DPI
    ncm.lfMessageFont.lfHeight = MulDiv(ncm.lfMessageFont.lfHeight, g_settingsDpi, 96);
    g_hSettingsFont = CreateFontIndirectA(&ncm.lfMessageFont);

    HWND hTooltip = CreateWindowExA(0, TOOLTIPS_CLASSA, NULL,
                               WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                               CW_USEDEFAULT, CW_USEDEFAULT,
                               CW_USEDEFAULT, CW_USEDEFAULT,
                               g_hSettingsDialog, NULL, GetModuleHandle(NULL), NULL);
    g_hTooltipControl = hTooltip;

    WNDCLASSA wc = {0};
    wc.lpfnWndProc = DefWindowProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hIcon = g_hIconActive;
    wc.lpszClassName = "OLED Aegis Settings Dialog";
    RegisterClassA(&wc);

    HMODULE hMod = GetModuleHandle(NULL);
    g_hSettingsDialog = CreateWindowExA(0, "OLED Aegis Settings Dialog", "OLED Aegis Settings",
                                      WS_POPUP | WS_CAPTION | WS_SYSMENU,
                                      CW_USEDEFAULT, CW_USEDEFAULT,
                                      ScaleDPI(410), ScaleDPI(430),
                                      NULL, NULL, hMod, NULL);

    if (g_hSettingsDialog)
    {
        SetWindowLongPtr(g_hSettingsDialog, GWLP_WNDPROC, (LONG_PTR)SettingsDialogProc);

        // Update DPI now that we have a window
        g_settingsDpi = GetDpiForWindowCompat(g_hSettingsDialog);

        INITCOMMONCONTROLSEX icex;
        icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
        icex.dwICC = ICC_UPDOWN_CLASS;
        InitCommonControlsEx(&icex);

        // Layout constants (base values at 96 DPI)
        int margin = ScaleDPI(20);
        int rowHeight = ScaleDPI(25);
        int controlHeight = ScaleDPI(20);
        int labelWidth = ScaleDPI(180);
        int editWidth = ScaleDPI(100);
        int checkboxWidth = ScaleDPI(340);
        int buttonWidth = ScaleDPI(100);
        int configBtnWidth = ScaleDPI(130);
        int buttonHeight = ScaleDPI(30);
        int buttonSpacing = ScaleDPI(10);

        int y = margin;

        HWND hTimeoutLabel = CreateWindowA("STATIC", "Idle Timeout (seconds):",
                     WS_CHILD | WS_VISIBLE,
                     margin, y, labelWidth, controlHeight, g_hSettingsDialog, NULL, hMod, NULL);
        HWND hTimeoutEdit = CreateWindowExA(0, "EDIT", "",
                     WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
                     margin + labelWidth, y, editWidth, controlHeight,
                     g_hSettingsDialog, (HMENU)IDC_TIMEOUT_EDIT, hMod, NULL);
        HWND hTimeoutUpDown = CreateWindowExA(0, UPDOWN_CLASS, "",
                     WS_CHILD | WS_VISIBLE | UDS_AUTOBUDDY | UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_ARROWKEYS,
                     0, 0, 0, 0, g_hSettingsDialog, NULL, hMod, hTimeoutEdit);
        SendMessage(hTimeoutUpDown, UDM_SETRANGE, 0, MAKELPARAM(MAX_IDLE_TIMEOUT_SEC, MIN_IDLE_TIMEOUT_SEC));
        y += rowHeight + ScaleDPI(5);

        HWND hIntervalLabel = CreateWindowA("STATIC", "Check Interval (ms):",
                     WS_CHILD | WS_VISIBLE,
                     margin, y, labelWidth, controlHeight, g_hSettingsDialog, NULL, hMod, NULL);
        HWND hIntervalEdit = CreateWindowExA(0, "EDIT", "",
                     WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
                     margin + labelWidth, y, editWidth, controlHeight,
                     g_hSettingsDialog, (HMENU)IDC_INTERVAL_EDIT, hMod, NULL);
        HWND hIntervalUpDown = CreateWindowExA(0, UPDOWN_CLASS, "",
                     WS_CHILD | WS_VISIBLE | UDS_AUTOBUDDY | UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_ARROWKEYS,
                     0, 0, 0, 0, g_hSettingsDialog, NULL, hMod, hIntervalEdit);
        SendMessage(hIntervalUpDown, UDM_SETRANGE, 0, MAKELPARAM(MAX_CHECK_INTERVAL_MS, MIN_CHECK_INTERVAL_MS));
        y += rowHeight + ScaleDPI(5);

        HWND hPixelShiftLabel = CreateWindowA("STATIC", "Pixel Shift Compensation (px):",
                     WS_CHILD | WS_VISIBLE,
                     margin, y, labelWidth, controlHeight, g_hSettingsDialog, NULL, hMod, NULL);
        HWND hPixelShiftEdit = CreateWindowExA(0, "EDIT", "",
                     WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
                     margin + labelWidth, y, editWidth, controlHeight,
                     g_hSettingsDialog, (HMENU)IDC_PIXELSHIFT_EDIT, hMod, NULL);
        HWND hPixelShiftUpDown = CreateWindowExA(0, UPDOWN_CLASS, "",
                     WS_CHILD | WS_VISIBLE | UDS_AUTOBUDDY | UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_ARROWKEYS,
                     0, 0, 0, 0, g_hSettingsDialog, NULL, hMod, hPixelShiftEdit);
        SendMessage(hPixelShiftUpDown, UDM_SETRANGE, 0, MAKELPARAM(MAX_PIXEL_SHIFT_COMPENSATION, MIN_PIXEL_SHIFT_COMPENSATION));
        y += rowHeight + ScaleDPI(5);

        HWND hFadeLabel = CreateWindowA("STATIC", "Fade Duration (ms):",
                     WS_CHILD | WS_VISIBLE,
                     margin, y, labelWidth, controlHeight, g_hSettingsDialog, NULL, hMod, NULL);
        HWND hFadeEdit = CreateWindowExA(0, "EDIT", "",
                     WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
                     margin + labelWidth, y, editWidth, controlHeight,
                     g_hSettingsDialog, (HMENU)IDC_FADE_EDIT, hMod, NULL);
        HWND hFadeUpDown = CreateWindowExA(0, UPDOWN_CLASS, "",
                     WS_CHILD | WS_VISIBLE | UDS_AUTOBUDDY | UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_ARROWKEYS,
                     0, 0, 0, 0, g_hSettingsDialog, NULL, hMod, hFadeEdit);
        SendMessage(hFadeUpDown, UDM_SETRANGE, 0, MAKELPARAM(MAX_FADE_DURATION_MS, MIN_FADE_DURATION_MS));
        y += rowHeight + ScaleDPI(5);

        HWND hVideoCheck = CreateWindowA("BUTTON", "Prevent Screen Saver During Media Playback",
                     WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                     margin, y, checkboxWidth, controlHeight, g_hSettingsDialog, (HMENU)IDC_MEDIA_CHECK, hMod, NULL);
        y += rowHeight;

        HWND hDebugCheck = CreateWindowA("BUTTON", "Debug Mode",
                     WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                     margin, y, checkboxWidth, controlHeight, g_hSettingsDialog, (HMENU)IDC_DEBUG_CHECK, hMod, NULL);
        y += rowHeight;

        HWND hStartupCheck = CreateWindowA("BUTTON", "Run at Startup",
                     WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                     margin, y, checkboxWidth, controlHeight, g_hSettingsDialog, (HMENU)IDC_STARTUP_CHECK, hMod, NULL);
        y += rowHeight + ScaleDPI(5);

        HWND hPerMonitorCheck = CreateWindowA("BUTTON", "Per-Monitor Input Detection",
                     WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                     margin, y, checkboxWidth, controlHeight, g_hSettingsDialog, (HMENU)IDC_PERMONITOR_CHECK, hMod, NULL);
        y += rowHeight + ScaleDPI(5);

        HWND hPerMonitorMediaCheck = CreateWindowA("BUTTON", "Per-Monitor Media Detection",
                     WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                     margin, y, checkboxWidth, controlHeight, g_hSettingsDialog, (HMENU)IDC_PERMONITOR_MEDIA_CHECK, hMod, NULL);
        y += rowHeight + ScaleDPI(5);

        HWND hMutedMediaCheck = CreateWindowA("BUTTON", "Block During Muted Media",
                     WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                     margin, y, checkboxWidth, controlHeight, g_hSettingsDialog, (HMENU)IDC_MUTED_MEDIA_CHECK, hMod, NULL);
        y += rowHeight + ScaleDPI(5);

        HWND hMonitorsLabel = CreateWindowA("STATIC", "Monitors:",
                     WS_CHILD | WS_VISIBLE,
                     margin, y, ScaleDPI(100), controlHeight, g_hSettingsDialog, NULL, hMod, NULL);
        y += rowHeight;

        for (int i = 0; i < g_monitorCount; i++)
        {
            HWND hMonitorCheck = CreateWindowA("BUTTON", g_monitors[i].displayName,
                         WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                         margin, y, checkboxWidth, controlHeight,
                         g_hSettingsDialog, (HMENU)(INT_PTR)(IDC_MONITOR_BASE + i),
                         hMod, NULL);
            if (g_hSettingsFont) SendMessageA(hMonitorCheck, WM_SETFONT, (WPARAM)g_hSettingsFont, TRUE);
            y += rowHeight;
        }

        y += margin;
        int btnX = margin;
        HWND hApplyBtn = CreateWindowA("BUTTON", "Apply",
                     WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                     btnX, y, buttonWidth, buttonHeight, g_hSettingsDialog, (HMENU)IDC_APPLY_BTN, hMod, NULL);
        btnX += buttonWidth + buttonSpacing;

        HWND hConfigBtn = CreateWindowA("BUTTON", "Open Config File",
                     WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                     btnX, y, configBtnWidth, buttonHeight, g_hSettingsDialog, (HMENU)IDC_CONFIG_BTN, hMod, NULL);
        btnX += configBtnWidth + buttonSpacing;

        HWND hCloseBtn = CreateWindowA("BUTTON", "Close",
                     WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                     btnX, y, buttonWidth, buttonHeight, g_hSettingsDialog, (HMENU)IDC_CLOSE_BTN, hMod, NULL);

        // Calculate dialog size based on content
        int dialogWidth = margin + checkboxWidth + margin + ScaleDPI(20);  // Add extra for window borders
        int dialogHeight = y + buttonHeight + margin + ScaleDPI(40);  // Add extra for title bar and new control
        SetWindowPos(g_hSettingsDialog, NULL, 0, 0, dialogWidth, dialogHeight,
                     SWP_NOMOVE | SWP_NOZORDER);

        // Center on primary monitor
        int screenWidth = GetSystemMetrics(SM_CXSCREEN);
        int screenHeight = GetSystemMetrics(SM_CYSCREEN);
        SetWindowPos(g_hSettingsDialog, NULL,
                     (screenWidth - dialogWidth) / 2, (screenHeight - dialogHeight) / 2,
                     0, 0, SWP_NOSIZE | SWP_NOZORDER);

        // Apply font to all controls
        if (g_hSettingsFont)
        {
            SendMessageA(hTimeoutLabel, WM_SETFONT, (WPARAM)g_hSettingsFont, TRUE);
            SendMessageA(hTimeoutEdit, WM_SETFONT, (WPARAM)g_hSettingsFont, TRUE);
            SendMessageA(hTimeoutUpDown, WM_SETFONT, (WPARAM)g_hSettingsFont, TRUE);
            SendMessageA(hIntervalLabel, WM_SETFONT, (WPARAM)g_hSettingsFont, TRUE);
            SendMessageA(hIntervalEdit, WM_SETFONT, (WPARAM)g_hSettingsFont, TRUE);
            SendMessageA(hIntervalUpDown, WM_SETFONT, (WPARAM)g_hSettingsFont, TRUE);
            SendMessageA(hVideoCheck, WM_SETFONT, (WPARAM)g_hSettingsFont, TRUE);
            SendMessageA(hDebugCheck, WM_SETFONT, (WPARAM)g_hSettingsFont, TRUE);
            SendMessageA(hStartupCheck, WM_SETFONT, (WPARAM)g_hSettingsFont, TRUE);
            SendMessageA(hPerMonitorCheck, WM_SETFONT, (WPARAM)g_hSettingsFont, TRUE);
            SendMessageA(hPerMonitorMediaCheck, WM_SETFONT, (WPARAM)g_hSettingsFont, TRUE);
            SendMessageA(hMutedMediaCheck, WM_SETFONT, (WPARAM)g_hSettingsFont, TRUE);
            SendMessageA(hPixelShiftLabel, WM_SETFONT, (WPARAM)g_hSettingsFont, TRUE);
            SendMessageA(hPixelShiftEdit, WM_SETFONT, (WPARAM)g_hSettingsFont, TRUE);
            SendMessageA(hPixelShiftUpDown, WM_SETFONT, (WPARAM)g_hSettingsFont, TRUE);
            SendMessageA(hFadeLabel, WM_SETFONT, (WPARAM)g_hSettingsFont, TRUE);
            SendMessageA(hFadeEdit, WM_SETFONT, (WPARAM)g_hSettingsFont, TRUE);
            SendMessageA(hFadeUpDown, WM_SETFONT, (WPARAM)g_hSettingsFont, TRUE);
            SendMessageA(hMonitorsLabel, WM_SETFONT, (WPARAM)g_hSettingsFont, TRUE);
            SendMessageA(hApplyBtn, WM_SETFONT, (WPARAM)g_hSettingsFont, TRUE);
            SendMessageA(hConfigBtn, WM_SETFONT, (WPARAM)g_hSettingsFont, TRUE);
            SendMessageA(hCloseBtn, WM_SETFONT, (WPARAM)g_hSettingsFont, TRUE);
        }

        // Add tooltips
        AddTooltip(g_hSettingsDialog, hTimeoutEdit,
                   "Idle timeout in seconds before the screen saver activates.");
        AddTooltip(g_hSettingsDialog, hIntervalEdit,
                   "How often to poll for user activity. (250-10000ms).");
        AddTooltip(g_hSettingsDialog, hVideoCheck,
                   "Prevent screen saver activation during video playback (on any monitor).");
        AddTooltip(g_hSettingsDialog, hDebugCheck,
                   "Enable debug logging to %APPDATA%\\OLED_Aegis\\oled_aegis_debug.log");
        AddTooltip(g_hSettingsDialog, hStartupCheck,
                   "Automatically start OLED Aegis when you log into Windows.");
        AddTooltip(g_hSettingsDialog, hPerMonitorCheck,
                   "Track input separately for each monitor. Allows screen saver to activate on unused monitors while you continue using others.");
        AddTooltip(g_hSettingsDialog, hPerMonitorMediaCheck,
                   "Detect media playback per monitor instead of globally. "
                   "Only blocks the screen saver on the monitor where media is actually playing, so playback on a non-OLED display won't keep the OLED awake.");
        AddTooltip(g_hSettingsDialog, hMutedMediaCheck,
                   "Block the screen saver even when media is muted or inaudible (e.g. muted video, OBS replay buffer). "
                   "When off, only audible media prevents the screen saver.");
        AddTooltip(g_hSettingsDialog, hPixelShiftEdit,
                   "Expand the screen saver window beyond the monitor bounds by this many pixels on each side. "
                   "Use 4-8 on QD-OLED panels (e.g. Alienware) to prevent hardware pixel shift from exposing the desktop edge. (0 = disabled)");
        AddTooltip(g_hSettingsDialog, hFadeEdit,
                   "Duration of the fade-to-black transition when the screen saver activates, and the fade back when it hides. "
                   "0 = instant (no fade). (0-3000ms)");

        // Set initial values
        char buffer[32];
        sprintf_s(buffer, 32, "%d", g_app.config.idleTimeout);
        SetDlgItemTextA(g_hSettingsDialog, IDC_TIMEOUT_EDIT, buffer);

        sprintf_s(buffer, 32, "%d", g_app.config.checkInterval);
        SetDlgItemTextA(g_hSettingsDialog, IDC_INTERVAL_EDIT, buffer);

        CheckDlgButton(g_hSettingsDialog, IDC_MEDIA_CHECK, g_app.config.mediaDetectionEnabled ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(g_hSettingsDialog, IDC_DEBUG_CHECK, g_app.config.debugMode ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(g_hSettingsDialog, IDC_STARTUP_CHECK, g_app.config.startupEnabled ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(g_hSettingsDialog, IDC_PERMONITOR_CHECK, g_app.config.perMonitorInputDetection ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(g_hSettingsDialog, IDC_PERMONITOR_MEDIA_CHECK, g_app.config.perMonitorMediaDetection ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(g_hSettingsDialog, IDC_MUTED_MEDIA_CHECK, g_app.config.blockOnMutedMedia ? BST_CHECKED : BST_UNCHECKED);

        sprintf_s(buffer, 32, "%d", g_app.config.pixelShiftCompensation);
        SetDlgItemTextA(g_hSettingsDialog, IDC_PIXELSHIFT_EDIT, buffer);

        sprintf_s(buffer, 32, "%d", g_app.config.fadeDurationMs);
        SetDlgItemTextA(g_hSettingsDialog, IDC_FADE_EDIT, buffer);

        for (int i = 0; i < g_monitorCount; i++)
        {
            CheckDlgButton(g_hSettingsDialog, IDC_MONITOR_BASE + i, g_app.config.monitorsEnabled[i] ? BST_CHECKED : BST_UNCHECKED);
        }

        ShowWindow(g_hSettingsDialog, SW_SHOW);
        UpdateWindow(g_hSettingsDialog);
    }
}

void ApplySettings(HWND hWnd)
{
    char buffer[32];
    GetDlgItemTextA(hWnd, IDC_TIMEOUT_EDIT, buffer, 32);
    int oldTimeout = g_app.config.idleTimeout;
    g_app.config.idleTimeout = atoi(buffer);

    GetDlgItemTextA(hWnd, IDC_INTERVAL_EDIT, buffer, 32);
    int oldInterval = g_app.config.checkInterval;
    g_app.config.checkInterval = atoi(buffer);

    int oldMedia = g_app.config.mediaDetectionEnabled;
    int oldDebug = g_app.config.debugMode;
    int oldStartup = g_app.config.startupEnabled;
    int oldPerMonitor = g_app.config.perMonitorInputDetection;
    int oldPerMonitorMedia = g_app.config.perMonitorMediaDetection;
    int oldBlockOnMutedMedia = g_app.config.blockOnMutedMedia;

    g_app.config.mediaDetectionEnabled = IsDlgButtonChecked(hWnd, IDC_MEDIA_CHECK) == BST_CHECKED;
    g_app.config.debugMode = IsDlgButtonChecked(hWnd, IDC_DEBUG_CHECK) == BST_CHECKED;
    g_app.config.startupEnabled = IsDlgButtonChecked(hWnd, IDC_STARTUP_CHECK) == BST_CHECKED;
    g_app.config.perMonitorInputDetection = IsDlgButtonChecked(hWnd, IDC_PERMONITOR_CHECK) == BST_CHECKED;
    g_app.config.perMonitorMediaDetection = IsDlgButtonChecked(hWnd, IDC_PERMONITOR_MEDIA_CHECK) == BST_CHECKED;
    g_app.config.blockOnMutedMedia = IsDlgButtonChecked(hWnd, IDC_MUTED_MEDIA_CHECK) == BST_CHECKED;

    GetDlgItemTextA(hWnd, IDC_PIXELSHIFT_EDIT, buffer, 32);
    g_app.config.pixelShiftCompensation = atoi(buffer);

    GetDlgItemTextA(hWnd, IDC_FADE_EDIT, buffer, 32);
    g_app.config.fadeDurationMs = atoi(buffer);
    ClampConfigValues();

    for (int i = 0; i < g_monitorCount; i++)
    {
        int wasEnabled = g_app.config.monitorsEnabled[i];
        g_app.config.monitorsEnabled[i] = IsDlgButtonChecked(hWnd, IDC_MONITOR_BASE + i) == BST_CHECKED;
        g_monitorStates[i].enabled = g_app.config.monitorsEnabled[i];

        if (!g_app.config.monitorsEnabled[i] && g_monitorStates[i].screenSaverActive)
        {
            LogMessage("Disabling monitor %d which has active screen saver, hiding it", i);
            HideScreenSaverOnMonitor(i);
        }

        if (!g_app.config.monitorsEnabled[i] && wasEnabled && g_monitorStates[i].hScreenSaverWnd)
        {
            DestroyWindow(g_monitorStates[i].hScreenSaverWnd);
            g_monitorStates[i].hScreenSaverWnd = NULL;
            LogMessage("Destroyed screen saver window for disabled monitor %d", i);
        }
    }

    if (!IsAnyMonitorActive())
    {
        g_app.screenSaverActive = 0;
        EnsureCursorVisible("no active monitors after settings");
    }

    if (!oldPerMonitor && g_app.config.perMonitorInputDetection)
    {
        time_t now = time(NULL);
        for (int i = 0; i < g_monitorCount; i++)
        {
            g_monitorStates[i].lastInputTime = now;
        }
        LogMessage("Per-monitor mode enabled: reset all monitor idle times");
    }

    SaveConfig();
    UpdateStartupRegistry();

    LogMessage("Settings applied: timeout %ds->%ds, interval %dms->%dms, media %d->%d, debug %d->%d, "
               "startup %d->%d, perMonitor %d->%d, perMonitorMedia %d->%d, mutedMedia %d->%d, "
               "pixelShift %dpx, fade %dms",
             oldTimeout, g_app.config.idleTimeout,
             oldInterval, g_app.config.checkInterval,
             oldMedia, g_app.config.mediaDetectionEnabled,
             oldDebug, g_app.config.debugMode,
             oldStartup, g_app.config.startupEnabled,
             oldPerMonitor, g_app.config.perMonitorInputDetection,
             oldPerMonitorMedia, g_app.config.perMonitorMediaDetection,
             oldBlockOnMutedMedia, g_app.config.blockOnMutedMedia,
             g_app.config.pixelShiftCompensation,
             g_app.config.fadeDurationMs);

    if (oldInterval != g_app.config.checkInterval)
    {
        KillTimer(g_app.hWnd, TIMER_IDLE_CHECK);
        SetTimer(g_app.hWnd, TIMER_IDLE_CHECK, g_app.config.checkInterval, NULL);
        LogMessage("Timer recreated with new interval: %dms", g_app.config.checkInterval);
    }

    sprintf_s(buffer, 32, "%d", g_app.config.checkInterval);
    SetDlgItemTextA(hWnd, IDC_INTERVAL_EDIT, buffer);
}

