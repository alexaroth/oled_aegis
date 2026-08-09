// media.c - Media playback detection: ES_DISPLAY_REQUIRED gate, WASAPI
// audio-session scanning, and window-to-monitor mapping. Also owns the
// DEFINE_GUID definitions for the MMDevice / audio-session interfaces.
//
// Part of OLED Aegis. See oled_aegis.h for the shared types/constants.

#include "oled_aegis.h"

static int g_mediaCacheInvalidated = 0;  // Set by WM_POWERBROADCAST to force media cache refresh

// The MMDevice / audio-session GUIDs are only extern-declared in the SDK
// headers, not DEFINE_GUID'd, so they don't resolve at link time. INITGUID is
// defined via the build command (/D "INITGUID"), so these DEFINE_GUID lines
// emit the actual definitions with SELECTANY storage.

DEFINE_GUID(CLSID_MMDeviceEnumerator,     0xBCDE0395, 0xE52F, 0x467C, 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E);
DEFINE_GUID(IID_IMMDeviceEnumerator,      0xA95664D2, 0x9614, 0x4F35, 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6);
DEFINE_GUID(IID_IAudioSessionManager2,    0x77AA99A0, 0x1BD6, 0x484F, 0x8B, 0xC7, 0x2C, 0x65, 0x4C, 0x9A, 0x9B, 0x6F);
DEFINE_GUID(IID_IAudioSessionControl2,    0xBFB7FF88, 0x7239, 0x4FC9, 0x8F, 0xA2, 0x07, 0xC9, 0x50, 0xBE, 0x9C, 0x6D);
DEFINE_GUID(IID_IAudioMeterInformation,   0xC02216F6, 0x8C67, 0x4B5B, 0x9D, 0x00, 0xD0, 0x08, 0xE7, 0x3E, 0x00, 0x64);
int IsMediaPlaying() {
    static int lastMediaState = -1;

    if (!g_app.config.mediaDetectionEnabled) {
        return 0;
    }

    ULONG executionState = 0;
    NTSTATUS status = CallNtPowerInformation(
        SystemExecutionState,
        NULL, 0,
        &executionState, sizeof(executionState)
    );

    if (status == 0) {
        int isPlaying = (executionState & ES_DISPLAY_REQUIRED) != 0;
        // Only log when state changes to reduce noise
        if (isPlaying != lastMediaState) {
            LogMessage("Media detection: state changed to %s (executionState=0x%08X)",
                     isPlaying ? "PLAYING" : "NOT_PLAYING", executionState);
            lastMediaState = isPlaying;
        }
        return isPlaying;
    }

    LogMessage("Media detection: CallNtPowerInformation failed with status=%d", status);
    return 0;
}

// Get the process name (e.g., "explorer.exe") from a window handle
// Returns 1 on success, 0 on failure
int GetProcessNameFromHwnd(HWND hWnd, char* buffer, int bufferSize) {
    if (!hWnd || !buffer || bufferSize <= 0) return 0;

    DWORD pid = 0;
    GetWindowThreadProcessId(hWnd, &pid);
    if (pid == 0) return 0;

    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!hProcess) {
        // Try with fewer permissions
        hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!hProcess) return 0;
    }

    DWORD result = GetModuleBaseNameA(hProcess, NULL, buffer, bufferSize);
    CloseHandle(hProcess);

    return result > 0 ? 1 : 0;
}

// Case-insensitive substring search (MSVC _strnicmp-based)
static int ContainsIgnoreCase(const char* haystack, const char* needle) {
    if (!haystack || !needle) return 0;

    size_t needleLen = strlen(needle);
    if (needleLen == 0) return 1;

    for (const char* p = haystack; *p; p++) {
        if (_strnicmp(p, needle, needleLen) == 0) {
            return 1;
        }
    }

    return 0;
}

static int ProcessNameMatchesAny(const char* processName, const char* const* names, int count) {
    if (!processName) return 0;

    for (int i = 0; i < count; i++) {
        if (_stricmp(processName, names[i]) == 0) {
            return 1;
        }
    }

    return 0;
}

static int IsKnownBrowserProcess(const char* processName) {
    static const char* const browserProcesses[] = {
        "chrome.exe",
        "msedge.exe",
        "firefox.exe",
        "brave.exe",
        "opera.exe",
        "opera_gx.exe",
        "vivaldi.exe",
        "arc.exe",
        "thorium.exe",
        "zen.exe"
    };

    return ProcessNameMatchesAny(
        processName,
        browserProcesses,
        (int)(sizeof(browserProcesses) / sizeof(browserProcesses[0]))
    );
}

// Known VIDEO player processes. Audio-only apps (Spotify, iTunes, etc.) are
// intentionally excluded: music playback does not keep the display on, so an
// open music player should not block the screen saver on its monitor. Video
// players are still gated by an active-audio check before they count as media.
static int IsKnownMediaProcess(const char* processName) {
    static const char* const mediaProcesses[] = {
        "vlc.exe",
        "mpv.exe",
        "mpvnet.exe",
        "potplayer.exe",
        "potplayermini.exe",
        "potplayermini64.exe",
        "wmplayer.exe",
        "mpc-hc.exe",
        "mpc-hc64.exe",
        "mpc-be.exe",
        "mpc-be64.exe",
        "kodi.exe",
        "plex.exe",
        "jellyfinmediaplayer.exe",
        "embytheater.exe",
        "video.ui.exe"
    };

    return ProcessNameMatchesAny(
        processName,
        mediaProcesses,
        (int)(sizeof(mediaProcesses) / sizeof(mediaProcesses[0]))
    );
}

// Known AUDIO-ONLY (music/podcast) applications. These play sound but never
// need the display kept on, so they must not block the screen saver on any
// monitor. Their audible audio is also not evidence of video playback, so it
// must not trigger the "unknown audio -> block everything" fallback in
// UpdateMediaMonitorStates (which would otherwise wake an idle OLED on another
// monitor when music is paused or unpaused elsewhere).
static int IsKnownAudioOnlyProcess(const char* processName) {
    static const char* const audioOnlyProcesses[] = {
        "spotify.exe",
        "itunes.exe",
        "music.exe",           // Apple Music for Windows
        "foobar2000.exe",
        "winamp.exe",
        "musicbee.exe",
        "deezer.exe",
        "tidal.exe",
        "qobuz.exe",
        "amazon music.exe",
        "youtube music.exe",
        "groove music.exe",
        "mediamonkey.exe",
        "clementine.exe",
        "strawberry.exe",
        "audacious.exe"
    };

    return ProcessNameMatchesAny(
        processName,
        audioOnlyProcesses,
        (int)(sizeof(audioOnlyProcesses) / sizeof(audioOnlyProcesses[0]))
    );
}

// Title hints for VIDEO playback sites. Audio-only services (Spotify,
// SoundCloud, Bandcamp, Apple Music) are excluded since music does not keep
// the display on. "YouTube Music" is covered by the "YouTube" hint.
static int WindowTitleHasMediaHint(const char* title) {
    static const char* const mediaTitleHints[] = {
        "YouTube",
        "Twitch",
        "Netflix",
        "Hulu",
        "Disney+",
        "Prime Video",
        "Amazon Prime",
        "HBO Max",
        "Paramount+",
        "Peacock",
        "Crunchyroll",
        "Vimeo",
        "Dailymotion",
        "Plex",
        "Jellyfin",
        "Emby",
        "Media Player",
        "VLC media player",
        "Picture in picture",
        "TikTok",
        "/ X"           // For x.com titles are "Home / X", "@user / X", "user on X: ... / X"
    };

    if (!title || title[0] == '\0') return 0;

    for (int i = 0; i < (int)(sizeof(mediaTitleHints) / sizeof(mediaTitleHints[0])); i++) {
        if (ContainsIgnoreCase(title, mediaTitleHints[i])) {
            return 1;
        }
    }

    return 0;
}

// Returns 1 if the (processName, title) pair looks like a media-playing window,
// 0 otherwise. Known media players always count; browsers count only with a
// video-site title hint; any other process counts only with a title hint.
//
// NOTE: for audible browser audio this is diagnostic only (see
// EnumMediaWindowCallback): the hint whitelist misses sites (e.g. Aniwave), so
// every visible window of an audio-active browser process maps regardless.
static int IsMediaCandidateWindow(const char* processName, const char* title) {
    if (IsKnownMediaProcess(processName)) {
        return 1;
    }

    if (IsKnownBrowserProcess(processName)) {
        return WindowTitleHasMediaHint(title) ? 1 : 0;
    }

    return WindowTitleHasMediaHint(title) ? 1 : 0;
}

static int IsWindowCloakedCompat(HWND hWnd) {
    DWORD cloaked = 0;
    HRESULT hr = DwmGetWindowAttribute(hWnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    return SUCCEEDED(hr) && cloaked != 0;
}

// Get the process name (e.g., "brave.exe") from a process ID.
// Uses QueryFullProcessImageNameW (needs only PROCESS_QUERY_LIMITED_INFORMATION)
// instead of GetModuleBaseNameA (needs PROCESS_VM_READ, which sandboxed
// Chromium renderer processes deny). Returns 1 on success, 0 on failure.
static int GetProcessNameFromPid(DWORD pid, char* buffer, int bufferSize) {
    if (!buffer || bufferSize <= 0 || pid == 0) return 0;

    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProcess) return 0;

    WCHAR wpath[MAX_PATH] = {0};
    DWORD wpathLen = MAX_PATH;
    BOOL ok = QueryFullProcessImageNameW(hProcess, 0, wpath, &wpathLen);
    CloseHandle(hProcess);

    if (!ok || wpathLen == 0) return 0;

    // Extract filename from full path (e.g. "C:\...\brave.exe" -> "brave.exe")
    WCHAR* wexe = wpath;
    for (WCHAR* p = wpath; *p; p++) {
        if (*p == L'\\') wexe = p + 1;
    }

    // Convert to narrow char (exe names are ASCII)
    int i = 0;
    for (; wexe[i] && i < bufferSize - 1; i++) {
        buffer[i] = (char)wexe[i];
    }
    buffer[i] = '\0';

    return buffer[0] != '\0' ? 1 : 0;
}

// Context passed to EnumMediaWindowCallback. Carries the per-monitor media
// flags being built plus the set of process names currently emitting audio.
// We match by name (not PID) because Chromium browsers (Chrome/Brave/Edge/etc.)
// run multi-process: the audio session's PID is the renderer process, while the
// browser window is owned by the main process. Both share the same exe name, so
// name matching bridges the gap. Single-process players (VLC, mpv) match too.
typedef struct {
    int mediaOnMonitor[MAX_MONITOR_COUNT];
    char audioActiveProcessNames[MAX_ACTIVE_AUDIO_PIDS][MAX_PATH];
    int audioActiveProcessNameCount;
    // Diagnostic info for the mask-change log: which browser windows were
    // examined, their hint status, and rects (rects also map hint-less media).
    char browserTitles[MAX_BROWSER_WINDOW_INFO][256];
    int browserMatched[MAX_BROWSER_WINDOW_INFO];  // 1 = hint matched, 0 = no hint (diagnostic only)
    int browserMapped[MAX_BROWSER_WINDOW_INFO];   // 1 = window was mapped as media, 0 = skipped
    RECT browserRects[MAX_BROWSER_WINDOW_INFO];   // window rects, used to map media to monitors without title hints
    int browserWindowCount;
    int audibleMappedWindowCount;  // Windows mapped via audible audio (ambiguity check)
    // Rects of visible media windows with no audible audio (muted-media
    // candidates, e.g. YouTube hover previews). Kept per window so the muted
    // mapping can be gated on per-monitor motion: a paused window must not
    // block the screen saver (its site wake lock keeps ES_DISPLAY_REQUIRED
    // set, but static content here means nothing is playing here).
    RECT mutedRects[MAX_BROWSER_WINDOW_INFO];
    int mutedRectCount;
} MediaEnumContext;

static int IsAudioActiveProcessName(const MediaEnumContext* ctx, const char* processName) {
    if (!processName || !ctx) return 0;
    for (int i = 0; i < ctx->audioActiveProcessNameCount; i++) {
        if (_stricmp(ctx->audioActiveProcessNames[i], processName) == 0) {
            return 1;
        }
    }
    return 0;
}

// Returns 1 if every audio-active process is a known browser. Used to decide
// whether to skip the block-all fallback: when a browser has active audio but
// no window title matches a video hint (e.g. video in a background tab), we
// can't determine which monitor is playing. For browsers, not blocking is
// preferable to blocking everything, since the user's use case is to let the
// OLED sleep when video plays elsewhere. For non-browser apps (unknown apps,
// media players with minimized windows), we keep the safe block-all fallback.
static int AllAudioActiveAreBrowsers(const MediaEnumContext* ctx) {
    if (ctx->audioActiveProcessNameCount == 0) return 0;
    for (int i = 0; i < ctx->audioActiveProcessNameCount; i++) {
        if (!IsKnownBrowserProcess(ctx->audioActiveProcessNames[i])) {
            return 0;
        }
    }
    return 1;
}

// Returns 1 if every audio-active process is a known audio-only app (music/
// podcast). Used to skip the block-all fallback: audio-only apps never need
// the display on and their windows are deliberately not mapped as media (see
// IsKnownMediaProcess), so audible music alone must not block the screen
// saver - including waking an idle OLED on another monitor when playback is
// started or stopped. Mirrors the browser handling above.
static int AllAudioActiveAreAudioOnly(const MediaEnumContext* ctx) {
    if (ctx->audioActiveProcessNameCount == 0) return 0;
    for (int i = 0; i < ctx->audioActiveProcessNameCount; i++) {
        if (!IsKnownAudioOnlyProcess(ctx->audioActiveProcessNames[i])) {
            return 0;
        }
    }
    return 1;
}

// Collect exe names of processes with an ACTIVE audio session on the default
// render endpoint. Returns the count filled into names[] (0 on any failure,
// which causes the caller's safe fallback to block all enabled monitors).
static int CollectActiveAudioProcessNames(char names[][MAX_PATH], int maxNames) {
    if (!names || maxNames <= 0) return 0;

    IMMDeviceEnumerator* pEnum = NULL;
    IMMDevice* pDevice = NULL;
    IAudioSessionManager2* pSessionManager = NULL;
    IAudioSessionEnumerator* pSessionEnum = NULL;
    int count = 0;

    HRESULT hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                                  &IID_IMMDeviceEnumerator, (void**)&pEnum);
    if (FAILED(hr) || !pEnum) {
        LogMessage("Audio: CoCreateInstance failed hr=0x%08X", (unsigned)hr);
        goto done;
    }

    hr = pEnum->lpVtbl->GetDefaultAudioEndpoint(pEnum, eRender, eConsole, &pDevice);
    if (FAILED(hr) || !pDevice) {
        LogMessage("Audio: GetDefaultAudioEndpoint(eRender,eConsole) failed hr=0x%08X", (unsigned)hr);
        goto done;
    }

    hr = pDevice->lpVtbl->Activate(pDevice, &IID_IAudioSessionManager2,
                                   CLSCTX_ALL, NULL, (void**)&pSessionManager);
    if (FAILED(hr) || !pSessionManager) {
        LogMessage("Audio: Activate(IAudioSessionManager2) failed hr=0x%08X", (unsigned)hr);
        goto done;
    }

    hr = pSessionManager->lpVtbl->GetSessionEnumerator(pSessionManager, &pSessionEnum);
    if (FAILED(hr) || !pSessionEnum) {
        LogMessage("Audio: GetSessionEnumerator failed hr=0x%08X", (unsigned)hr);
        goto done;
    }

    int sessionCount = 0;
    pSessionEnum->lpVtbl->GetCount(pSessionEnum, &sessionCount);

    for (int i = 0; i < sessionCount && count < maxNames; i++) {
        IAudioSessionControl* pControl = NULL;
        if (FAILED(pSessionEnum->lpVtbl->GetSession(pSessionEnum, i, &pControl)) || !pControl) {
            continue;
        }

        AudioSessionState state = AudioSessionStateInactive;
        pControl->lpVtbl->GetState(pControl, &state);

        if (state == AudioSessionStateActive) {
            // AudioSessionStateActive can be true even when a video is paused
            // (the session stays "active" but produces no sound). Use the peak
            // meter to filter out silent sessions so paused video doesn't block
            // the screen saver.
            IAudioMeterInformation* pMeter = NULL;
            if (SUCCEEDED(pControl->lpVtbl->QueryInterface(pControl, &IID_IAudioMeterInformation, (void**)&pMeter)) && pMeter) {
                float peak = 0.0f;
                if (SUCCEEDED(pMeter->lpVtbl->GetPeakValue(pMeter, &peak)) &&
                    peak > AUDIO_ACTIVE_PEAK_THRESHOLD) {
                    IAudioSessionControl2* pControl2 = NULL;
                    if (SUCCEEDED(pControl->lpVtbl->QueryInterface(pControl, &IID_IAudioSessionControl2,
                                                                   (void**)&pControl2)) && pControl2) {
                        DWORD pid = 0;
                        if (SUCCEEDED(pControl2->lpVtbl->GetProcessId(pControl2, &pid)) && pid != 0) {
                            char procName[MAX_PATH] = {0};
                            if (GetProcessNameFromPid(pid, procName, sizeof(procName))) {
                                int found = 0;
                                for (int j = 0; j < count; j++) {
                                    if (_stricmp(names[j], procName) == 0) { found = 1; break; }
                                }
                                if (!found) {
                                    strncpy(names[count], procName, MAX_PATH - 1);
                                    names[count][MAX_PATH - 1] = '\0';
                                    count++;
                                }
                            }
                        }
                        pControl2->lpVtbl->Release(pControl2);
                    }
                }
                pMeter->lpVtbl->Release(pMeter);
            }
        }
        pControl->lpVtbl->Release(pControl);
    }

done:
    if (pSessionEnum) pSessionEnum->lpVtbl->Release(pSessionEnum);
    if (pSessionManager) pSessionManager->lpVtbl->Release(pSessionManager);
    if (pDevice) pDevice->lpVtbl->Release(pDevice);
    if (pEnum) pEnum->lpVtbl->Release(pEnum);
    return count;
}

static LONGLONG RectArea(const RECT* rect) {
    LONG width = rect->right - rect->left;
    LONG height = rect->bottom - rect->top;

    if (width <= 0 || height <= 0) {
        return 0;
    }

    return (LONGLONG)width * (LONGLONG)height;
}

static LONGLONG RectIntersectionArea(const RECT* a, const RECT* b) {
    LONG left = a->left > b->left ? a->left : b->left;
    LONG top = a->top > b->top ? a->top : b->top;
    LONG right = a->right < b->right ? a->right : b->right;
    LONG bottom = a->bottom < b->bottom ? a->bottom : b->bottom;

    if (right <= left || bottom <= top) {
        return 0;
    }

    return (LONGLONG)(right - left) * (LONGLONG)(bottom - top);
}

// Prefer DWM's extended frame bounds (accounts for invisible drop-shadow borders);
// fall back to GetWindowRect if DWM query fails.
static int GetVisibleWindowRect(HWND hWnd, RECT* rect) {
    RECT frameRect;
    HRESULT hr = DwmGetWindowAttribute(hWnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frameRect, sizeof(frameRect));
    if (SUCCEEDED(hr) && RectArea(&frameRect) > 0) {
        *rect = frameRect;
        return 1;
    }

    return GetWindowRect(hWnd, rect) != 0;
}

static void MarkMediaWindowMonitors(int mediaOnMonitor[MAX_MONITOR_COUNT], const RECT* windowRect) {
    LONGLONG windowArea = RectArea(windowRect);

    if (windowArea < MIN_MEDIA_WINDOW_AREA) {
        return;
    }

    int marked = 0;
    for (int i = 0; i < g_monitorCount; i++) {
        LONGLONG intersectionArea = RectIntersectionArea(windowRect, &g_monitors[i].rect);
        double overlapRatio = (double)intersectionArea / (double)windowArea;
        if (intersectionArea >= MIN_MEDIA_WINDOW_AREA && overlapRatio >= MIN_MEDIA_WINDOW_OVERLAP_RATIO) {
            mediaOnMonitor[i] = 1;
            marked = 1;
        }
    }

    if (!marked) {
        int monitorIndex = GetMonitorIndexFromRect(*windowRect);
        if (monitorIndex >= 0 && monitorIndex < g_monitorCount) {
            mediaOnMonitor[monitorIndex] = 1;
        }
    }
}

static BOOL CALLBACK EnumMediaWindowCallback(HWND hWnd, LPARAM lParam) {
    MediaEnumContext* ctx = (MediaEnumContext*)lParam;

    if (!IsWindowVisible(hWnd) || IsIconic(hWnd) || IsWindowCloakedCompat(hWnd)) {
        return TRUE;
    }

    LONG_PTR exStyle = GetWindowLongPtr(hWnd, GWL_EXSTYLE);
    if ((exStyle & WS_EX_TOOLWINDOW) != 0) {
        return TRUE;
    }

    RECT rect;
    if (!GetVisibleWindowRect(hWnd, &rect)) {
        return TRUE;
    }

    if (RectArea(&rect) <= 0) {
        return TRUE;
    }

    char processName[MAX_PATH] = {0};
    if (!GetProcessNameFromHwnd(hWnd, processName, sizeof(processName))) {
        return TRUE;
    }

    int isAudioActive = IsAudioActiveProcessName(ctx, processName);

    // A window counts as media if its process is actually emitting audio. We
    // match by exe name (not PID): Chromium audio sessions belong to renderer
    // processes, windows to the main process; both share the exe name. This
    // also handles single-process players (VLC, mpv).
    //
    // Exception: with blockOnMutedMedia, visible browser/media windows with NO
    // audible audio are also collected as muted-media candidates
    // (mutedRects[]) - e.g. YouTube's hover-preview autoplay plays muted video
    // with no audio session.
    int isMutedCandidate = !isAudioActive &&
                           (IsKnownBrowserProcess(processName) || IsKnownMediaProcess(processName)) &&
                           g_app.config.blockOnMutedMedia;

    if (!isAudioActive && !isMutedCandidate) {
        return TRUE;
    }

    char title[512] = {0};
    GetWindowTextA(hWnd, title, sizeof(title));

    // Muted candidates count outright: the visible window IS the media.
    int matched = 1;
    int hintMatched = 1;  // Title-hint result, for the diagnostic log only
    if (isAudioActive) {
        // Multi-process browser audio belongs to renderer PIDs, so it can't be
        // attributed to a window; the hint whitelist also misses sites (e.g.
        // Aniwave), letting the screen saver cover a playing video. Map every
        // visible window of an audio-active process; the hint stays diagnostic.
        hintMatched = IsMediaCandidateWindow(processName, title);
        if (!IsKnownBrowserProcess(processName)) {
            // Non-browser apps keep the old rule: hint (or the global fallback)
            // still required.
            matched = hintMatched;
        }
    }

    // Collect diagnostic info for all browser windows considered (audio-active
    // or muted candidates), logged once in UpdateMediaMonitorStates when the
    // mask changes, so the user can see which windows were examined.
    if (IsKnownBrowserProcess(processName) && title[0] && ctx->browserWindowCount < MAX_BROWSER_WINDOW_INFO) {
        int idx = ctx->browserWindowCount++;
        strncpy(ctx->browserTitles[idx], title, 255);
        ctx->browserTitles[idx][255] = '\0';
        ctx->browserMatched[idx] = hintMatched;
        ctx->browserMapped[idx] = matched;
        ctx->browserRects[idx] = rect;
    }

    if (!matched) {
        return TRUE;
    }

    if (isAudioActive) {
        ctx->audibleMappedWindowCount++;
        MarkMediaWindowMonitors(ctx->mediaOnMonitor, &rect);
    } else if (ctx->mutedRectCount < MAX_BROWSER_WINDOW_INFO) {
        ctx->mutedRects[ctx->mutedRectCount++] = rect;
    }
    return TRUE;
}

// Reset media detection cache. Called after system sleep/wake to force a fresh
// scan, since WASAPI sessions and ES_DISPLAY_REQUIRED state may be stale.
void ResetMediaDetectionCache() {
    g_mediaCacheInvalidated = 1;
}

// --- Per-monitor motion probe ---
//
// When audible audio can't be attributed to one window (multiple visible
// windows of the same audio-active process), the window mapping can't tell a
// paused tab on the OLED from a playing one. Sampling the monitor's content
// does: a monitor counts as media only while its content is moving.
// MOTION_GRACE_MS keeps the flag while content briefly holds still (quiet
// scenes, letterboxing).
//
// While the black screen saver window is up (or fading) sampling is frozen:
// the previous sample stays the last pre-saver desktop frame, so the first
// comparison after dismissal sees the real current desktop - a paused video
// then yields no motion (fast sleep), a playing one yields motion immediately
// (no dim/undim race). The stale grace must also not apply while the black
// window is up, or it would deactivate the screen saver it just covered.
#define MOTION_REGION_COUNT 4
#define MOTION_REGION_SIZE 32
#define MOTION_GRACE_MS 15000
#define MOTION_PIXELS (MOTION_REGION_SIZE * MOTION_REGION_SIZE)

static BYTE g_motionPrev[MAX_MONITOR_COUNT][MOTION_REGION_COUNT][MOTION_PIXELS * 4];
static DWORD g_motionLastTick[MAX_MONITOR_COUNT];

// Sample 4 inset 32x32 blocks of the monitor; returns 1 when the content
// changed since the previous sample (moving content). The comparison is
// tolerant (>=8/255 on >=3% of pixels) so cursor blinks, clock ticks and DWM
// noise don't count, while real video changes massively. captureFails counts
// regions whose capture failed. Call once per media scan.
static int UpdateMonitorMotion(int monitorIndex, int* captureFails) {
    RECT mr = g_monitors[monitorIndex].rect;
    int w = mr.right - mr.left;
    int h = mr.bottom - mr.top;
    if (w < MOTION_REGION_SIZE * 2 || h < MOTION_REGION_SIZE * 2) return 0;

    HDC hdcScreen = GetDC(NULL);
    if (!hdcScreen) return 0;

    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    HBITMAP hbm = CreateCompatibleBitmap(hdcScreen, MOTION_REGION_SIZE, MOTION_REGION_SIZE);
    int diff = 0;
    if (hdcMem && hbm) {
        HGDIOBJ hOld = SelectObject(hdcMem, hbm);
        // Quarter-point anchors, inset from the edges (taskbar/clock strips).
        int ax[4] = { w / 4, 3 * w / 4, w / 4, 3 * w / 4 };
        int ay[4] = { h / 4, h / 4, 3 * h / 4, 3 * h / 4 };
        for (int r = 0; r < MOTION_REGION_COUNT; r++) {
            int x = mr.left + ax[r] - MOTION_REGION_SIZE / 2;
            int y = mr.top + ay[r] - MOTION_REGION_SIZE / 2;
            if (!BitBlt(hdcMem, 0, 0, MOTION_REGION_SIZE, MOTION_REGION_SIZE,
                        hdcScreen, x, y, SRCCOPY)) {
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
            if (GetDIBits(hdcMem, hbm, 0, MOTION_REGION_SIZE, cur, &bmi, DIB_RGB_COLORS) == MOTION_REGION_SIZE) {
                const BYTE* a = cur;
                const BYTE* b = g_motionPrev[monitorIndex][r];
                int changedPixels = 0;
                for (int p = 0; p < MOTION_PIXELS; p++) {
                    int d0 = (int)a[0] - (int)b[0]; if (d0 < 0) d0 = -d0;
                    int d1 = (int)a[1] - (int)b[1]; if (d1 < 0) d1 = -d1;
                    int d2 = (int)a[2] - (int)b[2]; if (d2 < 0) d2 = -d2;
                    if (d0 >= 8 || d1 >= 8 || d2 >= 8) changedPixels++;
                    a += 4;
                    b += 4;
                }
                if (changedPixels >= MOTION_PIXELS / 32) {
                    diff = 1;
                }
                memcpy(g_motionPrev[monitorIndex][r], cur, sizeof(cur));
            } else if (captureFails) {
                (*captureFails)++;
            }
        }
        SelectObject(hdcMem, hOld);
    }
    if (hbm) DeleteObject(hbm);
    if (hdcMem) DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);

    if (diff) {
        g_motionLastTick[monitorIndex] = GetTickCount();
        return 1;
    }
    return 0;
}

// Returns 1 when the foreground window covers >=95% of the monitor (fullscreen
// game/video). Screen-DC capture during fullscreen-optimized rendering can
// cause a brief hitch, so the motion probe is skipped then - the same guard
// the watchdog's pixel probe uses.
static int IsForegroundFullscreenOnMonitor(int monitorIndex) {
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

// Returns 1 when the foreground window belongs to a known browser or media
// player. The motion probe's fullscreen skip guards screen-DC capture against
// hitches from fullscreen-optimized rendering (games/unknown apps); browsers
// and media players cover the monitor with plain windows, so they can (and
// must) be sampled - otherwise a video muted from the start, or in a quiet
// scene longer than the audio grace, would lose its mapping once the grace
// expires and the screen saver would cover the playing video.
static int IsForegroundMediaProcess(void) {
    HWND hFg = GetForegroundWindow();
    if (!hFg) return 0;

    char processName[MAX_PATH] = {0};
    if (!GetProcessNameFromHwnd(hFg, processName, sizeof(processName))) return 0;

    return IsKnownBrowserProcess(processName) || IsKnownMediaProcess(processName);
}

// Fills mediaOnMonitor[] with 1 for each monitor hosting a visible media window.
// Uses the cheap ES_DISPLAY_REQUIRED gate to skip enumeration when nothing is
// playing, and caches the scan for MEDIA_DETECTION_CACHE_MS to keep the timer
// light. Returns 1 if any monitor has media. If media is playing globally but
// no candidate window maps to a monitor, falls back to blocking all enabled
// monitors (safe default so unknown apps are never covered).
int UpdateMediaMonitorStates(int mediaOnMonitor[MAX_MONITOR_COUNT]) {
    static DWORD lastLoggedMask = (DWORD)-1;
    static DWORD lastScanTick = 0;
    static int hasCachedState = 0;
    static int cachedAnyMedia = 0;
    static int cachedMediaOnMonitor[MAX_MONITOR_COUNT] = {0};
    static DWORD lastAudioDetectedTick = 0;

    for (int i = 0; i < MAX_MONITOR_COUNT; i++) {
        mediaOnMonitor[i] = 0;
    }

    if (g_mediaCacheInvalidated) {
        hasCachedState = 0;
        lastLoggedMask = (DWORD)-1;
        g_mediaCacheInvalidated = 0;
        LogMessage("Media detection cache invalidated (sleep/wake)");
    }

    if (!g_app.config.mediaDetectionEnabled) {
        hasCachedState = 0;
        if (lastLoggedMask != 0) {
            LogMessage("Media monitor detection: disabled");
            lastLoggedMask = 0;
        }
        return 0;
    }

    DWORD nowTick = GetTickCount();
    if (hasCachedState && (DWORD)(nowTick - lastScanTick) < MEDIA_DETECTION_CACHE_MS) {
        for (int i = 0; i < MAX_MONITOR_COUNT; i++) {
            mediaOnMonitor[i] = cachedMediaOnMonitor[i];
        }
        return cachedAnyMedia;
    }

    lastScanTick = nowTick;

    int globalMediaPlaying = IsMediaPlaying();

    if (!globalMediaPlaying) {
        for (int i = 0; i < MAX_MONITOR_COUNT; i++) {
            cachedMediaOnMonitor[i] = 0;
        }
        hasCachedState = 1;
        cachedAnyMedia = 0;

        if (lastLoggedMask != 0) {
            LogMessage("Media monitor detection: no active media monitors");
            lastLoggedMask = 0;
        }
        return 0;
    }

    int localMediaOnMonitor[MAX_MONITOR_COUNT] = {0};
    MediaEnumContext ctx = {0};
    int inAudioGrace = 0;  // No audio this scan, but inside the post-audio grace window
    ctx.audioActiveProcessNameCount = CollectActiveAudioProcessNames(
        ctx.audioActiveProcessNames, MAX_ACTIVE_AUDIO_PIDS);

    if (ctx.audioActiveProcessNameCount > 0) {
        lastAudioDetectedTick = nowTick;
    } else if (hasCachedState && cachedAnyMedia &&
               (DWORD)(nowTick - lastAudioDetectedTick) < AUDIO_GRACE_PERIOD_MS) {
        // Quiet passage (peak meter dipped). Still run the window scan below:
        // the old early return here kept the mapping stale for up to 30s after
        // the video moved monitors. The cached mask is only kept when the fresh
        // scan maps nothing (video minimized) - the anti-flicker case this
        // grace exists for.
        inAudioGrace = 1;
        lastScanTick = nowTick;
    }

    EnumWindows(EnumMediaWindowCallback, (LPARAM)&ctx);

    int mappedMonitorCount = 0;
    for (int i = 0; i < g_monitorCount; i++) {
        if (ctx.mediaOnMonitor[i]) {
            localMediaOnMonitor[i] = 1;
            mediaOnMonitor[i] = 1;
            mappedMonitorCount++;
        }
    }

    // Exactly one visible browser window while audio is AUDIBLE: it must be
    // the source (an audio session can't belong to a window that doesn't
    // exist), so trust its rect instead of the block-all fallback (fixes
    // hint-less autoplay sites like Reddit). Gated on audio: with no audio the
    // window is just a paused/muted candidate, and mapping it here would keep
    // the screen saver off forever (site wake locks keep ES_DISPLAY_REQUIRED
    // set while paused).
    if (mappedMonitorCount == 0 && ctx.browserWindowCount == 1 &&
        ctx.audioActiveProcessNameCount > 0) {
        MarkMediaWindowMonitors(ctx.mediaOnMonitor, &ctx.browserRects[0]);
        for (int i = 0; i < g_monitorCount; i++) {
            if (ctx.mediaOnMonitor[i]) {
                localMediaOnMonitor[i] = 1;
                mediaOnMonitor[i] = 1;
                mappedMonitorCount++;
            }
        }
    }

    // Ambiguity: audible audio with more than one visible window of the
    // audio-active process(es). Multi-process browsers can't attribute audio
    // to a window (renderer PID != window PID), so a window sitting on a
    // monitor doesn't mean that monitor is playing - a paused tab on the OLED
    // would keep the screen saver off while the actual playback is elsewhere.
    // Drop the window mapping; the per-monitor motion probe below decides.
    // Monitors fully covered by the foreground window keep their mapping: a
    // fullscreen video/game there is unambiguous watching, and the probe is
    // skipped during fullscreen anyway.
    int clearFired = 0;
    if (ctx.audioActiveProcessNameCount > 0 && ctx.audibleMappedWindowCount > 1) {
        clearFired = 1;
        for (int i = 0; i < g_monitorCount; i++) {
            if (IsForegroundFullscreenOnMonitor(i)) continue;
            mediaOnMonitor[i] = 0;
        }
    }

    // Motion supplement: a monitor whose content is moving counts as media
    // even without a window mapping (muted playback, or ambiguous multi-window
    // audio - see the ambiguity clear above). While the black screen saver
    // window is up (or fading) the probe is frozen and the grace does not
    // apply: the stale grace must not deactivate the screen saver it just
    // covered (that was the dim/undim cycle). Sampling is also skipped on
    // monitors a fullscreen window of a non-media process covers (screen
    // capture can hitch fullscreen-optimized game rendering) - per monitor,
    // so a maximized window on one monitor can't disable detection on the
    // others. A fullscreen browser/media player is sampled: it is a plain
    // window, and without the probe a video muted from the start (or in a
    // quiet scene longer than the audio grace) would get covered.
    int motionDiff[MAX_MONITOR_COUNT] = {0};
    int motionFails[MAX_MONITOR_COUNT] = {0};
    DWORD motionAge[MAX_MONITOR_COUNT];
    int fgMediaProcess = IsForegroundMediaProcess();
    for (int i = 0; i < g_monitorCount; i++) {
        motionAge[i] = 0xFFFFFFFFu;
        if (IsForegroundFullscreenOnMonitor(i) && !fgMediaProcess) {
            continue;  // fullscreen game/unknown app: probe skipped (capture hitch)
        }
        HWND hSaver = g_monitorStates[i].hScreenSaverWnd;
        if (hSaver && IsWindowVisible(hSaver)) {
            continue;  // black window up/fading: probe frozen, no grace
        }
        motionDiff[i] = UpdateMonitorMotion(i, &motionFails[i]);
        // Fresh GetTickCount(): lastTick was just set mid-scan when a diff was
        // found, so the scan-start nowTick minus it would wrap and the fresh
        // motion would fail the grace check below.
        motionAge[i] = GetTickCount() - g_motionLastTick[i];
        if (motionAge[i] < MOTION_GRACE_MS) {
            mediaOnMonitor[i] = 1;
        }
    }

    int usedGlobalFallback = 0;
    int usedMutedMapping = 0;
    int skippedFallbackForBrowser = 0;
    int skippedFallbackForAudioOnly = 0;
    int skippedFallbackForNoAudio = 0;
    int usedCachedMask = 0;
    int canUseMutedMapping = 0;
    if (mappedMonitorCount == 0) {
        // Nothing mapped from audible audio. If ES_DISPLAY_REQUIRED is set the
        // media may be MUTED (e.g. YouTube hover-preview autoplay has no audio
        // session): visible browser/media windows are candidates. They are not
        // mapped here - the gated mapping below runs after the motion probe
        // and only counts a monitor while its content moves, so a paused
        // window can't block the screen saver (the paused video's site wake
        // lock keeps ES_DISPLAY_REQUIRED set, but static content here means
        // nothing is playing here).
        if (ctx.audioActiveProcessNameCount == 0) {
            // Candidates only count inside the grace window; otherwise a
            // paused video blocks the screen saver forever.
            canUseMutedMapping = inAudioGrace && g_app.config.blockOnMutedMedia;
        } else if (!AllAudioActiveAreBrowsers(&ctx)) {
            // Non-browser audio (e.g. Discord voice chat) doesn't explain
            // ES_DISPLAY_REQUIRED; the media is likely a muted preview in a
            // visible browser. Mapping beats the blanket block-all.
            canUseMutedMapping = 1;
        }
    }

    // Muted-media mapping, gated on the motion probe just run. A visible
    // browser/media window with no audible audio is only evidence of playback
    // while its monitor's content moves (muted video, hover previews); a
    // paused window is static, so it must not wake the screen saver - pausing
    // a video elsewhere leaves its site wake lock set, and the old rect-only
    // mapping then deactivated the OLED that a paused browser window sat on.
    // Monitors fully covered by the foreground window are probe-skipped, so
    // their windows map outright: a fullscreen muted video is unambiguous.
    if (mappedMonitorCount == 0 && canUseMutedMapping) {
        int mutedOnMonitor[MAX_MONITOR_COUNT] = {0};
        for (int w = 0; w < ctx.mutedRectCount; w++) {
            MarkMediaWindowMonitors(mutedOnMonitor, &ctx.mutedRects[w]);
        }
        for (int i = 0; i < g_monitorCount; i++) {
            if (!mutedOnMonitor[i] || mediaOnMonitor[i]) {
                continue;
            }
            if (IsForegroundFullscreenOnMonitor(i) || motionAge[i] < MOTION_GRACE_MS) {
                mediaOnMonitor[i] = 1;
                mappedMonitorCount++;
            }
        }
        if (mappedMonitorCount > 0) {
            usedMutedMapping = 1;
        }
    }

    if (mappedMonitorCount == 0) {
        if (inAudioGrace) {
            // Fresh scan mapped nothing inside the grace window (video
            // minimized/closed during a quiet passage): keep the cached
            // mapping so the screen saver doesn't flicker. Moves are not
            // this case - a moved window maps in the fresh scan above. Union
            // (not overwrite): the motion probe and muted mapping above may
            // have already flagged monitors with fresh evidence.
            for (int i = 0; i < MAX_MONITOR_COUNT; i++) {
                if (!mediaOnMonitor[i]) {
                    mediaOnMonitor[i] = cachedMediaOnMonitor[i];
                }
            }
            usedCachedMask = 1;
        } else if (ctx.audioActiveProcessNameCount == 0) {
            // ES_DISPLAY_REQUIRED set with no audible audio and nothing
            // mapped (no window, or grace expired). The flag is global -
            // any process can set it (site wake locks, OBS replay buffer,
            // stale flags) - so without recent audio there's no evidence
            // of playback: let the screen saver activate.
            skippedFallbackForNoAudio = 1;
            LogMessage("Media detection: no audible audio recently and nothing mapped: skipping fallback");
        } else if (AllAudioActiveAreBrowsers(&ctx)) {
            // All audio-active processes are known browsers but nothing
            // mapped: with the per-window mapping in EnumMediaWindowCallback,
            // this can only happen when no visible (unminimized, uncloaked)
            // browser window exists - e.g. the playing tab's window is
            // minimized. Nothing visible to protect, so skip the fallback
            // and let the screen saver activate.
            skippedFallbackForBrowser = 1;
            LogMessage("Media detection: no visible browser window, all audio-active processes are browsers: skipping fallback");
        } else if (AllAudioActiveAreAudioOnly(&ctx)) {
            // Every audio-active process is a known audio-only app (music/
            // podcast). Audio never needs the display on and audio-only
            // windows are deliberately not mapped as media, so audible music
            // must not block the screen saver - including the block-all
            // fallback that would otherwise wake an idle OLED on another
            // monitor when music is paused or unpaused there.
            skippedFallbackForAudioOnly = 1;
            LogMessage("Media detection: all audio-active processes are known audio-only apps: skipping fallback");
        } else {
            // Non-browser audio (unknown app, media player with minimized
            // window, audio on non-default device). Conservatively block
            // all enabled monitors to avoid covering playback.
            for (int i = 0; i < g_monitorCount; i++) {
                if (g_monitorStates[i].enabled) {
                    mediaOnMonitor[i] = 1;
                }
            }
            usedGlobalFallback = 1;
        }
    }

    DWORD mask = 0;
    for (int i = 0; i < g_monitorCount && i < 32; i++) {
        if (mediaOnMonitor[i]) {
            mask |= (1u << i);
        }
    }

    hasCachedState = 1;
    cachedAnyMedia = mask != 0;
    for (int i = 0; i < MAX_MONITOR_COUNT; i++) {
        cachedMediaOnMonitor[i] = mediaOnMonitor[i];
    }

    if (mask != lastLoggedMask) {
        for (int i = 0; i < ctx.browserWindowCount; i++) {
            LogMessage("Media detection: browser window %s (hint: %s): '%.120s'",
                       ctx.browserMapped[i] ? "MAPPED" : "SKIPPED",
                       ctx.browserMatched[i] ? "MATCHED" : "no hint",
                       ctx.browserTitles[i]);
        }
        for (int i = 0; i < ctx.audioActiveProcessNameCount; i++) {
            LogMessage("Media detection: active audio process: %s", ctx.audioActiveProcessNames[i]);
        }
        LogMessage("Media monitor detection: mask=0x%08X (activeAudioNames=%d, fallback=%d, mutedMap=%d, "
                   "browserSkip=%d, audioOnlySkip=%d, noAudioSkip=%d, browserWindows=%d, graceKeep=%d, "
                   "clear=%d, motion=[%d %d %d] fails=[%d %d %d] age=[%u %u %u])",
                   mask, ctx.audioActiveProcessNameCount, usedGlobalFallback, usedMutedMapping,
                   skippedFallbackForBrowser, skippedFallbackForAudioOnly, skippedFallbackForNoAudio,
                   ctx.browserWindowCount, usedCachedMask, clearFired,
                   motionDiff[0], motionDiff[1], motionDiff[2],
                   motionFails[0], motionFails[1], motionFails[2],
                   motionAge[0], motionAge[1], motionAge[2]);
        lastLoggedMask = mask;
    }

    return cachedAnyMedia;
}

// Check if a Windows shell overlay window (Start Menu, Task View, Action Center) is open
// Returns the number of shell windows detected (0, 1, or 2 if both Start Menu and Action Center)
