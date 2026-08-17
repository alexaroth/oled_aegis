// monitors.c - Monitor enumeration (GDI + DisplayConfig) and lookup helpers. Part of OLED Aegis. Owns g_monitors / g_monitorStates. See oled_aegis.h.

#include "oled_aegis.h"

int g_monitorCount = 0;
MonitorInfo g_monitors[MAX_MONITOR_COUNT];
MonitorState g_monitorStates[MAX_MONITOR_COUNT];

static int GetMonitorIdentifiers(const char* gdiDeviceName,
                          char* friendlyName, int friendlyNameLen,
                          char* devicePath, int devicePathLen)
{
    UINT32 pathCount = 0, modeCount = 0;
    int result = 0;

    if (friendlyName && friendlyNameLen > 0) friendlyName[0] = '\0';
    if (devicePath && devicePathLen > 0) devicePath[0] = '\0';

    LONG ret = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount);
    if (ret != ERROR_SUCCESS || pathCount == 0)
    {
        return 0;
    }

    DISPLAYCONFIG_PATH_INFO* paths = malloc(pathCount * sizeof(DISPLAYCONFIG_PATH_INFO));
    DISPLAYCONFIG_MODE_INFO* modes = malloc(modeCount * sizeof(DISPLAYCONFIG_MODE_INFO));
    if (!paths || !modes)
    {
        free(paths);
        free(modes);
        return 0;
    }

    ret = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths, &modeCount, modes, NULL);
    if (ret != ERROR_SUCCESS)
    {
        free(paths);
        free(modes);
        return 0;
    }

    // Convert GDI device name to wide string for comparison
    WCHAR gdiDeviceNameW[CCHDEVICENAME];
    MultiByteToWideChar(CP_ACP, 0, gdiDeviceName, -1, gdiDeviceNameW, CCHDEVICENAME);

    for (UINT32 i = 0; i < pathCount; i++)
    {
        // Get source device name (GDI device name like \\.\DISPLAY1)
        DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName = {0};
        sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        sourceName.header.size = sizeof(sourceName);
        sourceName.header.adapterId = paths[i].sourceInfo.adapterId;
        sourceName.header.id = paths[i].sourceInfo.id;

        ret = DisplayConfigGetDeviceInfo(&sourceName.header);
        if (ret != ERROR_SUCCESS)
        {
            continue;
        }

        if (wcscmp(sourceName.viewGdiDeviceName, gdiDeviceNameW) != 0)
        {
            continue;
        }

        // Found matching source; get target (monitor) info
        DISPLAYCONFIG_TARGET_DEVICE_NAME targetName = {0};
        targetName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        targetName.header.size = sizeof(targetName);
        targetName.header.adapterId = paths[i].targetInfo.adapterId;
        targetName.header.id = paths[i].targetInfo.id;

        ret = DisplayConfigGetDeviceInfo(&targetName.header);
        if (ret != ERROR_SUCCESS)
        {
            continue;
        }

        // Extract friendly name (if available from EDID)
        if (friendlyName && friendlyNameLen > 0)
        {
            if (targetName.flags.friendlyNameFromEdid)
            {
                WideCharToMultiByte(CP_UTF8, 0, targetName.monitorFriendlyDeviceName, -1,
                                   friendlyName, friendlyNameLen, NULL, NULL);
            }
            else
            {
                strncpy(friendlyName, "Unknown Monitor", friendlyNameLen - 1);
                friendlyName[friendlyNameLen - 1] = '\0';
            }
        }

        // Extract device path (persistent identifier)
        if (devicePath && devicePathLen > 0)
        {
            WideCharToMultiByte(CP_UTF8, 0, targetName.monitorDevicePath, -1,
                               devicePath, devicePathLen, NULL, NULL);
        }

        result = 1;
        break;
    }

    free(paths);
    free(modes);
    return result;
}

static BOOL CALLBACK EnumMonitorCallback(HMONITOR hMonitor, HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData)
{
    MONITORINFOEXA mi;
    mi.cbSize = sizeof(MONITORINFOEXA);
    GetMonitorInfoA(hMonitor, (LPMONITORINFO)&mi);

    if (g_monitorCount < MAX_MONITOR_COUNT)
    {
        g_monitors[g_monitorCount].hMonitor = hMonitor;
        g_monitors[g_monitorCount].rect = *lprcMonitor;
        g_monitors[g_monitorCount].monitorIndex = g_monitorCount;
        g_monitors[g_monitorCount].isPrimary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;

        strncpy(g_monitors[g_monitorCount].deviceName, mi.szDevice, CCHDEVICENAME);
        g_monitors[g_monitorCount].deviceName[31] = '\0';

        DEVMODEA dm = {0};
        dm.dmSize = sizeof(DEVMODEA);
        if (EnumDisplaySettingsA(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm))
        {
            g_monitors[g_monitorCount].width = dm.dmPelsWidth;
            g_monitors[g_monitorCount].height = dm.dmPelsHeight;
        }
        else
        {
            g_monitors[g_monitorCount].width = lprcMonitor->right - lprcMonitor->left;
            g_monitors[g_monitorCount].height = lprcMonitor->bottom - lprcMonitor->top;
        }

        int gotIdentifiers = GetMonitorIdentifiers(
            mi.szDevice,
            g_monitors[g_monitorCount].friendlyName,
            sizeof(g_monitors[g_monitorCount].friendlyName),
            g_monitors[g_monitorCount].monitorDevicePath,
            sizeof(g_monitors[g_monitorCount].monitorDevicePath)
        );

        // Fallback: use GDI device name if DisplayConfig failed
        if (!gotIdentifiers || g_monitors[g_monitorCount].friendlyName[0] == '\0')
        {
            const char* fallbackName = mi.szDevice;
            if (strncmp(fallbackName, DEVICE_NAME_PREFIX, DEVICE_NAME_PREFIX_LEN) == 0)
            {
                fallbackName += DEVICE_NAME_PREFIX_LEN;
            }
            strncpy(g_monitors[g_monitorCount].friendlyName, fallbackName,
                    sizeof(g_monitors[g_monitorCount].friendlyName) - 1);
            g_monitors[g_monitorCount].friendlyName[sizeof(g_monitors[g_monitorCount].friendlyName) - 1] = '\0';
        }

        // Fallback: use GDI device name as device path if not available
        if (!gotIdentifiers || g_monitors[g_monitorCount].monitorDevicePath[0] == '\0')
        {
            strncpy(g_monitors[g_monitorCount].monitorDevicePath, mi.szDevice,
                    sizeof(g_monitors[g_monitorCount].monitorDevicePath) - 1);
            g_monitors[g_monitorCount].monitorDevicePath[sizeof(g_monitors[g_monitorCount].monitorDevicePath) - 1] = '\0';
        }

        snprintf(g_monitors[g_monitorCount].displayName,
                sizeof(g_monitors[g_monitorCount].displayName),
                "%s (%dx%d)%s",
                g_monitors[g_monitorCount].friendlyName,
                g_monitors[g_monitorCount].width,
                g_monitors[g_monitorCount].height,
                g_monitors[g_monitorCount].isPrimary ? " [Primary]" : "");

        g_monitorCount++;
    }

    return TRUE;
}

int GetMonitorIndexFromPoint(POINT pt)
{
    for (int i = 0; i < g_monitorCount; i++)
    {
        if (PtInRect(&g_monitors[i].rect, pt))
        {
            return i;
        }
    }
    return -1;
}

int GetMonitorIndexFromRect(RECT rect)
{
    POINT center = {
        (rect.left + rect.right) / 2,
        (rect.top + rect.bottom) / 2
    };
    return GetMonitorIndexFromPoint(center);
}

int IsAnyMonitorActive()
{
    for (int i = 0; i < g_monitorCount; i++)
    {
        if (g_monitorStates[i].screenSaverActive)
        {
            return 1;
        }
    }
    return 0;
}

int IsAnyMonitorEnabled()
{
    for (int i = 0; i < g_monitorCount; i++)
    {
        if (g_monitorStates[i].enabled)
        {
            return 1;
        }
    }
    return 0;
}

int FindMonitorByDeviceName(const char* deviceName)
{
    for (int i = 0; i < g_monitorCount; i++)
    {
        if (strcmp(g_monitors[i].deviceName, deviceName) == 0)
        {
            return i;
        }
    }
    return -1;
}

int FindMonitorByDevicePath(const char* devicePath)
{
    for (int i = 0; i < g_monitorCount; i++)
    {
        if (strcmp(g_monitors[i].monitorDevicePath, devicePath) == 0)
        {
            return i;
        }
    }
    return -1;
}

int FindPrimaryMonitorIndex()
{
    for (int i = 0; i < g_monitorCount; i++)
    {
        if (g_monitors[i].isPrimary)
        {
            return i;
        }
    }
    return -1;
}

void EnumerateMonitors()
{
    g_monitorCount = 0;
    EnumDisplayMonitors(NULL, NULL, EnumMonitorCallback, 0);
    LogMessage("Enumerated %d monitors", g_monitorCount);
}

