// media_windows.h - window evidence: one EnumWindows pass turns visible windows into per-monitor evidence for media.c's decision rules.

#ifndef MEDIA_WINDOWS_H
#define MEDIA_WINDOWS_H

typedef struct
{
    // Monitors hosting visible windows of audible-audio processes.
    int audibleMapped[MAX_MONITOR_COUNT];
    // Windows mapped via audible audio (ambiguity check: >1 means audio can't be attributed to a single window).
    int audibleMappedWindowCount;
    // Visible media windows with NO audio (muted candidates, e.g. hover previews), gated on per-monitor motion so paused windows don't block.
    int mutedRectCount;
    RECT mutedRects[MAX_BROWSER_WINDOW_INFO];
    // Visible browser windows examined: diagnostics for the mask-change log plus the single-browser-window rule (browserRects[0]).
    int browserWindowCount;
    char browserTitles[MAX_BROWSER_WINDOW_INFO][256];
    int browserMatched[MAX_BROWSER_WINDOW_INFO];  // title hint matched (diagnostic only)
    RECT browserRects[MAX_BROWSER_WINDOW_INFO];
} MediaWindowEvidence;

// Walk visible top-level windows and fill the evidence. Audio windows match by exe name, not PID (Chromium audio is in renderer processes).
void CollectWindowEvidence(MediaWindowEvidence* ev,
                           const char audioActiveProcessNames[][MAX_PATH],
                           int audioActiveProcessNameCount);

// Mark every monitor the window overlaps (min-area / min-overlap gates; falls back to the monitor holding the window center).
void MarkMediaWindowMonitors(int mediaOnMonitor[MAX_MONITOR_COUNT], const RECT* windowRect);

#endif // MEDIA_WINDOWS_H
