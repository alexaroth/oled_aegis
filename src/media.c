// media.c - Media detection core: ES_DISPLAY_REQUIRED gate, scan orchestration, rule table (policy), cache, logging. Owns DEFINE_GUIDs; sensors in media_*.

#include "oled_aegis.h"
#include "media_classify.h"
#include "media_audio.h"
#include "media_windows.h"
#include "media_motion.h"

static int g_mediaCacheInvalidated = 0;  // Set by WM_POWERBROADCAST to force media cache refresh

// Scan cache (file scope: the grace-keep rule needs the cached mask)
static DWORD g_lastLoggedMask = (DWORD)-1;
static DWORD g_lastScanTick = 0;
static int g_hasCachedState = 0;
static int g_cachedAnyMedia = 0;
static int g_cachedMediaOnMonitor[MAX_MONITOR_COUNT] = {0};
static DWORD g_lastAudioDetectedTick = 0;

// SDK headers only extern-declare these GUIDs; INITGUID (build /D) makes these DEFINE_GUID lines emit the definitions with SELECTANY storage.

DEFINE_GUID(CLSID_MMDeviceEnumerator,     0xBCDE0395, 0xE52F, 0x467C, 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E);
DEFINE_GUID(IID_IMMDeviceEnumerator,      0xA95664D2, 0x9614, 0x4F35, 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6);
DEFINE_GUID(IID_IAudioSessionManager2,    0x77AA99A0, 0x1BD6, 0x484F, 0x8B, 0xC7, 0x2C, 0x65, 0x4C, 0x9A, 0x9B, 0x6F);
DEFINE_GUID(IID_IAudioSessionControl2,    0xBFB7FF88, 0x7239, 0x4FC9, 0x8F, 0xA2, 0x07, 0xC9, 0x50, 0xBE, 0x9C, 0x6D);
DEFINE_GUID(IID_IAudioMeterInformation,   0xC02216F6, 0x8C67, 0x4B5B, 0x9D, 0x00, 0xD0, 0x08, 0xE7, 0x3E, 0x00, 0x64);

int IsMediaPlaying()
{
    static int lastMediaState = -1;

    if (!g_app.config.mediaDetectionEnabled)
    {
        return 0;
    }

    ULONG executionState = 0;
    NTSTATUS status = CallNtPowerInformation(
        SystemExecutionState,
        NULL, 0,
        &executionState, sizeof(executionState)
    );

    if (status == 0)
    {
        int isPlaying = (executionState & ES_DISPLAY_REQUIRED) != 0;
        // Only log when state changes to reduce noise
        if (isPlaying != lastMediaState)
        {
            LogMessage("Media detection: state changed to %s (executionState=0x%08X)",
                     isPlaying ? "PLAYING" : "NOT_PLAYING", executionState);
            lastMediaState = isPlaying;
        }
        return isPlaying;
    }

    LogMessage("Media detection: CallNtPowerInformation failed with status=%d", status);
    return 0;
}

// Get the process name (e.g., "explorer.exe") from a window handle. Returns 1 on success, 0 on failure
int GetProcessNameFromHwnd(HWND hWnd, char* buffer, int bufferSize)
{
    if (!hWnd || !buffer || bufferSize <= 0) return 0;

    DWORD pid = 0;
    GetWindowThreadProcessId(hWnd, &pid);
    if (pid == 0) return 0;

    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!hProcess)
    {
        // Try with fewer permissions
        hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!hProcess) return 0;
    }

    DWORD result = GetModuleBaseNameA(hProcess, NULL, buffer, bufferSize);
    CloseHandle(hProcess);

    return result > 0 ? 1 : 0;
}

// Reset media detection cache. Called after system sleep/wake to force a fresh scan, since WASAPI sessions and ES_DISPLAY_REQUIRED state may be stale.
void ResetMediaDetectionCache()
{
    g_mediaCacheInvalidated = 1;
}

// --- scan state and decision rules ---
// Rules run in array order; terminal ones end the scan. Order IS policy (bug history), do not reorder.

typedef enum
{
    REASON_NONE = 0,
    REASON_AUDIBLE_MAPPED,       // audible-audio window rects mapped monitors
    REASON_MOTION,               // only the motion probe flagged monitors
    REASON_MUTED_MAPPING,
    REASON_GRACE_KEEP,           // cached mask kept during an audio grace
    REASON_NO_AUDIO_SKIP,
    REASON_BROWSER_SKIP,
    REASON_AUDIO_ONLY_SKIP,
    REASON_FULLSCREEN_MAPPED,    // foreground fullscreen window of an audio-active process
    REASON_UNATTRIBUTED_SKIP     // audible audio with no attributable monitor (already a skip, no block-all)
} MediaScanReason;

static const char* const g_reasonNames[] = {
    "none", "audible", "motion", "muted", "grace",
    "no-audio", "browser", "audio-only", "fullscreen", "unattributed"
};

typedef struct
{
    int mediaOnMonitor[MAX_MONITOR_COUNT];  // mask being built
    char audioNames[MAX_ACTIVE_AUDIO_PIDS][MAX_PATH];
    DWORD audioPids[MAX_ACTIVE_AUDIO_PIDS];  // PID of the first audible session per exe
    int audioCount;
    int inAudioGrace;            // No audio this scan, but inside the post-audio grace window
    int mappedMonitorCount;      // Monitors mapped from window rects (audible/muted)
    MediaWindowEvidence ev;
    int motionDiff[MAX_MONITOR_COUNT];
    int motionFails[MAX_MONITOR_COUNT];
    DWORD motionAge[MAX_MONITOR_COUNT];
    // Decision trace for the mask-change log
    int unattributed, mutedMap, browserSkip, audioOnlySkip, noAudioSkip, graceKeep, clear;
    MediaScanReason reason;
} MediaScan;

static int CountMappedMonitors(const MediaScan* s)
{
    int n = 0;
    for (int i = 0; i < g_monitorCount; i++)
    {
        if (s->mediaOnMonitor[i]) n++;
    }
    return n;
}

static int AllAudioAre(const MediaScan* s, MediaProcessClass cls)
{
    if (s->audioCount == 0) return 0;
    for (int i = 0; i < s->audioCount; i++)
    {
        if (ClassifyProcess(s->audioNames[i]) != cls) return 0;
    }
    return 1;
}

static int AllAudioAreAudioOnlyOrBackground(const MediaScan* s)
{
    if (s->audioCount == 0) return 0;
    for (int i = 0; i < s->audioCount; i++)
    {
        MediaProcessClass c = ClassifyProcess(s->audioNames[i]);
        if (c != MEDIA_CLASS_AUDIO_ONLY && c != MEDIA_CLASS_BACKGROUND) return 0;
    }
    return 1;
}

// Rules: each returns 1 when it ends the scan (terminal), 0 to continue.

// R2: Audible audio with >1 visible window of the SAME audio-active process (e.g. a browser with two windows: its
// audio can't be attributed to one of them): drop the window mapping, the motion probe decides; fullscreen keeps it.
// Different audible processes on different monitors are NOT ambiguous (geometry maps a game and a Discord window each
// to its own monitor), so only the per-process count matters here.
static int RuleAmbiguityClear(MediaScan* s)
{
    if (s->audioCount == 0) return 0;
    int anyProcessAmbiguous = 0;
    for (int i = 0; i < s->audioCount && i < MAX_ACTIVE_AUDIO_PIDS; i++)
    {
        if (s->ev.audibleProcWindowCount[i] > 1)
        {
            anyProcessAmbiguous = 1;
            break;
        }
    }
    if (!anyProcessAmbiguous)
    {
        return 0;
    }
    s->clear = 1;
    for (int i = 0; i < g_monitorCount; i++)
    {
        if (IsForegroundFullscreenOnMonitor(i)) continue;
        s->mediaOnMonitor[i] = 0;
    }
    return 0;
}

// R3: Motion supplement: moving content counts as media without a window mapping (muted playback, ambiguous multi-window audio - see R2).
static int RuleMotionSupplement(MediaScan* s)
{
    RunMotionProbe(s->motionDiff, s->motionFails, s->motionAge);
    for (int i = 0; i < g_monitorCount; i++)
    {
        if (s->motionAge[i] < MOTION_GRACE_MS)
        {
            s->mediaOnMonitor[i] = 1;
        }
    }
    return 0;
}

// R4: Muted windows map only where content moves (paused = static, must not wake the saver); probe-skipped fullscreen monitors map outright (unambiguous).
static int RuleMutedMapping(MediaScan* s)
{
    if (s->mappedMonitorCount != 0) return 0;

    int canUse = 0;
    if (s->audioCount == 0)
    {
        // Only inside the grace window; otherwise a paused video blocks the saver.
        canUse = s->inAudioGrace && g_app.config.blockOnMutedMedia;
    }
    else if (!AllAudioAre(s, MEDIA_CLASS_BROWSER))
    {
        // Non-browser audio (Discord) doesn't explain ES_DISPLAY_REQUIRED; the media is likely a muted browser preview. Mapping beats an unattributed skip.
        canUse = 1;
    }
    if (!canUse) return 0;

    int mutedOnMonitor[MAX_MONITOR_COUNT] = {0};
    for (int w = 0; w < s->ev.mutedRectCount; w++)
    {
        MarkMediaWindowMonitors(mutedOnMonitor, &s->ev.mutedRects[w]);
    }
    for (int i = 0; i < g_monitorCount; i++)
    {
        if (!mutedOnMonitor[i] || s->mediaOnMonitor[i])
        {
            continue;
        }
        if (IsForegroundFullscreenOnMonitor(i) || s->motionAge[i] < MOTION_GRACE_MS)
        {
            s->mediaOnMonitor[i] = 1;
            s->mappedMonitorCount++;
        }
    }
    if (s->mappedMonitorCount > 0)
    {
        s->mutedMap = 1;
        s->reason = REASON_MUTED_MAPPING;
    }
    return 0;
}

// R5 (terminal): Fresh scan mapped nothing in the grace window (video minimized during quiet passage): keep the cached mask, unioned with fresh evidence.
static int RuleGraceKeep(MediaScan* s)
{
    if (s->mappedMonitorCount != 0 || !s->inAudioGrace) return 0;
    for (int i = 0; i < MAX_MONITOR_COUNT; i++)
    {
        if (!s->mediaOnMonitor[i])
        {
            s->mediaOnMonitor[i] = g_cachedMediaOnMonitor[i];
        }
    }
    s->graceKeep = 1;
    s->reason = REASON_GRACE_KEEP;
    return 1;
}

// R6 (terminal): Flag set but no audio and nothing mapped. The flag is global (wake locks, OBS, stale), so without recent audio: let the saver activate.
static int RuleNoAudioSkip(MediaScan* s)
{
    if (s->mappedMonitorCount != 0 || s->inAudioGrace || s->audioCount != 0) return 0;
    LogMessage("Media detection: no audible audio recently and nothing mapped: skipping media detection");
    s->noAudioSkip = 1;
    s->reason = REASON_NO_AUDIO_SKIP;
    return 1;
}

// R7 (terminal): Audio-active processes are all browsers but nothing mapped (no visible window, e.g. minimized tab): nothing to protect, skip media detection.
static int RuleBrowserSkip(MediaScan* s)
{
    if (s->mappedMonitorCount != 0 || !AllAudioAre(s, MEDIA_CLASS_BROWSER)) return 0;
    LogMessage("Media detection: no visible browser window, all audio-active processes are browsers: skipping media detection");
    s->browserSkip = 1;
    s->reason = REASON_BROWSER_SKIP;
    return 1;
}

// R8 (terminal): All audio-active processes are audio-only/background: music never needs the display, so it must not block the saver.
static int RuleAudioOnlySkip(MediaScan* s)
{
    if (s->mappedMonitorCount != 0 || !AllAudioAreAudioOnlyOrBackground(s)) return 0;
    LogMessage("Media detection: all audio-active processes are known audio-only apps: skipping media detection");
    s->audioOnlySkip = 1;
    s->reason = REASON_AUDIO_ONLY_SKIP;
    return 1;
}

// R9 (terminal): Foreground fullscreen window of an audio-active process: attribute the playback to its monitor(s) instead of blocking all.
// EAC-protected games fail the title-hint mapping and deny OpenProcess(VM_READ), so match the foreground window PID first.
static int RuleForegroundFullscreenMapping(MediaScan* s)
{
    if (s->mappedMonitorCount != 0 || s->audioCount == 0)
    {
        return 0;
    }

    HWND hFg = GetForegroundWindow();
    if (!hFg)
    {
        return 0;
    }

    DWORD fgPid = 0;
    GetWindowThreadProcessId(hFg, &fgPid);
    if (fgPid == 0)
    {
        return 0;
    }

    int fgIsAudioActive = 0;
    for (int i = 0; i < s->audioCount; i++)
    {
        if (s->audioPids[i] == fgPid)
        {
            fgIsAudioActive = 1;
            break;
        }
    }

    // Same exe with a second session carries a PID not recorded above (first-session PID only): fall back to a name match.
    if (!fgIsAudioActive)
    {
        char fgProcess[MAX_PATH] = {0};
        if (GetProcessNameFromHwnd(hFg, fgProcess, sizeof(fgProcess)))
        {
            for (int i = 0; i < s->audioCount; i++)
            {
                if (_stricmp(fgProcess, s->audioNames[i]) == 0)
                {
                    fgIsAudioActive = 1;
                    break;
                }
            }
        }
    }
    if (!fgIsAudioActive)
    {
        return 0;
    }

    for (int i = 0; i < g_monitorCount; i++)
    {
        if (IsForegroundFullscreenOnMonitor(i))
        {
            s->mediaOnMonitor[i] = 1;
        }
    }
    s->mappedMonitorCount = CountMappedMonitors(s);
    if (s->mappedMonitorCount > 0)
    {
        s->reason = REASON_FULLSCREEN_MAPPED;
        return 1;
    }
    return 0;
}

// R10 (terminal): Audible audio with nothing attributable (no visible window, no fullscreen foreground). A real sound
// we can't place is background/UI noise or hidden media - never keep the OLED awake for it (per-monitor burns are the
// whole point; a sound we can't find a monitor for must not override that). The motion probe already ran (R3), so any
// visible motion gets its own grace before this terminal skip.
static int RuleUnattributedSkip(MediaScan* s)
{
    if (s->mappedMonitorCount != 0) return 0;
    LogMessage("Media detection: audible audio with no attributable monitor: skipping (no monitor blocked)");
    s->unattributed = 1;
    s->reason = REASON_UNATTRIBUTED_SKIP;
    return 1;
}

typedef struct
{
    int (*apply)(MediaScan* s);
    int terminal;
} MediaRule;

static const MediaRule g_rules[] = {
    { RuleAmbiguityClear,       0 },
    { RuleMotionSupplement,     0 },
    { RuleMutedMapping,         0 },
    { RuleGraceKeep,            1 },
    { RuleNoAudioSkip,          1 },
    { RuleBrowserSkip,          1 },
    { RuleAudioOnlySkip,        1 },
    { RuleForegroundFullscreenMapping, 1 },
    { RuleUnattributedSkip,     1 },
};

// --- scan orchestration ---
// Fill mediaOnMonitor[] via the ES_DISPLAY_REQUIRED gate + cached scan; 1 if any monitor has media.
int UpdateMediaMonitorStates(int mediaOnMonitor[MAX_MONITOR_COUNT])
{
    for (int i = 0; i < MAX_MONITOR_COUNT; i++)
    {
        mediaOnMonitor[i] = 0;
    }

    if (g_mediaCacheInvalidated)
    {
        g_hasCachedState = 0;
        g_lastLoggedMask = (DWORD)-1;
        g_mediaCacheInvalidated = 0;
        LogMessage("Media detection cache invalidated (sleep/wake)");
    }

    if (!g_app.config.mediaDetectionEnabled)
    {
        g_hasCachedState = 0;
        if (g_lastLoggedMask != 0)
        {
            LogMessage("Media monitor detection: disabled");
            g_lastLoggedMask = 0;
        }
        return 0;
    }

    DWORD nowTick = GetTickCount();
    if (g_hasCachedState && (DWORD)(nowTick - g_lastScanTick) < MEDIA_DETECTION_CACHE_MS)
    {
        for (int i = 0; i < MAX_MONITOR_COUNT; i++)
        {
            mediaOnMonitor[i] = g_cachedMediaOnMonitor[i];
        }
        return g_cachedAnyMedia;
    }

    g_lastScanTick = nowTick;

    int globalMediaPlaying = IsMediaPlaying();

    if (!globalMediaPlaying)
    {
        for (int i = 0; i < MAX_MONITOR_COUNT; i++)
        {
            g_cachedMediaOnMonitor[i] = 0;
        }
        g_hasCachedState = 1;
        g_cachedAnyMedia = 0;

        if (g_lastLoggedMask != 0)
        {
            LogMessage("Media monitor detection: no active media monitors");
            g_lastLoggedMask = 0;
        }
        return 0;
    }

    MediaScan scan;
    memset(&scan, 0, sizeof(scan));

    scan.audioCount = CollectActiveAudioProcessNames(scan.audioNames, scan.audioPids, MAX_ACTIVE_AUDIO_PIDS);

    if (scan.audioCount > 0) 
    {
        g_lastAudioDetectedTick = nowTick;
    } 
    else if (g_hasCachedState && g_cachedAnyMedia && (DWORD)(nowTick - g_lastAudioDetectedTick) < AUDIO_GRACE_PERIOD_MS) 
    {
        // Quiet passage: still scan windows (an early return kept the mapping stale ~30s after a video moved monitors); cache kept only if unmapped.
        scan.inAudioGrace = 1;
        g_lastScanTick = nowTick;
    }

    CollectWindowEvidence(&scan.ev, scan.audioNames, scan.audioCount);

    // Audible-audio window mapping becomes the starting mask.
    for (int i = 0; i < g_monitorCount; i++) 
    {
        if (scan.ev.audibleMapped[i]) 
        {
            scan.mediaOnMonitor[i] = 1;
        }
    }
    scan.mappedMonitorCount = CountMappedMonitors(&scan);

    for (int r = 0; r < (int)(sizeof(g_rules) / sizeof(g_rules[0])); r++) 
    {
        if (g_rules[r].apply(&scan) && g_rules[r].terminal) 
        {
            break;
        }
    }

    if (scan.reason == REASON_NONE) 
    {
        if (scan.mappedMonitorCount > 0) 
        {
            scan.reason = REASON_AUDIBLE_MAPPED;
        } 
        else 
        {
            int anyMask = 0;
            for (int i = 0; i < g_monitorCount; i++) 
            {
                if (scan.mediaOnMonitor[i])
                {
                    anyMask = 1;
                    break;
                }
            }
            scan.reason = anyMask ? REASON_MOTION : REASON_NONE;
        }
    }

    DWORD mask = 0;
    for (int i = 0; i < g_monitorCount && i < 32; i++) 
    {
        if (scan.mediaOnMonitor[i]) 
        {
            mask |= (1u << i);
        }
    }

    g_hasCachedState = 1;
    g_cachedAnyMedia = mask != 0;
    for (int i = 0; i < MAX_MONITOR_COUNT; i++)
    {
        g_cachedMediaOnMonitor[i] = scan.mediaOnMonitor[i];
        mediaOnMonitor[i] = scan.mediaOnMonitor[i];
    }

    if (mask != g_lastLoggedMask)
    {
        for (int i = 0; i < scan.ev.browserWindowCount; i++)
        {
            LogMessage("Media detection: browser window MAPPED: '%.120s'", scan.ev.browserTitles[i]);
        }
        for (int i = 0; i < scan.audioCount; i++)
        {
            LogMessage("Media detection: active audio process: %s", scan.audioNames[i]);
        }
        LogMessage("Media monitor detection: mask=0x%08X (activeAudioNames=%d, unattr=%d, mutedMap=%d, "
                   "browserSkip=%d, audioOnlySkip=%d, noAudioSkip=%d, browserWindows=%d, graceKeep=%d, "
                   "clear=%d, motion=[%d %d %d] fails=[%d %d %d] age=[%u %u %u], reason=%s)",
                   mask, scan.audioCount, scan.unattributed, scan.mutedMap,
                   scan.browserSkip, scan.audioOnlySkip, scan.noAudioSkip,
                   scan.ev.browserWindowCount, scan.graceKeep, scan.clear,
                   scan.motionDiff[0], scan.motionDiff[1], scan.motionDiff[2],
                   scan.motionFails[0], scan.motionFails[1], scan.motionFails[2],
                   scan.motionAge[0], scan.motionAge[1], scan.motionAge[2],
                   g_reasonNames[scan.reason]);
        g_lastLoggedMask = mask;
    }

    return g_cachedAnyMedia;
}
