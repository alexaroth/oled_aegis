// media_windows.c - window evidence: which visible windows belong to audio-active/media processes and where they are (pure sensor, no policy).

#include "oled_aegis.h"
#include "media_classify.h"
#include "media_windows.h"

static int IsWindowCloakedCompat(HWND hWnd)
{
    DWORD cloaked = 0;
    HRESULT hr = DwmGetWindowAttribute(hWnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    return SUCCEEDED(hr) && cloaked != 0;
}

static LONGLONG RectArea(const RECT* rect)
{
    LONG width = rect->right - rect->left;
    LONG height = rect->bottom - rect->top;

    if (width <= 0 || height <= 0)
    {
        return 0;
    }

    return (LONGLONG)width * (LONGLONG)height;
}

static LONGLONG RectIntersectionArea(const RECT* a, const RECT* b)
{
    LONG left = a->left > b->left ? a->left : b->left;
    LONG top = a->top > b->top ? a->top : b->top;
    LONG right = a->right < b->right ? a->right : b->right;
    LONG bottom = a->bottom < b->bottom ? a->bottom : b->bottom;

    if (right <= left || bottom <= top)
    {
        return 0;
    }

    return (LONGLONG)(right - left) * (LONGLONG)(bottom - top);
}

// Prefer DWM's extended frame bounds (accounts for invisible drop-shadow borders); fall back to GetWindowRect if the DWM query fails.
static int GetVisibleWindowRect(HWND hWnd, RECT* rect)
{
    RECT frameRect;
    HRESULT hr = DwmGetWindowAttribute(hWnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frameRect, sizeof(frameRect));
    if (SUCCEEDED(hr) && RectArea(&frameRect) > 0)
    {
        *rect = frameRect;
        return 1;
    }

    return GetWindowRect(hWnd, rect) != 0;
}

void MarkMediaWindowMonitors(int mediaOnMonitor[MAX_MONITOR_COUNT], const RECT* windowRect)
{
    LONGLONG windowArea = RectArea(windowRect);

    if (windowArea < MIN_MEDIA_WINDOW_AREA)
    {
        return;
    }

    int marked = 0;
    for (int i = 0; i < g_monitorCount; i++)
    {
        LONGLONG intersectionArea = RectIntersectionArea(windowRect, &g_monitors[i].rect);
        double overlapRatio = (double)intersectionArea / (double)windowArea;
        if (intersectionArea >= MIN_MEDIA_WINDOW_AREA && overlapRatio >= MIN_MEDIA_WINDOW_OVERLAP_RATIO)
        {
            mediaOnMonitor[i] = 1;
            marked = 1;
        }
    }

    if (!marked)
    {
        int monitorIndex = GetMonitorIndexFromRect(*windowRect);
        if (monitorIndex >= 0 && monitorIndex < g_monitorCount)
        {
            mediaOnMonitor[monitorIndex] = 1;
        }
    }
}

typedef struct
{
    MediaWindowEvidence* ev;
    const char (*audioNames)[MAX_PATH];
    int audioCount;
} EnumCtx;

static int IsAudioActiveProcessName(const EnumCtx* ctx, const char* processName)
{
    if (!processName || !ctx) return 0;
    for (int i = 0; i < ctx->audioCount; i++)
    {
        if (_stricmp(ctx->audioNames[i], processName) == 0)
        {
            return 1;
        }
    }
    return 0;
}

static BOOL CALLBACK EnumMediaWindowCallback(HWND hWnd, LPARAM lParam)
{
    EnumCtx* ctx = (EnumCtx*)lParam;
    MediaWindowEvidence* ev = ctx->ev;

    if (!IsWindowVisible(hWnd) || IsIconic(hWnd) || IsWindowCloakedCompat(hWnd))
    {
        return TRUE;
    }

    LONG_PTR exStyle = GetWindowLongPtr(hWnd, GWL_EXSTYLE);
    if ((exStyle & WS_EX_TOOLWINDOW) != 0)
    {
        return TRUE;
    }

    RECT rect;
    if (!GetVisibleWindowRect(hWnd, &rect))
    {
        return TRUE;
    }

    if (RectArea(&rect) <= 0)
    {
        return TRUE;
    }

    char processName[MAX_PATH] = {0};
    if (!GetProcessNameFromHwnd(hWnd, processName, sizeof(processName)))
    {
        return TRUE;
    }

    MediaProcessClass cls = ClassifyProcess(processName);
    int isAudioActive = IsAudioActiveProcessName(ctx, processName);

    // Audio-active processes count as media; with blockOnMutedMedia, visible browser/media windows without audio become muted candidates (hover previews).
    int isMutedCandidate = !isAudioActive &&
                           (cls == MEDIA_CLASS_BROWSER || cls == MEDIA_CLASS_VIDEO_PLAYER) &&
                           g_app.config.blockOnMutedMedia;

    if (!isAudioActive && !isMutedCandidate)
    {
        return TRUE;
    }

    char title[512] = {0};
    GetWindowTextA(hWnd, title, sizeof(title));

    // Muted candidates count outright: the visible window IS the media.
    int matched = 1;
    int hintMatched = 1;  // Title-hint result, for the diagnostic log only
    if (isAudioActive)
    {
        // Browser audio belongs to renderer PIDs, so map every visible window (hints miss sites like Aniwave); non-browser apps still need a hint.
        hintMatched = WindowCountsAsMedia(processName, title);
        if (cls != MEDIA_CLASS_BROWSER)
        {
            matched = hintMatched;
        }
    }

    // Diagnostic info for all browser windows considered (audio-active or muted candidates), logged once in media.c when the mask changes.
    if (cls == MEDIA_CLASS_BROWSER && title[0] && ev->browserWindowCount < MAX_BROWSER_WINDOW_INFO)
    {
        int idx = ev->browserWindowCount++;
        strncpy(ev->browserTitles[idx], title, 255);
        ev->browserTitles[idx][255] = '\0';
        ev->browserMatched[idx] = hintMatched;
        ev->browserRects[idx] = rect;
    }

    if (!matched)
    {
        return TRUE;
    }

    if (isAudioActive)
    {
        ev->audibleMappedWindowCount++;
        MarkMediaWindowMonitors(ev->audibleMapped, &rect);
    }
    else if (ev->mutedRectCount < MAX_BROWSER_WINDOW_INFO)
    {
        ev->mutedRects[ev->mutedRectCount++] = rect;
    }
    return TRUE;
}

void CollectWindowEvidence(MediaWindowEvidence* ev, const char audioActiveProcessNames[][MAX_PATH], int audioActiveProcessNameCount)
{
    memset(ev, 0, sizeof(*ev));

    EnumCtx ctx;
    ctx.ev = ev;
    ctx.audioNames = audioActiveProcessNames;
    ctx.audioCount = audioActiveProcessNameCount;
    EnumWindows(EnumMediaWindowCallback, (LPARAM)&ctx);
}
