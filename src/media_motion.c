// media_motion.c - per-monitor motion probe: moving content => media (settles ambiguous audio). Frozen under the saver (no stale grace, no dim/undim race).

#include "oled_aegis.h"
#include "media_classify.h"
#include "media_motion.h"

#define MOTION_REGION_COUNT 4
#define MOTION_REGION_SIZE 32
#define MOTION_PIXELS (MOTION_REGION_SIZE * MOTION_REGION_SIZE)

static BYTE g_motionPrev[MAX_MONITOR_COUNT][MOTION_REGION_COUNT][MOTION_PIXELS * 4];
static DWORD g_motionLastTick[MAX_MONITOR_COUNT];

// Sample 4 inset 32x32 blocks; returns 1 if changed since last sample (tolerant threshold, so cursor/clock/DWM noise don't count). captureFails per region.
static int UpdateMonitorMotion(int monitorIndex, int* captureFails)
{
    RECT mr = g_monitors[monitorIndex].rect;
    int w = mr.right - mr.left;
    int h = mr.bottom - mr.top;
    if (w < MOTION_REGION_SIZE * 2 || h < MOTION_REGION_SIZE * 2) return 0;

    HDC hdcScreen = GetDC(NULL);
    if (!hdcScreen) return 0;

    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    HBITMAP hbm = CreateCompatibleBitmap(hdcScreen, MOTION_REGION_SIZE, MOTION_REGION_SIZE);
    int diff = 0;
    if (hdcMem && hbm)
    {
        HGDIOBJ hOld = SelectObject(hdcMem, hbm);
        // Quarter-point anchors, inset from the edges (taskbar/clock strips).
        int ax[4] = { w / 4, 3 * w / 4, w / 4, 3 * w / 4 };
        int ay[4] = { h / 4, h / 4, 3 * h / 4, 3 * h / 4 };
        for (int r = 0; r < MOTION_REGION_COUNT; r++)
        {
            int x = mr.left + ax[r] - MOTION_REGION_SIZE / 2;
            int y = mr.top + ay[r] - MOTION_REGION_SIZE / 2;
            if (!BitBlt(hdcMem, 0, 0, MOTION_REGION_SIZE, MOTION_REGION_SIZE,
                        hdcScreen, x, y, SRCCOPY))
            {
                if (captureFails) (*captureFails)++;
                continue;
            }
            BITMAPINFO bmi = {0};
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = MOTION_REGION_SIZE;
            bmi.bmiHeader.biHeight = -MOTION_REGION_SIZE;  // top-down
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biSizeImage = MOTION_PIXELS * 4;
            BYTE cur[MOTION_PIXELS * 4];
            if (GetDIBits(hdcMem, hbm, 0, MOTION_REGION_SIZE, cur, &bmi, DIB_RGB_COLORS) == MOTION_REGION_SIZE)
            {
                const BYTE* a = cur;
                const BYTE* b = g_motionPrev[monitorIndex][r];
                int changedPixels = 0;
                for (int p = 0; p < MOTION_PIXELS; p++)
                {
                    int d0 = (int)a[0] - (int)b[0]; if (d0 < 0) d0 = -d0;
                    int d1 = (int)a[1] - (int)b[1]; if (d1 < 0) d1 = -d1;
                    int d2 = (int)a[2] - (int)b[2]; if (d2 < 0) d2 = -d2;
                    if (d0 >= 8 || d1 >= 8 || d2 >= 8) changedPixels++;
                    a += 4;
                    b += 4;
                }
                if (changedPixels >= MOTION_PIXELS / 32)
                {
                    diff = 1;
                }
                memcpy(g_motionPrev[monitorIndex][r], cur, sizeof(cur));
            }
            else if (captureFails)
            {
                (*captureFails)++;
            }
        }
        SelectObject(hdcMem, hOld);
    }
    if (hbm) DeleteObject(hbm);
    if (hdcMem) DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);

    if (diff)
    {
        g_motionLastTick[monitorIndex] = GetTickCount();
        return 1;
    }
    return 0;
}

int IsForegroundFullscreenOnMonitor(int monitorIndex)
{
    HWND hFg = GetForegroundWindow();
    if (!hFg) return 0;
    RECT fr;
    if (!GetWindowRect(hFg, &fr)) return 0;

    RECT mr = g_monitors[monitorIndex].rect;
    LONG il = fr.left   > mr.left   ? fr.left   : mr.left;
    LONG it = fr.top    > mr.top    ? fr.top    : mr.top;
    LONG ir = fr.right  < mr.right  ? fr.right  : mr.right;
    LONG ib = fr.bottom < mr.bottom ? fr.bottom : mr.bottom;
    if (ir <= il || ib <= it) return 0;

    LONGLONG covered = (LONGLONG)(ir - il) * (ib - it);
    LONGLONG monArea = (LONGLONG)(mr.right - mr.left) * (mr.bottom - mr.top);
    return monArea > 0 && covered * 100 >= monArea * FULLSCREEN_FOREGROUND_MIN_COVERAGE_PCT;
}

// Foreground is a browser/media player: the fullscreen skip above guards games only - these must be sampled or a muted/quiet video loses its mapping.
static int IsForegroundMediaProcess(void)
{
    HWND hFg = GetForegroundWindow();
    if (!hFg) return 0;

    char processName[MAX_PATH] = {0};
    if (!GetProcessNameFromHwnd(hFg, processName, sizeof(processName))) return 0;

    MediaProcessClass cls = ClassifyProcess(processName);
    return cls == MEDIA_CLASS_BROWSER || cls == MEDIA_CLASS_VIDEO_PLAYER;
}

void RunMotionProbe(int motionDiff[MAX_MONITOR_COUNT],
                    int motionFails[MAX_MONITOR_COUNT],
                    DWORD motionAge[MAX_MONITOR_COUNT])
{
    int fgMediaProcess = IsForegroundMediaProcess();
    for (int i = 0; i < g_monitorCount; i++)
    {
        motionAge[i] = 0xFFFFFFFFu;
        if (IsForegroundFullscreenOnMonitor(i) && !fgMediaProcess)
        {
            continue;  // fullscreen game/unknown app: probe skipped (capture hitch)
        }
        HWND hSaver = g_monitorStates[i].hScreenSaverWnd;
        if (hSaver && IsWindowVisible(hSaver))
        {
            continue;  // black window up/fading: probe frozen, no grace
        }
        motionDiff[i] = UpdateMonitorMotion(i, &motionFails[i]);
        // Fresh tick: lastTick was set mid-scan on a diff; a scan-start tick would wrap and make fresh motion fail the caller's grace check.
        motionAge[i] = GetTickCount() - g_motionLastTick[i];
    }
}
