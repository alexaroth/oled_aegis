# Build Instructions for OLED Aegis

## Prerequisites

You need Visual Studio installed with the C++ build tools. You can use:
- Visual Studio 2022/2019/2017/2015 (Community edition is free)
- Or just the "Build Tools for Visual Studio" from Microsoft

## Quick Start (Windows)

### Option 1: Visual Studio Solution (Easiest for Debugging)

1. Open `oled_aegis.sln` in Visual Studio 2022
2. Set the configuration to **Debug** (or Release) and press **F5**

The project is configured to match the command-line build (see below):
same source files, `INITGUID` define, libraries, and output location
(`build\oled_aegis.exe`). Both configurations produce a PDB, so
breakpoints work out of the box.

### Option 2: Using build.bat (Easiest)

1. Open **Developer Command Prompt for VS 2022** (or your version)
   - Find it in the Start Menu: "Developer Command Prompt for VS 2022"

2. Navigate to the project directory:
   ```batch
   cd C:\path\to\oled_aegis
   ```

3. Run the build script:
   ```batch
   build.bat
   ```

4. Run the application:
   ```batch
   oled_aegis.exe
   ```

### Option 3: Using build.ps1

1. Open PowerShell (can be run from WSL or Windows)

2. Navigate to the project directory:
   ```powershell
   cd C:\path\to\oled_aegis
   ```

3. Run the build script:
   ```powershell
   .\build.ps1
   ```

4. For a debug build:
   ```powershell
   .\build.ps1 debug
   ```

### Option 4: Using build.sh from WSL

1. Open WSL terminal

2. Navigate to the project directory (mounted Windows drive):
   ```bash
   cd /mnt/c/path/to/oled_aegis
   ```

3. Run the build script:
   ```bash
   ./build.sh
   ```

4. For a debug build:
   ```bash
   ./build.sh debug
   ```

#### Note on WSL development

I don't really like Visual Studio and prefer my nvim/tmux development
environment carried over from Linux. For Windows apps, I recommend
[raddbg](https://github.com/EpicGamesExt/raddebugger) for debugging.

While WSL is great, it's obviously not an ideal Windows application development
environment. To workaround LSP errors (e.g. missing "windows.h"), one can
install `cargo-xwin` to obtain the necessary Windows SDK components:

```bash
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y
source "$HOME/.cargo/env"
cargo install xwin --locked
xwin --accept-license splat --output ~/.xwin
# Update .clangd paths!
# Unfortunately clangd configuration doesn't support env vars like $USER.
```

## Manual Build

If you prefer to compile manually from a Developer Command Prompt:

```batch
cl.exe src\oled_aegis.c src\util.c src\logging.c src\config.c src\monitors.c src\media.c src\screensaver.c src\settings.c /Fe:oled_aegis.exe /O2 /MD /link user32.lib shell32.lib ole32.lib uuid.lib gdi32.lib advapi32.lib comctl32.lib powrprof.lib
```

For breakpoint debugging from the command line, build with `build.ps1 debug`
(produces `build\oled_aegis.pdb`) and attach a debugger (Visual Studio,
raddbg) to the running process.

## Source Layout

The source is split into small modules, all sharing the `oled_aegis.h` header:

| File | Contents |
|---|---|
| `src/oled_aegis.c` | Entry point (`WinMain`), main window, idle-check timer state machine, tray icon |
| `src/monitors.c` | Monitor enumeration (GDI + DisplayConfig) and lookup helpers |
| `src/screensaver.c` | Black screen saver windows: show/hide, topmost, watchdog, shell-window closing |
| `src/media.c` | Media detection: WASAPI audio sessions, window scan, DEFINE_GUID definitions |
| `src/settings.c` | Settings dialog UI and `ApplySettings` |
| `src/config.c` | `.ini` load/save, value clamping, startup registry |
| `src/logging.c` | Debug log file (append + rotation) |
| `src/util.c` | Small shared helpers: paths, cursor visibility |

`/D "INITGUID"` is required: the MMDevice/audio-session GUIDs are `DEFINE_GUID`'d
in `media.c`, and the build flag makes those definitions emit with SELECTANY
storage.

## Build Options

### PowerShell/build.ps1 Features

The PowerShell build script includes:

* **Environment Variable Caching**: MSVC environment variables are cached in `build/vc_env.txt` to speed up subsequent builds
* **Build Type Selection**: Choose between `release` (optimized) or `debug` builds
* **Automatic Directory Creation**: Creates `build/` directory if it doesn't exist
* **Error Handling**: Proper error messages if Visual Studio is not found

### Compiler Flags

* **/O2** - Maximum optimization (fastest code, smallest size) - release builds only
* **/FC** - Display full path in diagnostics
* **/Zi** - Generate debugging information - debug builds only
* **/MD** - Link with multi-threaded DLL runtime (release)
* **/MDd** - Link with debug multi-threaded DLL runtime (debug)

### Linked Libraries

* **user32.lib** - Windows user interface functions
* **shell32.lib** - Shell functions (for system tray)
* **ole32.lib** - COM functions
* **uuid.lib** - GUID definitions for COM interfaces
* **gdi32.lib** - Graphics Device Interface (for creating solid black brushes)
* **advapi32.lib** - Advanced Windows APIs (for registry functions)
* **comctl32.lib** - Common Controls library (for NumericUpDown control)
* **powrprof.lib** - Power Profile library (for media detection)

## Troubleshooting

### "cl.exe is not recognized"

This means you're not using a Developer Command Prompt. You need to:
1. Open Start Menu
2. Search for "Developer Command Prompt for VS 2022"
3. Run that instead of regular cmd.exe

### Build succeeds but application crashes on startup

Make sure you have:
- Windows 10 or Windows 11

## Clean Build

### From Windows (build.bat)
```batch
del oled_aegis.exe
del oled_aegis.obj
build.bat
```

### From PowerShell (build.ps1)
```powershell
Remove-Item build\oled_aegis.exe -ErrorAction SilentlyContinue
.\build.ps1
```

### From WSL (build.sh)
```bash
rm -f build/oled_aegis.exe
./build.sh
```

## Clearing Environment Cache (PowerShell/WSL only)

If you change your Visual Studio installation or environment settings, delete the cached environment variables:

```bash
rm build/vc_env.txt
```

Then rebuild to regenerate the cache.

## Running without Installing

The compiled `oled_aegis.exe` is a standalone executable. You can:
- Copy it anywhere on your computer
- Run it from a USB drive
- No installation required

The application will automatically create its config file in `%APPDATA%\OLED_Aegis\oled_aegis.ini` on first run.

## Why didn't you just make a custom Screen Saver (`.scr`)?

From my testing, it is not possible to have a real Windows screensaver (`.scr`
launched by the OS) affect only one monitor while leaving the others showing
their normal desktop / video playback.

Even if a `.scr` program specifically draws on only one monitor, when Windows
activates a screensaver due to timeout, Explorer switches into a screensaver
mode and the desktop window manager creates an internal blank backdrop surface
which is applied to *all* monitors.

Also, one of the primary issues I had with the built-in screen saver was it's
apparent disabling of Bluetooth media controls, so using the built-in screen
saver wouldn't resolve this particular issue.

