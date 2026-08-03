// config.c - Settings persistence: oled_aegis.ini load/save, value
// clamping, startup registry entry, and opening the config file location.
//
// Part of OLED Aegis. See oled_aegis.h for the shared types/constants.

#include "oled_aegis.h"

static int ClampInt(int value, int minValue, int maxValue) {
    if (value < minValue) return minValue;
    if (value > maxValue) return maxValue;
    return value;
}
void ClampConfigValues() {
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
int ConfigFileExists() {
    char appDataPath[MAX_PATH];
    char configPath[MAX_PATH];
    GetAppDataPath(appDataPath, sizeof(appDataPath));
    sprintf_s(configPath, sizeof(configPath), "%s\\oled_aegis.ini", appDataPath);

    FILE* f = fopen(configPath, "r");
    if (f) {
        fclose(f);
        return 1;
    }
    return 0;
}

void LoadConfig() {
    char appDataPath[MAX_PATH];
    char configPath[MAX_PATH];
    GetAppDataPath(appDataPath, sizeof(appDataPath));
    sprintf_s(configPath, sizeof(configPath), "%s\\oled_aegis.ini", appDataPath);

    g_app.config.monitorCount = g_monitorCount;

    int hadMonitorConfig = 0;  // Track if we found any monitor config entries
    int anyMonitorMatched = 0; // Track if any monitor config matched current monitors

    FILE* f = fopen(configPath, "r");
    if (f) {
        char line[512];  // Increased buffer size for longer device paths
        while (fgets(line, sizeof(line), f)) {
            // Strip inline comments (everything after ';')
            char* comment = strchr(line, ';');
            if (comment) *comment = '\0';

            // Trim trailing whitespace
            size_t len = strlen(line);
            while (len > 0 && (line[len-1] == ' ' || line[len-1] == '\t' || line[len-1] == '\n' || line[len-1] == '\r')) {
                line[--len] = '\0';
            }

            char key[300], value[64];  // Increased key size for device paths
            if (sscanf(line, "%299[^=]=%63s", key, value) == 2) {
                if (strcmp(key, "idleTimeout") == 0) {
                    g_app.config.idleTimeout = atoi(value);
                } else if (strcmp(key, "checkInterval") == 0) {
                    g_app.config.checkInterval = atoi(value);
                } else if (strcmp(key, "audioDetectionEnabled") == 0) {
                    g_app.config.mediaDetectionEnabled = atoi(value);
                } else if (strcmp(key, "mediaDetectionEnabled") == 0) {
                    g_app.config.mediaDetectionEnabled = atoi(value);
                } else if (strcmp(key, "startupEnabled") == 0) {
                    g_app.config.startupEnabled = atoi(value);
                } else if (strcmp(key, "debugMode") == 0) {
                    g_app.config.debugMode = atoi(value);
                } else if (strcmp(key, "perMonitorInputDetection") == 0) {
                    g_app.config.perMonitorInputDetection = atoi(value);
                } else if (strcmp(key, "perMonitorMediaDetection") == 0) {
                    g_app.config.perMonitorMediaDetection = atoi(value);
                } else if (strcmp(key, "blockOnMutedMedia") == 0) {
                    g_app.config.blockOnMutedMedia = atoi(value);
                } else if (strcmp(key, "pixelShiftCompensation") == 0) {
                    g_app.config.pixelShiftCompensation = atoi(value);
                } else if (strcmp(key, "fadeDurationMs") == 0) {
                    g_app.config.fadeDurationMs = atoi(value);
                } else if (strncmp(key, "monitorEnabled_", 15) == 0) {
                    const char* identifier = key + 15;
                    hadMonitorConfig = 1;

                    // Try matching by device path first (new format)
                    int idx = FindMonitorByDevicePath(identifier);
                    if (idx < 0) {
                        // Fall back to device name match (legacy format: \\.\DISPLAY1)
                        idx = FindMonitorByDeviceName(identifier);
                    }

                    if (idx >= 0 && idx < MAX_MONITOR_COUNT) {
                        g_app.config.monitorsEnabled[idx] = atoi(value);
                        if (atoi(value)) {
                            anyMonitorMatched = 1;
                        }
                        LogMessage("Config: matched monitor %d (%s) from identifier: %s",
                                  idx, g_monitors[idx].friendlyName, identifier);
                    } else {
                        LogMessage("Config: no match for monitor identifier: %s", identifier);
                    }
                } else if (strncmp(key, "monitor", 7) == 0 && key[7] >= '0' && key[7] <= '9') {
                    // Legacy format: monitor0=1, monitor1=0, etc.
                    int idx = atoi(key + 7);
                    hadMonitorConfig = 1;
                    if (idx >= 0 && idx < MAX_MONITOR_COUNT && idx < g_monitorCount) {
                        g_app.config.monitorsEnabled[idx] = atoi(value);
                        if (atoi(value)) {
                            anyMonitorMatched = 1;
                        }
                    }
                }
            }
        }
        fclose(f);
    }

    // Fallback: if we had monitor config but none matched, enable the primary monitor
    // This handles the case where display configuration changed (monitors unplugged/replugged)
    if (hadMonitorConfig && !anyMonitorMatched) {
        int primaryIdx = FindPrimaryMonitorIndex();
        if (primaryIdx >= 0) {
            g_app.config.monitorsEnabled[primaryIdx] = 1;
            LogMessage("Config fallback: no monitors matched saved config, enabled primary monitor %d (%s)",
                      primaryIdx, g_monitors[primaryIdx].friendlyName);
        }
    }

    ClampConfigValues();
}

void SaveConfig() {
    char appDataPath[MAX_PATH];
    char configPath[MAX_PATH];
    GetAppDataPath(appDataPath, sizeof(appDataPath));
    sprintf_s(configPath, sizeof(configPath), "%s\\oled_aegis.ini", appDataPath);

    FILE* f = fopen(configPath, "w");
    if (f) {
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
        // Save monitor settings using persistent device path as key, with comment showing friendly name
        for (int i = 0; i < g_monitorCount; i++) {
            fprintf(f, "monitorEnabled_%s=%d ; %s\n",
                    g_monitors[i].monitorDevicePath,
                    g_app.config.monitorsEnabled[i],
                    g_monitors[i].displayName);
        }
        fclose(f);
    }
}

void UpdateStartupRegistry() {
    HKEY hKey;
    WCHAR exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);

    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        if (g_app.config.startupEnabled) {
            DWORD valueSize = (DWORD)((wcslen(exePath) + 1) * sizeof(WCHAR));
            LONG result = RegSetValueExW(hKey, APP_NAME, 0, REG_SZ, (BYTE*)exePath, valueSize);
            if (result == ERROR_SUCCESS) {
                RegFlushKey(hKey);
            }
        } else {
            RegDeleteValueW(hKey, APP_NAME);
        }
        RegCloseKey(hKey);
    }
}

void OpenConfigFileLocation() {
    char appDataPath[MAX_PATH];
    char configPath[MAX_PATH];
    GetAppDataPath(appDataPath, sizeof(appDataPath));
    sprintf_s(configPath, sizeof(configPath), "%s\\oled_aegis.ini", appDataPath);

    char selectCmd[MAX_PATH + 20];
    sprintf_s(selectCmd, sizeof(selectCmd), "/select,\"%s\"", configPath);
    ShellExecuteA(NULL, "open", "explorer.exe", selectCmd, NULL, SW_SHOW);
}
