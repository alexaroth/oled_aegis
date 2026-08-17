// util.c - Shared helpers: path resolution, DPI scaling, cursor visibility. Part of OLED Aegis. See oled_aegis.h for shared types/constants.

#include "oled_aegis.h"

static char g_appDataPath[MAX_PATH];
static int g_appDataPathInitialized = 0;

static int IsAppUiActive()
{
    return g_app.trayMenuActive;
}

// Restore the cursor: ShowCursor is reference-counted and drifts (focus changes), so loop until visible or the safety bound; GetCursorInfo fallback.
void EnsureCursorVisible(const char* reason)
{
    int adjusted = 0;
    int count = 0;

    if (g_app.cursorHidden)
    {
        do
        {
            count = ShowCursor(TRUE);
            adjusted++;
        } while (count < 0 && adjusted < CURSOR_COUNTER_MAX_ATTEMPTS);

        g_app.cursorHidden = 0;
    }
    else
    {
        CURSORINFO cursorInfo = {0};
        cursorInfo.cbSize = sizeof(cursorInfo);
        if (GetCursorInfo(&cursorInfo) && (cursorInfo.flags & CURSOR_SHOWING) == 0)
        {
            do
            {
                count = ShowCursor(TRUE);
                adjusted++;
            } while (count < 0 && adjusted < CURSOR_COUNTER_MAX_ATTEMPTS);
        }
    }

    if (adjusted)
    {
        LogMessage("Cursor restored (%s, count=%d, adjustments=%d)",
                   reason ? reason : "unknown", count, adjusted);
    }
}

void HideCursorForScreenSaver(const char* reason)
{
    if (IsAppUiActive())
    {
        EnsureCursorVisible(reason ? reason : "app UI active");
        return;
    }

    int adjusted = 0;
    int count = 0;

    if (!g_app.cursorHidden)
    {
        do
        {
            count = ShowCursor(FALSE);
            adjusted++;
        } while (count >= 0 && adjusted < CURSOR_COUNTER_MAX_ATTEMPTS);
        g_app.cursorHidden = 1;
    }
    else
    {
        // cursorHidden is already 1, but the counter may have drifted back up (e.g. from sleep/wake) so the cursor is actually visible.
        CURSORINFO cursorInfo = {0};
        cursorInfo.cbSize = sizeof(cursorInfo);
        if (GetCursorInfo(&cursorInfo) && (cursorInfo.flags & CURSOR_SHOWING))
        {
            do
            {
                count = ShowCursor(FALSE);
                adjusted++;
            } while (count >= 0 && adjusted < CURSOR_COUNTER_MAX_ATTEMPTS);
        }
    }

    if (adjusted)
    {
        LogMessage("Cursor hidden (%s, count=%d, adjustments=%d)",
                   reason ? reason : "screen saver", count, adjusted);
    }
}

void GetAppDataPath(char* buffer, size_t bufferSize)
{
    if (!g_appDataPathInitialized)
    {
        SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, g_appDataPath);
        sprintf_s(g_appDataPath, sizeof(g_appDataPath), "%s\\OLED_Aegis", g_appDataPath);
        CreateDirectoryA(g_appDataPath, NULL);
        g_appDataPathInitialized = 1;
    }

    strncpy_s(buffer, bufferSize, g_appDataPath, _TRUNCATE);
}

