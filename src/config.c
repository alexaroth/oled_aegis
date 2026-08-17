// config.c - oled_aegis.ini load/save, clamping, startup registry entry. Part of OLED Aegis. See oled_aegis.h for shared types/constants.

#include "oled_aegis.h"

// Remember settings of monitors seen in the config so a SaveConfig while one is temporarily disconnected doesn't erase its selection (re-emitted if absent).
#define MAX_KNOWN_MONITORS (MAX_MONITOR_COUNT * 4)
#define KNOWN_PATH_MAX 256  // Must hold monitorDevicePath (char[256])
static char g_knownMonitorPaths[MAX_KNOWN_MONITORS][KNOWN_PATH_MAX];
static int g_knownMonitorEnabled[MAX_KNOWN_MONITORS];
static int g_knownMonitorCount = 0;

static int ClampInt(int value, int minValue, int maxValue)
{
    if (value < minValue) return minValue;
    if (value > maxValue) return maxValue;
    return value;
}
void ClampConfigValues()
{
    g_app.config.idleTimeout = ClampInt(g_app.config.idleTimeout, MIN_IDLE_TIMEOUT_SEC, MAX_IDLE_TIMEOUT_SEC);
    g_app.config.checkInterval = ClampInt(g_app.config.checkInterval, MIN_CHECK_INTERVAL_MS, MAX_CHECK_INTERVAL_MS);
    g_app.config.pixelShiftCompensation = ClampInt(
        g_app.config.pixelShiftCompensation,
        MIN_PIXEL_SHIFT_COMPENSATION,
        MAX_PIXEL_SHIFT_COMPENSATION
    );
    g_app.config.fadeDurationMs = ClampInt(g_app.config.fadeDurationMs, MIN_FADE_DURATION_MS, MAX_FADE_DURATION_MS);

    g_app.config.mediaDetectionEnabled = g_app.config.mediaDetectionEnabled ? 1 : 0;
    g_app.config.startupEnabled = g_app.config.startupEnabled ? 1 : 0;
    g_app.config.debugMode = g_app.config.debugMode ? 1 : 0;
    g_app.config.perMonitorInputDetection = g_app.config.perMonitorInputDetection ? 1 : 0;
    g_app.config.perMonitorMediaDetection = g_app.config.perMonitorMediaDetection ? 1 : 0;
    g_app.config.blockOnMutedMedia = g_app.config.blockOnMutedMedia ? 1 : 0;
}
int ConfigFileExists()
{
    char appDataPath[MAX_PATH];
    char configPath[MAX_PATH];
    GetAppDataPath(appDataPath, sizeof(appDataPath));
    sprintf_s(configPath, sizeof(configPath), "%s\\oled_aegis.ini", appDataPath);

    FILE* f = fopen(configPath, "r");
    if (f)
    {
        fclose(f);
        return 1;
    }
    return 0;
}

// Record a monitorEnabled_<path> entry so its setting survives disconnects.
static void RememberMonitorSetting(const char* devicePath, int enabled)
{
    for (int i = 0; i < g_knownMonitorCount; i++)
    {
        if (strcmp(g_knownMonitorPaths[i], devicePath) == 0)
        {
            g_knownMonitorEnabled[i] = enabled;
            return;
        }
    }
    if (g_knownMonitorCount < MAX_KNOWN_MONITORS)
    {
        strncpy(g_knownMonitorPaths[g_knownMonitorCount], devicePath, KNOWN_PATH_MAX - 1);
        g_knownMonitorPaths[g_knownMonitorCount][KNOWN_PATH_MAX - 1] = '\0';
        g_knownMonitorEnabled[g_knownMonitorCount] = enabled;
        g_knownMonitorCount++;
    }
}

void LoadConfig()
{
    char appDataPath[MAX_PATH];
    char configPath[MAX_PATH];
    GetAppDataPath(appDataPath, sizeof(appDataPath));
    sprintf_s(configPath, sizeof(configPath), "%s\\oled_aegis.ini", appDataPath);

    g_app.config.monitorCount = g_monitorCount;

    // Reset first so monitors without an entry start disabled; stale values or the enable-all HandleCreation default would resurrect a disabled monitor.
    for (int i = 0; i < MAX_MONITOR_COUNT; i++)
    {
        g_app.config.monitorsEnabled[i] = 0;
    }
    g_knownMonitorCount = 0;

    int hadMonitorConfig = 0;
    int anyMonitorMatched = 0;

    FILE* f = fopen(configPath, "r");
    if (f)
    {
        char line[512];  // Large enough for long device paths
        while (fgets(line, sizeof(line), f))
        {
            // Strip inline comments (everything after ';')
            char* comment = strchr(line, ';');
            if (comment) *comment = '\0';

            // Trim trailing whitespace
            size_t len = strlen(line);
            while (len > 0 && (line[len-1] == ' ' || line[len-1] == '\t' || line[len-1] == '\n' || line[len-1] == '\r'))
            {
                line[--len] = '\0';
            }

            char key[300], value[64];  // Large enough for device path keys
            if (sscanf(line, "%299[^=]=%63s", key, value) == 2)
            {
                if (strcmp(key, "idleTimeout") == 0)
                {
                    g_app.config.idleTimeout = atoi(value);
                }
                else if (strcmp(key, "checkInterval") == 0)
                {
                    g_app.config.checkInterval = atoi(value);
                }
                else if (strcmp(key, "audioDetectionEnabled") == 0)
                {
                    g_app.config.mediaDetectionEnabled = atoi(value);
                }
                else if (strcmp(key, "mediaDetectionEnabled") == 0)
                {
                    g_app.config.mediaDetectionEnabled = atoi(value);
                }
                else if (strcmp(key, "startupEnabled") == 0)
                {
                    g_app.config.startupEnabled = atoi(value);
                }
                else if (strcmp(key, "debugMode") == 0)
                {
                    g_app.config.debugMode = atoi(value);
                }
                else if (strcmp(key, "perMonitorInputDetection") == 0)
                {
                    g_app.config.perMonitorInputDetection = atoi(value);
                }
                else if (strcmp(key, "perMonitorMediaDetection") == 0)
                {
                    g_app.config.perMonitorMediaDetection = atoi(value);
                }
                else if (strcmp(key, "blockOnMutedMedia") == 0)
                {
                    g_app.config.blockOnMutedMedia = atoi(value);
                }
                else if (strcmp(key, "pixelShiftCompensation") == 0)
                {
                    g_app.config.pixelShiftCompensation = atoi(value);
                }
                else if (strcmp(key, "fadeDurationMs") == 0)
                {
                    g_app.config.fadeDurationMs = atoi(value);
                }
                else if (strncmp(key, "monitorEnabled_", 15) == 0)
                {
                    const char* identifier = key + 15;
                    hadMonitorConfig = 1;

                    // Remember even if absent now, so SaveConfig can re-emit it later.
                    RememberMonitorSetting(identifier, atoi(value));

                    // Try matching by device path first (new format)
                    int idx = FindMonitorByDevicePath(identifier);
                    if (idx < 0)
                    {
                        // Fall back to device name match (legacy format: \\.\DISPLAY1)
                        idx = FindMonitorByDeviceName(identifier);
                    }

                    if (idx >= 0 && idx < MAX_MONITOR_COUNT)
                    {
                        g_app.config.monitorsEnabled[idx] = atoi(value);
                        if (atoi(value))
                        {
                            anyMonitorMatched = 1;
                        }
                        LogMessage("Config: matched monitor %d (%s) from identifier: %s",
                                  idx, g_monitors[idx].friendlyName, identifier);
                    }
                    else
                    {
                        LogMessage("Config: no match for monitor identifier: %s", identifier);
                    }
                }
                else if (strncmp(key, "monitor", 7) == 0 && key[7] >= '0' && key[7] <= '9')
                {
                    // Legacy format: monitor0=1, monitor1=0, etc.
                    int idx = atoi(key + 7);
                    hadMonitorConfig = 1;
                    if (idx >= 0 && idx < MAX_MONITOR_COUNT && idx < g_monitorCount)
                    {
                        g_app.config.monitorsEnabled[idx] = atoi(value);
                        if (atoi(value))
                        {
                            anyMonitorMatched = 1;
                        }
                    }
                }
            }
        }
        fclose(f);
    }

    // No entries at all (pre-monitor-config era): enable primary only. Entries that don't match mean configured monitors are disconnected: enable nothing.
    if (!hadMonitorConfig)
    {
        int primaryIdx = FindPrimaryMonitorIndex();
        if (primaryIdx >= 0)
        {
            g_app.config.monitorsEnabled[primaryIdx] = 1;
            LogMessage("Config fallback: no monitor entries in config, enabled primary monitor %d (%s)",
                      primaryIdx, g_monitors[primaryIdx].friendlyName);
        }
    }
    else if (!anyMonitorMatched)
    {
        LogMessage("Config: monitor entries present but none matched (%d monitors) - nothing enabled",
                  g_monitorCount);
    }

    ClampConfigValues();
}

void SaveConfig()
{
    char appDataPath[MAX_PATH];
    char configPath[MAX_PATH];
    GetAppDataPath(appDataPath, sizeof(appDataPath));
    sprintf_s(configPath, sizeof(configPath), "%s\\oled_aegis.ini", appDataPath);

    FILE* f = fopen(configPath, "w");
    if (f)
    {
        fprintf(f, "idleTimeout=%d\n", g_app.config.idleTimeout);
        fprintf(f, "checkInterval=%d\n", g_app.config.checkInterval);
        fprintf(f, "mediaDetectionEnabled=%d\n", g_app.config.mediaDetectionEnabled);
        fprintf(f, "startupEnabled=%d\n", g_app.config.startupEnabled);
        fprintf(f, "debugMode=%d\n", g_app.config.debugMode);
        fprintf(f, "perMonitorInputDetection=%d\n", g_app.config.perMonitorInputDetection);
        fprintf(f, "perMonitorMediaDetection=%d\n", g_app.config.perMonitorMediaDetection);
        fprintf(f, "blockOnMutedMedia=%d\n", g_app.config.blockOnMutedMedia);
        fprintf(f, "pixelShiftCompensation=%d\n", g_app.config.pixelShiftCompensation);
        fprintf(f, "fadeDurationMs=%d\n", g_app.config.fadeDurationMs);
        // Save monitor settings keyed by device path, with friendly name in comment
        for (int i = 0; i < g_monitorCount; i++)
        {
            fprintf(f, "monitorEnabled_%s=%d ; %s\n",
                    g_monitors[i].monitorDevicePath,
                    g_app.config.monitorsEnabled[i],
                    g_monitors[i].displayName);
        }
        // Re-emit known-but-disconnected entries so an Apply can't erase them.
        for (int i = 0; i < g_knownMonitorCount; i++)
        {
            int connected = 0;
            for (int j = 0; j < g_monitorCount; j++)
            {
                if (strcmp(g_knownMonitorPaths[i], g_monitors[j].monitorDevicePath) == 0)
                {
                    connected = 1;
                    break;
                }
            }
            if (!connected)
            {
                fprintf(f, "monitorEnabled_%s=%d ; not connected\n",
                        g_knownMonitorPaths[i], g_knownMonitorEnabled[i]);
            }
        }
        fclose(f);
    }
}

void UpdateStartupRegistry()
{
    HKEY hKey;
    WCHAR exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);

    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS)
    {
        if (g_app.config.startupEnabled)
        {
            DWORD valueSize = (DWORD)((wcslen(exePath) + 1) * sizeof(WCHAR));
            LONG result = RegSetValueExW(hKey, APP_NAME, 0, REG_SZ, (BYTE*)exePath, valueSize);
            if (result == ERROR_SUCCESS)
            {
                RegFlushKey(hKey);
            }
        }
        else
        {
            RegDeleteValueW(hKey, APP_NAME);
        }
        RegCloseKey(hKey);
    }
}

void OpenConfigFileLocation()
{
    char appDataPath[MAX_PATH];
    char configPath[MAX_PATH];
    GetAppDataPath(appDataPath, sizeof(appDataPath));
    sprintf_s(configPath, sizeof(configPath), "%s\\oled_aegis.ini", appDataPath);

    char selectCmd[MAX_PATH + 20];
    sprintf_s(selectCmd, sizeof(selectCmd), "/select,\"%s\"", configPath);
    ShellExecuteA(NULL, "open", "explorer.exe", selectCmd, NULL, SW_SHOW);
}
