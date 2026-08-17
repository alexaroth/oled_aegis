// oled_aegis.h - Shared header: constants, types, shared globals, prototypes. Module ownership listed in AGENTS.md; GUIDs are defined in media.c.

#ifndef OLED_AEGIS_H
#define OLED_AEGIS_H

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <time.h>
#include <stdarg.h>
#include <string.h>
#include <commctrl.h>
#include <powerbase.h>
#include <psapi.h>
#include <dwmapi.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "powrprof.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "ole32.lib")

#define APP_NAME L"OLED Aegis"
#define WM_TRAYICON (WM_USER + 1)
#define TIMER_IDLE_CHECK 1
#define TIMER_FADE 2
#define DEFAULT_IDLE_TIMEOUT 300
#define MAX_LOG_SIZE_BYTES (1 * 1024 * 1024)  // 1 MB log file size limit
#define MANUAL_ACTIVATION_COOLDOWN_MS 2500
#define MAX_MONITOR_COUNT 16

// Resource IDs (must match oled_aegis.rc)
#define IDI_ICON_ACTIVE   101
#define IDI_ICON_INACTIVE 102

// Settings dialog control IDs
#define IDC_TIMEOUT_EDIT            1001
#define IDC_MEDIA_CHECK             1002
#define IDC_DEBUG_CHECK             1003
#define IDC_STARTUP_CHECK           1004
#define IDC_APPLY_BTN               1005
#define IDC_CONFIG_BTN              1006
#define IDC_CLOSE_BTN               1007
#define IDC_INTERVAL_EDIT           1008
#define IDC_PERMONITOR_CHECK        1009
#define IDC_PERMONITOR_MEDIA_CHECK  1011
#define IDC_MUTED_MEDIA_CHECK       1012
#define IDC_PIXELSHIFT_EDIT         1010
#define IDC_MONITOR_BASE            2000  // Monitor checkboxes: IDC_MONITOR_BASE + index
#define IDC_FADE_EDIT               1013

// Tray context menu command IDs
#define IDM_SETTINGS            1
#define IDM_EXIT                2

// Timing constants
#define INPUT_IGNORE_DELAY_MS           500     // Ignore input right after screen saver window creation
#define IDLE_ACTIVITY_THRESHOLD_MS      1000    // Time threshold to count the user as active
#define IDLE_DEACTIVATE_THRESHOLD_MS    2000    // Time threshold to deactivate screen saver after input
#define IDLE_DEACTIVATE_THRESHOLD_SEC   2       // Time threshold in seconds (for per-monitor mode)
#define SHELL_CLOSE_DELAY_MS            250     // Delay after sending Escape to close shell windows
#define SHELL_CLOSE_MAX_ATTEMPTS        2       // Maximum attempts to close shell windows
#define MIN_MEDIA_WINDOW_AREA           10000   // Ignore tiny windows when mapping media to monitors
#define MIN_MEDIA_WINDOW_OVERLAP_RATIO  0.10    // Ignore thin window-border overlap onto adjacent monitors
#define MEDIA_DETECTION_CACHE_MS        2000    // Cache media-window scans to keep timer work light
#define AUDIO_ACTIVE_PEAK_THRESHOLD     0.0001f // Ignore paused/silent sessions that remain "active"
#define AUDIO_GRACE_PERIOD_MS           30000   // Keep media state during brief audio silence (quiet passages)
#define CURSOR_COUNTER_MAX_ATTEMPTS     16      // Safety bound when normalizing ShowCursor's counter
#define TOPMOST_REFRESH_INTERVAL_MS     5000    // Reassert topmost occasionally, not every timer tick
#define FULLSCREEN_FOREGROUND_MIN_COVERAGE_PCT 95 // Foreground window covering >=95% of a monitor counts as fullscreen (pixel-probe skip)
#define FADE_TIMER_INTERVAL_MS          15      // Fade animation step (~66 fps)
#define DEFAULT_FADE_DURATION_MS        400     // Default fade-to-black transition duration
#define MIN_FADE_DURATION_MS            0       // 0 = instant show/hide (legacy behavior)
#define MAX_FADE_DURATION_MS            3000
#define MAX_ACTIVE_AUDIO_PIDS           64      // Cap on concurrently active audio sessions we track
#define MAX_BROWSER_WINDOW_INFO         32      // Max browser windows to collect for diagnostic logging

// Check interval bounds (milliseconds)
#define MIN_CHECK_INTERVAL_MS   250
#define MAX_CHECK_INTERVAL_MS   10000

// Idle timeout bounds (seconds)
#define MIN_IDLE_TIMEOUT_SEC    5
#define MAX_IDLE_TIMEOUT_SEC    3600

// Pixel shift compensation bounds (pixels)
#define MIN_PIXEL_SHIFT_COMPENSATION    0
#define MAX_PIXEL_SHIFT_COMPENSATION    1024

// Device name prefix for display devices (e.g., "\\.\DISPLAY1")
#define DEVICE_NAME_PREFIX      "\\\\.\\"
#define DEVICE_NAME_PREFIX_LEN  4

typedef struct
{
    HMONITOR hMonitor;
    RECT rect;
    int monitorIndex;
    char deviceName[CCHDEVICENAME];     // GDI device name (e.g., \\.\DISPLAY1)
    char displayName[128];              // UI display name (friendly name + resolution)
    char friendlyName[64];              // EDID friendly name (e.g., "LG OLED48C1")
    char monitorDevicePath[256];        // Persistent device path for config matching
    int isPrimary;
    int width;
    int height;
} MonitorInfo;

typedef struct
{
    time_t lastInputTime;
    int screenSaverActive;
    int enabled;
    HWND hScreenSaverWnd;
    DWORD mediaPauseOffsetMs;  // Idle ms excluded from this monitor's countdown while media plays on it
    // Fade-to-black state (when fadeDurationMs > 0): alpha animates from fadeFromAlpha to fadeToAlpha; fadeActive = 0 when no fade is running.
    int fadeActive;
    BYTE fadeFromAlpha;
    BYTE fadeToAlpha;
    DWORD fadeStartTick;
    DWORD fadeDurationMs;
} MonitorState;

typedef struct
{
    int idleTimeout;
    int checkInterval;
    int mediaDetectionEnabled;
    int monitorsEnabled[MAX_MONITOR_COUNT];
    int monitorCount;
    int startupEnabled;
    int debugMode;
    int perMonitorInputDetection;
    int perMonitorMediaDetection;
    int blockOnMutedMedia;
    int pixelShiftCompensation;
    int fadeDurationMs;  // Fade-to-black transition duration in ms (0 = instant)
} Config;

typedef struct
{
    HWND hWnd;
    Config config;
    NOTIFYICONDATAA nid;
    int screenSaverActive;
    int isShuttingDown;
    int cursorHidden;
    int trayMenuActive;
    int trayIconActive;
    DWORD manualActivationTime;
    int isManualActivation;
} AppState;

// Shared globals (defined in the owning module; see module list above)
extern AppState g_app;                                  // oled_aegis.c
extern HBRUSH g_blackBrush;                             // oled_aegis.c
extern HICON g_hIconActive;                             // oled_aegis.c
extern HICON g_hIconInactive;                           // oled_aegis.c
extern HWND g_hSettingsDialog;                          // settings.c
extern int g_monitorCount;                              // monitors.c
extern MonitorInfo g_monitors[MAX_MONITOR_COUNT];       // monitors.c
extern MonitorState g_monitorStates[MAX_MONITOR_COUNT]; // monitors.c
extern FILE* g_logFile;                                 // logging.c

// util.c
void GetAppDataPath(char* buffer, size_t bufferSize);
void EnsureCursorVisible(const char* reason);
void HideCursorForScreenSaver(const char* reason);

// logging.c
void LogMessage(const char* format, ...);

// config.c
void ClampConfigValues();
int ConfigFileExists();
void LoadConfig();
void SaveConfig();
void UpdateStartupRegistry();
void OpenConfigFileLocation();

// monitors.c
void EnumerateMonitors();
int GetMonitorIndexFromPoint(POINT pt);
int GetMonitorIndexFromRect(RECT rect);
int IsAnyMonitorActive();
int IsAnyMonitorEnabled();
int FindMonitorByDeviceName(const char* deviceName);
int FindMonitorByDevicePath(const char* devicePath);
int FindPrimaryMonitorIndex();

// screensaver.c
LRESULT CALLBACK MonitorWindowProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
void ShowScreenSaverOnMonitor(int monitorIndex, int isManual);
void ShowScreenSaver(int isManual);
void HideScreenSaver();
void HideScreenSaverOnMonitor(int monitorIndex);
int IsShellWindowOpen();
void CloseShellWindows(int escapeCount);
int IsShellOverlayWindow(HWND hWnd);
void EnsureScreenSaverTopmost();
void VerifyScreenSaverWindows();
void UpdateFades();

// media.c
int IsMediaPlaying();
int GetProcessNameFromHwnd(HWND hWnd, char* buffer, int bufferSize);
int UpdateMediaMonitorStates(int mediaOnMonitor[MAX_MONITOR_COUNT]);
void ResetMediaDetectionCache();

// settings.c
LRESULT CALLBACK SettingsDialogProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
void ShowSettingsDialog();
void ApplySettings(HWND hWnd);

// oled_aegis.c (main)
void LoadTrayIcons();
void UpdateTrayIcon(int active);
int HandleCreation(HWND hWnd);
void HandleTimeout(WPARAM wParam);
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow);

#endif // OLED_AEGIS_H
