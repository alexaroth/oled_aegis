# AGENTS.md — OLED Aegis

Project instructions for AI agents and developers working on this repository. User has 2 OLEDs and an IPS. One OLED is primary monitor, second monitor is the IPS, and the third
  monitor is the secondary OLED. The user wants to prevent burn-in on the third monitor, the OLED.

## What this is

A Windows tray application that prevents OLED burn-in by blackening **individual monitors** after an idle timeout (a per-monitor "screen saver"), instead of all monitors at once.
  It also detects video/media playback and keeps the display awake (or the screen saver off) while media plays — per monitor when enabled.

Pure Win32 **C** (MSVC), no C++, no framework, no third-party runtime. Windows 10/11 only.

## Build & run

- **`oled_aegis.sln`** (Visual Studio 2022, x64, Debug/Release) — open and press F5. The project mirrors the command-line build exactly (`INITGUID` define, same libs, `.rc`
  compiled, output to `build\oled_aegis.exe`, PDB produced in both configs).
- **`build.bat`** — compile + link everything. Requires a Developer Command Prompt (vcvars). Output: `build/oled_aegis.exe`.
- **`build.ps1 [release|debug]`** — locates VS itself, caches the MSVC env in `build/vc_env.txt` (delete that file if VS paths change). Also usable from WSL via `build.sh`.
- **`/D "INITGUID"` is mandatory.** The MMDevice/audio-session GUIDs are `DEFINE_GUID`'d in `src/media.c` only; without INITGUID they don't emit definitions and the link fails.
  Don't move those `DEFINE_GUID` lines to a header (would duplicate definitions per TU).
- GitHub Actions builds via `build.bat` on push to `main` touching `src/**`.
- The exe is a tray app: to rebuild while it's running, kill it first (`taskkill /IM oled_aegis.exe /F`) or the linker fails with `LNK1104`.

## Source layout

All modules share `src/oled_aegis.h` (constants, typedefs, `extern` globals, cross-module prototypes).

| File | Responsibility |
|---|---|
| `src/oled_aegis.c` | `WinMain`, hidden main window (`OLEDAegisWindow`), `HandleCreation` (init), `HandleTimeout` (the idle-check timer state machine), tray icon, `WndProc` |
| `src/screensaver.c` | Black windows (one per monitor): show/hide, fade-to-black, hide/minimize vetoes, topmost reassertion, watchdog, shell-window closing |
| `src/media.c` | Media core: `ES_DISPLAY_REQUIRED` gate, scan orchestration, the **ordered rule table** (policy), scan cache, logging. Owns `DEFINE_GUID` blocks |
| `src/media_classify.c` | Exe → process-class table (browsers/video/audio-only/background) + title hints. **All classification knowledge lives here**: one-line entry per app |
| `src/media_audio.c` | WASAPI audible-session scan (active session + peak-meter gate) |
| `src/media_windows.c` | Window evidence: visible window rects → per-monitor mapping, muted-media candidates, browser-window diagnostics |
| `src/media_motion.c` | Per-monitor content motion probe (4×32×32 inset blocks, fullscreen skip, saver-frozen logic) |
| `src/monitors.c` | Monitor enumeration (GDI + DisplayConfig), owns `g_monitors`/`g_monitorStates`, lookup helpers |
| `src/settings.c` | Settings dialog (hand-built controls, DPI-scaled), `ApplySettings` |
| `src/config.c` | `oled_aegis.ini` load/save, clamping, startup registry entry |
| `src/util.c` | Path resolution, cursor visibility helpers |
| `src/logging.c` | Debug log file with 1 MB rotation |

## Architecture notes

- **Single instance** via named mutex `OLEDAegis_SingleInstance` (held for app lifetime).
- **No message pump complexity**: one hidden window, one `WM_TIMER` (`TIMER_IDLE_CHECK`, interval = `checkInterval`, 250–10000 ms). All idle logic lives in `HandleTimeout`
  (`src/oled_aegis.c`).
- **Idle detection**: global mode uses `GetLastInputInfo`; per-monitor mode tracks cursor position + foreground-window location per monitor.
- **Media detection pipeline** (`src/media_*.c`, called from `HandleTimeout`):
  1. Cheap gate: `CallNtPowerInformation(SystemExecutionState)` checks `ES_DISPLAY_REQUIRED`; skip everything if unset.
  2. WASAPI: enumerate default render endpoint sessions, keep processes whose session is `AudioSessionStateActive` **and** peak meter `> 0.0001f` (filters paused/silent
    sessions).
  3. `EnumWindows` collects window evidence: visible, non-cloaked, non-tool windows of **audio-active** processes map their rect → monitors by **geometry** (no title-hint gate: a
    window that makes sound is where the content is, be it a game, Discord, or a browser). Music (`AUDIO_ONLY`) and background/launcher noise (`BACKGROUND`) are the only classes
    that never map, even with a visible window — that audio must not keep a display on. Muted candidates (visible browser/media windows with no audio) are collected as rects; the
    motion probe decides whether their monitor counts. Results cached 2 s.
  4. The **rule table** in `src/media.c` (`g_rules`, applied in array order — order IS policy, encodes the bug history) decides the final per-monitor mask:
     - `ambiguity-clear` — a SINGLE audio-active process with >1 visible window (e.g. a browser with two windows: its audio can't be attributed to one): that process's window
       mapping is dropped, the motion probe decides (a paused tab on the OLED must not block the screen saver while playback is elsewhere). Different audible processes on
       different monitors are NOT ambiguous — geometry maps a game and a Discord window each to its own monitor, so only the per-process window count matters. Fullscreen
       foreground windows keep their mapping.
     - `motion` — a monitor counts as media while its content moves (15 s grace; probe frozen while the screen saver covers the monitor, so the stale grace can't deactivate it;
       captures skipped per-monitor under fullscreen windows of non-media processes — a fullscreen browser/media player is sampled).
     - `muted` — muted-media rects map only while inside the 30 s audio grace **and** the monitor's content moves (or a fullscreen foreground window covers it); a paused window
       is static, so it never wakes the screen saver.
     - `grace-keep` — fresh scan mapped nothing inside the grace window (video minimized during a quiet passage): keep the cached mask (union, not overwrite).
     - `no-audio` / `browser` / `audio-only` — skip the unattributed-audio rule (no audible audio; only browsers audible but nothing visible; only music/podcast/background
       audio audible).
     - `fullscreen` — foreground window is fullscreen on a monitor and its process is audio-active (window PID match first, exe-name fallback): attribute playback to
       that monitor instead of leaving it unattributed. EAC-protected games (War Thunder) fail `OpenProcess(VM_READ)`, so the match uses the window PID.
     - `unattributed` — audible audio with nothing attributable (no visible window of the audible process, no fullscreen foreground) is treated as background/UI/hidden audio
       and blocks NOTHING (the per-monitor burn-in goal wins; the motion probe already ran, so visible motion got its grace first). There is deliberately **no block-all
       fallback** — that was the whack-a-mole engine that woke the OLED for every unclassified app's sound. The decision is logged with `reason=<rule name>` in the `mask=` line.
  - Browsers are matched **by exe name, not PID** (Chromium audio sessions belong to renderer processes; windows to the main process).
  - Classification (`media_classify.c`) is a NEGATIVE list only: browsers/players for mute/motion semantics, plus audio-only (`spotify.exe`…), background (`nvcontainer.exe`),
    and launcher/UI noise (`leagueclient*.exe`, `riotclientservices.exe`) that must never block. Every other audible app with a visible window is mapped by geometry — no
    per-app positive whitelist, so new games/players/streamers wake zero monitors.
- **Modes** (all in `HandleTimeout`):
  - Global input + global media: all-on/all-off.
  - Global input + per-monitor media: per-monitor idle "pause" offsets (`mediaPauseOffsetMs` in `MonitorState`, global `g_mediaPauseOffsetMs`), cleared on real input.
  - Per-monitor input: independent per-monitor timers.
- **Manual activation cooldown**: `SendInput` Escape presses (closing Start Menu etc. before showing the screen saver) register as user input, so activation sets a 2500 ms
  cooldown (`MANUAL_ACTIVATION_COOLDOWN_MS`) to avoid instantly deactivating.
- **Robustness layer** (`src/screensaver.c`): `WM_WINDOWPOSCHANGING`/`WM_STYLECHANGING` vetoes on the black windows, 5 s topmost reassert, watchdog that restores
  hidden/minimized/moved/cloaked windows, and a throttled pixel probe that verifies the screen is actually black.
- **Fade-to-black transition** (`fadeDurationMs`, 0 = instant): when enabled, black windows are layered (`WS_EX_LAYERED`) and their whole-window alpha is animated by
  `UpdateFades` on a dedicated `TIMER_FADE` (~15 ms) via `SetLayeredWindowAttributes` — activation fades 0→255 (desktop darkens to black), deactivation fades 255→0 before hiding.
  Fades can be reversed mid-flight (e.g. re-show during fade-out) from the current alpha, and the watchdog's pixel probe is skipped while a fade is in progress since the screen
  is legitimately not black yet.
- **Pixel shift compensation**: black windows are expanded beyond monitor bounds by `pixelShiftCompensation` px (4–8 for QD-OLED panels).
- **Persistence**: config in `%APPDATA%\OLED_Aegis\oled_aegis.ini`; monitors keyed by persistent `monitorDevicePath` (legacy `monitor0=` and `monitorEnabled_<GDI name>` keys
  still read for migration). `LoadConfig` resets enablement first, so monitors without an entry start disabled. Entries seen in the file are remembered and re-emitted by
  `SaveConfig` even when the monitor is currently disconnected, so a temporary disconnect (or an Apply while unplugged) can't erase the saved selection. Fallback: only a config
  with **no** monitor entries at all enables the primary monitor; if entries exist but none match, nothing is enabled (configured monitors are just disconnected) until the saved
  monitor returns.

## Conventions

- Plain C99-style C (MSVC). No C++, no exceptions, no STL.
- **Braces: Allman style, enforced.** Opening braces go on their own line for all blocks: functions, `if`/`else`/`else if`/`for`/`while`/`do`, `switch`/`case` bodies,
  and `typedef struct`/`typedef enum`. The brace aligns with the statement's opening line, not a continuation line of a wrapped condition. `} else` chains are split as
  `}` / `else if (...)`. Exceptions: aggregate/array initializers (`= {`, `{0}`, table rows like `{ RuleX, 0 },`) stay on the same line, and brace-less single statements are
  fine. `reformat_allman.py` (repo root) converts a K&R tree to this style and is idempotent; run it after editing if you favored K&R, then rebuild to verify.
- Shared state is global by design: define the variable in its owner module, declare `extern` in `oled_aegis.h`. Module-internal helpers are `static`. Owner modules: `g_app`,
  `g_blackBrush`, `g_hIconActive/Inactive` → `oled_aegis.c`; `g_hSettingsDialog` → `settings.c`; `g_monitorCount`, `g_monitors`, `g_monitorStates` → `monitors.c`; `g_logFile` →
  `logging.c`.
- **Media policy** lives only in the `g_rules` table in `src/media.c` (ordered: order IS policy); process knowledge lives only in the table in `src/media_classify.c`. Don't add
  per-process or per-rule logic elsewhere. The `mask=` log line carries a `reason=<rule name>` field identifying which rule decided the scan.
- **Max line length: 180 chars** (code and comments). Wrap long lines.
- **Comments: 1 line when they fit, max 2 lines, ≤180 chars each.** Fill the full width —
  never split early; never state the obvious; explain *why* (especially hard-won bug history).
- `LogMessage(...)` is the debug facility (gated by `debugMode`); keep existing log lines' wording/format stable — the user greps them for diagnostics. Log file:
  `%APPDATA%\OLED_Aegis\oled_aegis_debug.log`.
- Resource IDs must match `src/oled_aegis.rc` (icons 101/102); dialog control IDs start at 1001, monitor checkboxes at `IDC_MONITOR_BASE + index`.

## Gotchas

- **Cursor**: `ShowCursor` is reference-counted and drifts; always go through `EnsureCursorVisible`/`HideCursorForScreenSaver` (loop with a safety bound), never call `ShowCursor`
  directly.
- **`GetProcessNameFromHwnd`** uses `GetModuleBaseNameA` (needs `PROCESS_VM_READ`); **`GetProcessNameFromPid`** uses `QueryFullProcessImageNameW` (sandboxed Chromium renderers
  deny VM_READ — use the PID variant for audio-session PIDs). Process comparisons are case-insensitive (`_stricmp`).
- **Hidden-but-active windows**: the shell can hide/minimize/`DWMWA_CLOAKED` the black windows (taskbar thumbnail previews, Aero Peek). Don't "fix" this by trusting window flags
  alone — the watchdog exists for a reason.
- **`g_mediaPauseOffsetMs` vs per-monitor offsets**: both exist and serve different modes; clearing one without the other breaks a mode. See the comment block above their
  declarations in `oled_aegis.c`.
- **Sleep/wake**: `WM_POWERBROADCAST` invalidates the media cache (`ResetMediaDetectionCache`); `WM_DISPLAYCHANGE` destroys black windows and re-enumerates monitors (config
  reloaded, settings dialog reopened if open).
- **Audio-only apps** (Spotify, etc.) are intentionally excluded from media detection — music does not keep the display on; video does.

## Testing & debugging

1. `build.bat` (or `build.ps1`, or the VS solution) from the project root.
2. Run `build\oled_aegis.exe`; verify the tray icon appears (it switches between the active/inactive icons from `images/`) and the process stays alive.
3. Enable **Debug Mode** in Settings → watch `%APPDATA%\OLED_Aegis\oled_aegis_debug.log` — the timer logs every activation/deactivation and media-detection mask change.
4. Manual activation: left-click the tray icon.
5. Before rebuilding, kill the running instance or you'll hit `LNK1104`.

### Breakpoint debugging (Visual Studio)

- Open `oled_aegis.sln`, set the configuration to **Debug**, press **F5**. The PDB is generated automatically; breakpoints bind directly in `src\`.
- Key functions: `WinMain`/`HandleCreation` (startup), `HandleTimeout` (every timer tick, default 1 s), `UpdateMediaMonitorStates` (media detection),
  `ShowScreenSaverOnMonitor`/`HideScreenSaverOnMonitor` (activation), `MonitorWindowProc` (black-window messages), `VerifyScreenSaverWindows` (watchdog, throttled ~2 s).
- Gotchas: kill any running instance first (single-instance mutex makes the debugged copy exit with a message box). The app is timer-driven — lower the idle timeout to 5 s or
  left-click the tray icon to trigger activation. No console: use `LogMessage` (Debug Mode) or `OutputDebugStringA`.
- Without the solution, alternatives: `build.ps1 debug` + attach to the process, or `raddbg build\oled_aegis.exe`. Release builds made by `build.bat`/`build.ps1` (without
  `debug`) have no PDB — no breakpoints.

LSP (nvim/WSL) config lives in `.clangd` (uses xwin SDK headers; paths are machine-specific).
