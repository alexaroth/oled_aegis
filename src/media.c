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
    // Diagnostic info: all browser windows with active audio, collected during
    // enumeration and logged once in UpdateMediaMonitorStates when the mask changes.
    // This avoids per-tick log spam and shows both matching and non-matching windows.
    char browserTitles[MAX_BROWSER_WINDOW_INFO][256];
    int browserMatched[MAX_BROWSER_WINDOW_INFO];  // 1 = matched a hint, 0 = no hint
    RECT browserRects[MAX_BROWSER_WINDOW_INFO];   // window rects, used to map media to monitors without title hints
    int browserWindowCount;
    int mutedMediaOnMonitor[MAX_MONITOR_COUNT];    // Monitors hosting visible browser/media-player windows with no audible audio (muted-media candidates, e.g. YouTube hover previews)
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
                if (SUCCEEDED(pMeter->lpVtbl->GetPeakValue(pMeter, &peak)) && peak > AUDIO_ACTIVE_PEAK_THRESHOLD) {
                    IAudioSessionControl2* pControl2 = NULL;
                    if (SUCCEEDED(pControl->lpVtbl->QueryInterface(pControl, &IID_IAudioSessionControl2, (void**)&pControl2)) && pControl2) {
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

    // A window only counts as media if its process is actually emitting audio.
    // We match by exe name (not PID) because Chromium browsers run multi-process:
    // the audio session belongs to the renderer process, while the window belongs
    // to the main process both share the same exe name. This also handles
    // single-process players (VLC, mpv) where name match == PID match.
    //
    // Exception: when the user opted in to blocking muted media, visible
    // browser/media-player windows with NO audible audio are still collected as
    // muted-media candidates (mutedMediaOnMonitor[]) - e.g. YouTube's
    // hover-preview autoplay plays muted video with no audio session. The caller
    // maps muted media to those monitors instead of blocking every enabled
    // monitor.
    int isMutedCandidate = !isAudioActive &&
                           (IsKnownBrowserProcess(processName) || IsKnownMediaProcess(processName)) &&
                           g_app.config.blockOnMutedMedia;

    if (!isAudioActive && !isMutedCandidate) {
        return TRUE;
    }

    char title[512] = {0};
    GetWindowTextA(hWnd, title, sizeof(title));

    int matched;
    if (isAudioActive) {
        matched = IsMediaCandidateWindow(processName, title);
    } else {
        // Muted-media candidate: treat the visible browser/media-player window
        // itself as the media location.
        matched = 1;
    }

    // Collect diagnostic info for all browser windows considered (audio-active
    // or muted candidates), logged once in UpdateMediaMonitorStates when the
    // mask changes, so the user can see which windows were examined.
    if (IsKnownBrowserProcess(processName) && title[0] && ctx->browserWindowCount < MAX_BROWSER_WINDOW_INFO) {
        int idx = ctx->browserWindowCount++;
        strncpy(ctx->browserTitles[idx], title, 255);
        ctx->browserTitles[idx][255] = '\0';
        ctx->browserMatched[idx] = matched;
        ctx->browserRects[idx] = rect;
    }

    if (!matched) {
        return TRUE;
    }

    if (isAudioActive) {
        MarkMediaWindowMonitors(ctx->mediaOnMonitor, &rect);
    } else {
        MarkMediaWindowMonitors(ctx->mutedMediaOnMonitor, &rect);
    }
    return TRUE;
}

// Reset media detection cache. Called after system sleep/wake to force a fresh
// scan, since WASAPI sessions and ES_DISPLAY_REQUIRED state may be stale.
void ResetMediaDetectionCache() {
    g_mediaCacheInvalidated = 1;
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
    ctx.audioActiveProcessNameCount = CollectActiveAudioProcessNames(
        ctx.audioActiveProcessNames, MAX_ACTIVE_AUDIO_PIDS);

    if (ctx.audioActiveProcessNameCount > 0) {
        lastAudioDetectedTick = nowTick;
    } else if (hasCachedState && cachedAnyMedia &&
               (DWORD)(nowTick - lastAudioDetectedTick) < AUDIO_GRACE_PERIOD_MS) {
        // Audio was detected recently but this scan found no audible audio.
        // This happens during quiet passages in video audio where the peak
        // meter momentarily drops below threshold. Keep the previous media
        // state to avoid flickering the screen saver on and off.
        for (int i = 0; i < MAX_MONITOR_COUNT; i++) {
            mediaOnMonitor[i] = cachedMediaOnMonitor[i];
        }
        hasCachedState = 1;
        cachedAnyMedia = 1;
        lastScanTick = nowTick;

        DWORD mask = 0;
        for (int i = 0; i < g_monitorCount && i < 32; i++) {
            if (mediaOnMonitor[i]) {
                mask |= (1u << i);
            }
        }
        if (mask != lastLoggedMask) {
            LogMessage("Media monitor detection: mask=0x%08X (grace period, %lums since last audio)",
                       mask, (unsigned long)(nowTick - lastAudioDetectedTick));
            lastLoggedMask = mask;
        }
        return 1;
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

    // No window title hinted at video, but exactly one visible browser window
    // has active audio. That window must be where the audio is coming from (a
    // browser's audio session can't belong to a window that doesn't exist), so
    // trust its position instead of falling back to blocking all enabled
    // monitors. This fixes autoplaying videos on sites with no video hint in
    // the tab title (e.g. Reddit), where the old code deactivated the screen
    // saver on every enabled monitor even though the video was elsewhere.
    if (mappedMonitorCount == 0 && ctx.browserWindowCount == 1) {
        MarkMediaWindowMonitors(ctx.mediaOnMonitor, &ctx.browserRects[0]);
        for (int i = 0; i < g_monitorCount; i++) {
            if (ctx.mediaOnMonitor[i]) {
                localMediaOnMonitor[i] = 1;
                mediaOnMonitor[i] = 1;
                mappedMonitorCount++;
            }
        }
    }

    int usedGlobalFallback = 0;
    int usedMutedMapping = 0;
    int skippedFallbackForBrowser = 0;
    int skippedFallbackForNoAudio = 0;
    if (mappedMonitorCount == 0) {
        // No media window was mapped from audible audio. If ES_DISPLAY_REQUIRED
        // is set, the media may be MUTED - e.g. YouTube's hover-preview autoplay
        // plays muted video with no audio session. Visible browser/media-player
        // windows are the best evidence of where the media is, so map to their
        // monitors instead of blocking every enabled monitor (which would
        // deactivate the screen saver on unused monitors).
        int canUseMutedMapping = 0;
        if (ctx.audioActiveProcessNameCount == 0) {
            canUseMutedMapping = g_app.config.blockOnMutedMedia;
        } else if (!AllAudioActiveAreBrowsers(&ctx)) {
            // Non-browser audio (e.g. Discord voice chat) doesn't explain
            // ES_DISPLAY_REQUIRED; the media is likely a muted preview in a
            // visible browser. Mapping beats the blanket block-all.
            canUseMutedMapping = 1;
        }

        if (canUseMutedMapping) {
            for (int i = 0; i < g_monitorCount; i++) {
                if (ctx.mutedMediaOnMonitor[i]) {
                    mediaOnMonitor[i] = 1;
                    mappedMonitorCount++;
                }
            }
            if (mappedMonitorCount > 0) {
                usedMutedMapping = 1;
            }
        }

        if (mappedMonitorCount == 0) {
            if (ctx.audioActiveProcessNameCount == 0) {
            // ES_DISPLAY_REQUIRED is set but no audible audio is detected on the
            // default render endpoint. This happens with muted video, OBS replay
            // buffer, or other apps that call SetThreadExecutionState without
            // producing audio.
            if (g_app.config.blockOnMutedMedia) {
                // User opted in to blocking on muted/silent media. Conservatively
                // block all enabled monitors.
                for (int i = 0; i < g_monitorCount; i++) {
                    if (g_monitorStates[i].enabled) {
                        mediaOnMonitor[i] = 1;
                    }
                }
                usedGlobalFallback = 1;
            } else {
                // No audible media is playing, let the screen saver activate.
                skippedFallbackForNoAudio = 1;
                LogMessage("Media detection: ES_DISPLAY_REQUIRED set but no audible audio detected: skipping fallback");
            }
        } else if (AllAudioActiveAreBrowsers(&ctx)) {
            // All audio-active processes are known browsers, but no window title
            // matched a video hint. This typically means video is playing in a
            // background tab - the window title shows the active tab, not the
            // playing one. We can't determine which monitor is playing, so
            // rather than blocking all monitors (which would defeat per-monitor
            // detection), we skip the fallback and let the screen saver activate.
            // The user can always move the mouse to dismiss it if needed.
            skippedFallbackForBrowser = 1;
            LogMessage("Media detection: no title hint match, all audio-active processes are browsers: skipping fallback");
        } else {
            // Non-browser audio (unknown app, media player with minimized window,
            // audio on non-default device). Conservatively block all enabled
            // monitors to avoid covering playback.
            for (int i = 0; i < g_monitorCount; i++) {
                if (g_monitorStates[i].enabled) {
                    mediaOnMonitor[i] = 1;
                }
            }
            usedGlobalFallback = 1;
            }
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
            LogMessage("Media detection: browser window %s: '%.120s'",
                       ctx.browserMatched[i] ? "MATCHED  " : "no hint  ",
                       ctx.browserTitles[i]);
        }
        for (int i = 0; i < ctx.audioActiveProcessNameCount; i++) {
            LogMessage("Media detection: active audio process: %s", ctx.audioActiveProcessNames[i]);
        }
        LogMessage("Media monitor detection: mask=0x%08X (activeAudioNames=%d, fallback=%d, mutedMap=%d, browserSkip=%d, noAudioSkip=%d, browserWindows=%d)",
                   mask, ctx.audioActiveProcessNameCount, usedGlobalFallback, usedMutedMapping, skippedFallbackForBrowser, skippedFallbackForNoAudio, ctx.browserWindowCount);
        lastLoggedMask = mask;
    }

    return cachedAnyMedia;
}

// Check if a Windows shell overlay window (Start Menu, Task View, Action Center) is open
// Returns the number of shell windows detected (0, 1, or 2 if both Start Menu and Action Center)
