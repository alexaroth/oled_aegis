# AGENTS.md — OLED Aegis

Project instructions for AI agents and developers working on this repository. User has 2 OLEDs and an IPS. One oled is primary monitor, second monitor is the IPS, and the third monitor is the secondary OLED. The user wants to prevent burn-in on the third monitor, the OLED.

## What this is

A Windows tray application that prevents OLED burn-in by blackening **individual
monitors** after an idle timeout (a per-monitor "screen saver"), instead of all
monitors at once. It also detects video/media playback and keeps the display
awake (or the screen saver off) while media plays — per monitor when enabled.

Pure Win32 **C** (MSVC), no C++, no framework, no third-party runtime. Windows
10/11 only.

## Build & run

- **`oled_aegis.sln`** (Visual Studio 2022, x64, Debug/Release) — open and
  press F5. The project mirrors the command-line build exactly (`INITGUID`
  define, same libs, `.rc` compiled, output to `build\oled_aegis.exe`, PDB
  produced in both configs).
- **`build.bat`** — compile + link everything. Requires a Developer Command
  Prompt (vcvars). Output: `build/oled_aegis.exe`.
- **`build.ps1 [release|debug]`** — locates VS itself, caches the MSVC env in
  `build/vc_env.txt` (delete that file if VS paths change). Also usable from
  WSL via `build.sh`.
- **`/D "INITGUID"` is mandatory.** The MMDevice/audio-session GUIDs are
  `DEFINE_GUID`'d in `src/media.c` only; without INITGUID they don't emit
  definitions and the link fails. Don't move those `DEFINE_GUID` lines to a
  header (would duplicate definitions per TU).
- GitHub Actions builds via `build.bat` on push to `main` touching `src/**`.
- The exe is a tray app: to rebuild while it's running, kill it first
  (`taskkill /IM oled_aegis.exe /F`) or the linker fails with `LNK1104`.

## Source layout

All modules share `src/oled_aegis.h` (constants, typedefs, `extern` globals,
cross-module prototypes).

| File | Responsibility |
|---|---|
| `src/oled_aegis.c` | `WinMain`, hidden main window (`OLEDAegisWindow`), `HandleCreation` (init), `HandleTimeout` (the idle-check timer state machine), tray icon, `WndProc` |
| `src/screensaver.c` | The black windows (`OLEDAegisScreen`, one per monitor): show/hide, fade-to-black transition, hide/minimize vetoes, topmost reassertion, watchdog, shell-window (Start Menu etc.) closing |
| `src/media.c` | Media detection: `ES_DISPLAY_REQUIRED` gate, WASAPI audio-session scan, window→monitor mapping. Owns the `DEFINE_GUID` blocks |
| `src/monitors.c` | Monitor enumeration (GDI + DisplayConfig), owns `g_monitors`/`g_monitorStates`, lookup helpers |
| `src/settings.c` | Settings dialog (hand-built controls, DPI-scaled), `ApplySettings` |
| `src/config.c` | `oled_aegis.ini` load/save, clamping, startup registry entry |
| `src/util.c` | Path resolution, cursor visibility helpers |
| `src/logging.c` | Debug log file with 1 MB rotation |

## Architecture notes

- **Single instance** via named mutex `OLEDAegis_SingleInstance` (held for app
  lifetime).
- **No message pump complexity**: one hidden window, one `WM_TIMER`
  (`TIMER_IDLE_CHECK`, interval = `checkInterval`, 250–10000 ms). All idle
  logic lives in `HandleTimeout` (`src/oled_aegis.c`).
- **Idle detection**: global mode uses `GetLastInputInfo`; per-monitor mode
  tracks cursor position + foreground-window location per monitor.
- **Media detection pipeline** (`src/media.c`, called from `HandleTimeout`):
  1. Cheap gate: `CallNtPowerInformation(SystemExecutionState)` checks
     `ES_DISPLAY_REQUIRED`; skip everything if unset.
  2. WASAPI: enumerate default render endpoint sessions, keep processes whose
     session is `AudioSessionStateActive` **and** peak meter `> 0.0001f`
     (filters paused/silent sessions).
  3. `EnumWindows`: visible, non-cloaked, non-tool windows of audio-active
     processes map their rect → monitors by overlap (title hints are
     diagnostic only — the whitelist misses sites like Aniwave). Muted
     candidates (visible browser/media windows with no audio) map only while
     inside the 30 s audio grace window **and** their monitor's content is
     moving (or a fullscreen foreground window covers it — the probe is
     skipped there); a paused window is static, so it never wakes the screen
     saver. Results cached 2 s.
  4. Ambiguity: audible audio with >1 visible window of the audio-active
     process can't be attributed per window (renderer PID ≠ window PID), so
     the window mapping is dropped and a per-monitor motion probe decides:
     a monitor counts as media only while its content moves (4×32×32 inset
     blocks, 15 s grace, tolerant compare; probe frozen while the screen saver
     covers the monitor, so the stale grace can't deactivate it; captures
     skipped per-monitor under fullscreen windows of non-media processes — a
     fullscreen browser/media player is sampled, covering videos muted from
     the start and quiet scenes longer than the audio grace).
     This keeps a paused tab on the OLED from blocking the screen saver while
     playback is elsewhere, and keeps visibly-playing muted video from being
     covered.
  - Browsers are matched **by exe name, not PID** (Chromium audio sessions
    belong to renderer processes; windows to the main process).
  - Fallbacks: single browser window with audible audio → trust its rect;
    unknown non-browser audio → block all enabled monitors (conservative).
    No audible audio recently + nothing mapped → never block (ES_DISPLAY_REQUIRED
    is a global flag any process can set — wake locks, OBS, stale flags).
- **Modes** (all in `HandleTimeout`):
  - Global input + global media: all-on/all-off.
  - Global input + per-monitor media: per-monitor idle "pause" offsets
    (`mediaPauseOffsetMs` in `MonitorState`, global `g_mediaPauseOffsetMs`),
    cleared on real input.
  - Per-monitor input: independent per-monitor timers.
- **Manual activation cooldown**: `SendInput` Escape presses (closing Start
  Menu etc. before showing the screen saver) register as user input, so
  activation sets a 2500 ms cooldown (`MANUAL_ACTIVATION_COOLDOWN_MS`) to
  avoid instantly deactivating.
- **Robustness layer** (`src/screensaver.c`): `WM_WINDOWPOSCHANGING`/
  `WM_STYLECHANGING` vetoes on the black windows, 5 s topmost reassert,
  watchdog that restores hidden/minimized/moved/cloaked windows, and a
  throttled pixel probe that verifies the screen is actually black.
- **Fade-to-black transition** (`fadeDurationMs`, 0 = instant): when enabled,
  black windows are layered (`WS_EX_LAYERED`) and their whole-window alpha is
  animated by `UpdateFades` on a dedicated `TIMER_FADE` (~15 ms) via
  `SetLayeredWindowAttributes` — activation fades 0→255 (desktop darkens to
  black), deactivation fades 255→0 before hiding. Fades can be reversed
  mid-flight (e.g. re-show during fade-out) from the current alpha, and the
  watchdog's pixel probe is skipped while a fade is in progress since the
  screen is legitimately not black yet.
- **Pixel shift compensation**: black windows are expanded beyond monitor
  bounds by `pixelShiftCompensation` px (4–8 for QD-OLED panels).
- **Persistence**: config in `%APPDATA%\OLED_Aegis\oled_aegis.ini`; monitors
  keyed by persistent `monitorDevicePath` (legacy `monitor0=` and
  `monitorEnabled_<GDI name>` keys still read for migration). `LoadConfig`
  resets enablement first, so monitors without an entry start disabled.
  Entries seen in the file are remembered and re-emitted by `SaveConfig` even
  when the monitor is currently disconnected, so a temporary disconnect (or an
  Apply while unplugged) can't erase the saved selection. Fallback: only a
  config with **no** monitor entries at all enables the primary monitor; if
  entries exist but none match, nothing is enabled (configured monitors are
  just disconnected) until the saved monitor returns.

## Conventions

- Plain C99-style C (MSVC). No C++, no exceptions, no STL.
- Shared state is global by design: define the variable in its owner module,
  declare `extern` in `oled_aegis.h`. Module-internal helpers are `static`.
  Owner modules: `g_app`, `g_blackBrush`, `g_hIconActive/Inactive` →
  `oled_aegis.c`; `g_hSettingsDialog` → `settings.c`; `g_monitorCount`,
  `g_monitors`, `g_monitorStates` → `monitors.c`; `g_logFile` → `logging.c`.
- **Max line length: 170 chars** (code and comments). Wrap long lines.
- Comments should be **concise** (typically a few lines, never paragraphs) but
  must still explain *why* — especially hard-won bug history. If a comment
  needs more than ~6 lines, say it in fewer.
- `LogMessage(...)` is the debug facility (gated by `debugMode`); keep
  existing log lines' wording/format stable — the user greps them for
  diagnostics. Log file: `%APPDATA%\OLED_Aegis\oled_aegis_debug.log`.
- Resource IDs must match `src/oled_aegis.rc` (icons 101/102); dialog control
  IDs start at 1001, monitor checkboxes at `IDC_MONITOR_BASE + index`.

## Gotchas

- **Cursor**: `ShowCursor` is reference-counted and drifts; always go through
  `EnsureCursorVisible`/`HideCursorForScreenSaver` (loop with a safety bound),
  never call `ShowCursor` directly.
- **`GetProcessNameFromHwnd`** uses `GetModuleBaseNameA` (needs `PROCESS_VM_READ`);
  **`GetProcessNameFromPid`** uses `QueryFullProcessImageNameW` (sandboxed
  Chromium renderers deny VM_READ — use the PID variant for audio-session
  PIDs). Process comparisons are case-insensitive (`_stricmp`).
- **Hidden-but-active windows**: the shell can hide/minimize/`DWMWA_CLOAKED`
  the black windows (taskbar thumbnail previews, Aero Peek). Don't "fix" this
  by trusting window flags alone — the watchdog exists for a reason.
- **`g_mediaPauseOffsetMs` vs per-monitor offsets**: both exist and serve
  different modes; clearing one without the other breaks a mode. See the
  comment block above their declarations in `oled_aegis.c`.
- **Sleep/wake**: `WM_POWERBROADCAST` invalidates the media cache
  (`ResetMediaDetectionCache`); `WM_DISPLAYCHANGE` destroys black windows and
  re-enumerates monitors (config reloaded, settings dialog reopened if open).
- **Audio-only apps** (Spotify, etc.) are intentionally excluded from media
  detection — music does not keep the display on; video does.

## Testing & debugging

1. `build.bat` (or `build.ps1`, or the VS solution) from the project root.
2. Run `build\oled_aegis.exe`; verify the tray icon appears (it switches
   between the active/inactive icons from `images/`) and the process stays
   alive.
3. Enable **Debug Mode** in Settings → watch
   `%APPDATA%\OLED_Aegis\oled_aegis_debug.log` — the timer logs every
   activation/deactivation and media-detection mask change.
4. Manual activation: left-click the tray icon.
5. Before rebuilding, kill the running instance or you'll hit `LNK1104`.

### Breakpoint debugging (Visual Studio)

- Open `oled_aegis.sln`, set the configuration to **Debug**, press **F5**.
  The PDB is generated automatically; breakpoints bind directly in `src\`.
- Key functions: `WinMain`/`HandleCreation` (startup), `HandleTimeout`
  (every timer tick, default 1 s), `UpdateMediaMonitorStates` (media
  detection), `ShowScreenSaverOnMonitor`/`HideScreenSaverOnMonitor`
  (activation), `MonitorWindowProc` (black-window messages),
  `VerifyScreenSaverWindows` (watchdog, throttled ~2 s).
- Gotchas: kill any running instance first (single-instance mutex makes the
  debugged copy exit with a message box). The app is timer-driven — lower the
  idle timeout to 5 s or left-click the tray icon to trigger activation.
  No console: use `LogMessage` (Debug Mode) or `OutputDebugStringA`.
- Without the solution, alternatives: `build.ps1 debug` + attach to the
  process, or `raddbg build\oled_aegis.exe`. Release builds made by
  `build.bat`/`build.ps1` (without `debug`) have no PDB — no breakpoints.

LSP (nvim/WSL) config lives in `.clangd` (uses xwin SDK headers; paths are
machine-specific).
