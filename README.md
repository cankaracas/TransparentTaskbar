# TransparentTaskbar

A 100% native, zero-overhead Windows 11 taskbar customization written in C++20 and WinRT. It transforms the Windows 11 taskbar into floating, macOS-style **Liquid Glass** pills over a completely transparent, click-through bar — featuring a **GPU-composited, native-refresh-rate (60–240+ Hz) auto-hide animation**, **zero** third-party frameworks, **zero** background `.exe` processes, and **~0% idle CPU** usage.

## Features

- **100% Native & Ultra-Lightweight:** Compiled directly with MSVC into a small in-process DLL (`TransparentTaskbar.dll`) and a one-shot injector (`tb_loader.exe`). No Electron, .NET, Python, Windhawk, or persistent background daemon.
- **Zero-Footprint Runtime:** `tb_loader.exe` attaches the module to `explorer.exe` and exits immediately. Once loaded, there are **0** extra running `.exe` processes and **~0%** steady-state idle CPU overhead.
- **Buttery-Smooth GPU Auto-Hide (Native Monitor Refresh Rate):** Replaces Windows 11's choppy, low-FPS `SetWindowPos` taskbar slide with hardware-accelerated `Windows.UI.Composition` (`Translation` + `Opacity` with cubic-bezier easing). DWM renders every frame at your display's native refresh rate (120Hz, 144Hz, 165Hz, 240Hz+) with zero stutter and seamless mid-flight interruption.
- **Instant Bottom-Edge Trigger:** Hovering the bottom 2px edge (or opening Start / Search / flyouts) immediately glides the taskbar into view; moving away smoothly hides it back down after a short grace period.
- **Full-Screen WorkArea Expansion:** Maximized windows extend edge-to-edge behind the taskbar (`ABS_AUTOHIDE | ABS_ALWAYSONTOP`), giving you 100% of your screen real estate.
- **True Click-Through Empty Space:** Empty areas between the app icon dock and the system tray pass mouse clicks directly to whatever window is underneath via a diff-cached `CreateRoundRectRgn` / `SetWindowRgn` mask on `Shell_TrayWnd`.
- **Liquid Glass Floating Pills:** Uses native Windows XAML `HostBackdrop` `AcrylicBrush` + `LinearGradientBrush` specular glass borders (`CornerRadius=12`) on `Taskbar.TaskbarBackground` and `SystemTrayFrameGrid` so icons and tray widgets float smoothly over any window color.
- **Fullscreen Video & Game Aware:** Automatically yields and clears its hit-test region during Direct3D fullscreen games and borderless browser fullscreen videos (such as YouTube / Firefox / Chrome fullscreen) so it never blocks video controls or stays stuck on screen.
- **Automatic Persistence:** Registers cleanly in `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` with cold-boot retry so it activates automatically on every Windows login.

## How It Works (Technical Architecture)

Unlike external overlay tools or fragile API-hooking mods, **TransparentTaskbar** operates as an official **XAML Diagnostics TAP** (`InitializeXamlDiagnosticsEx` + `IVisualTreeServiceCallback2`) directly inside `explorer.exe`:

1. **Zero IAT / Inline Hooks (CFG & XFG Safe):** No function detours or memory patching on system binaries, making it resilient across Windows 11 builds (tested on Windows 11 26H2).
2. **In-Process XAML Tree Styling:** Walks the live `Taskbar.TaskbarFrame` and `SystemTrayFrameGrid` visual tree inside `explorer.exe`'s UI thread:
   - Collapses default opaque `BackgroundFill`, `BackgroundStroke`, and `ScreenEdgeStroke` elements.
   - Applies a translucent `HostBackdrop` `AcrylicBrush` and specular rim gradient to the taskbar icon dock and system tray container, turning them into rounded floating pills.
3. **GPU-Composited Auto-Hide Engine (`Windows.UI.Composition`):**
   - Enables `SHAppBarMessage(ABM_SETSTATE)` with `ABS_AUTOHIDE | ABS_ALWAYSONTOP` so Windows natively expands the desktop `WorkArea` to the full monitor bounds.
   - Intercepts Windows' sluggish native `TrayUI::_Hide()` timer (`kTrayUITimerHide`) and instead animates the XAML island's `Grid` container via GPU `Vector3KeyFrameAnimation` (`Translation`) and `ScalarKeyFrameAnimation` (`Opacity`) with `CompositionScopedBatch` completion callbacks.
   - Keeps `Shell_TrayWnd` at its bottom rect without fighting Z-order (`SWP_NOZORDER`), allowing fullscreen apps and `EVENT_SYSTEM_FOREGROUND` transitions to take precedence cleanly.
   - When hidden, collapses `Shell_TrayWnd`'s `SetWindowRgn` to an empty `0x0` region (`NULLREGION`) for 100% click-through and uses a lock-free `WH_MOUSE_LL` bottom-edge detector (active only while hidden) to trigger instant hover reveal, keeping idle CPU near 0%.
4. **Diff-Cached Region Masking:**
   - Computes the exact union bounding boxes of the active icon dock and system tray on layout changes and applies `SetWindowRgn` to `Shell_TrayWnd` only when geometry changes.
5. **Cross-Desktop IPC:** `tb_loader.exe` supports `--status`, `--refresh`, `--restore`, and `--dump` flags via Win32 named events (`Evt_Apply`, `Evt_Restore`, `Evt_Refresh`, `Evt_Dump`) without keeping any process alive.

## Requirements

- Windows 11 (x64)
- Visual Studio Build Tools / MSVC (`cl.exe`, `link.exe`) + Windows 10/11 SDK (`10.0.26100.0` or compatible) for building from source
- PowerShell 5.1+

## Build & Install

```powershell
# 1. Build TransparentTaskbar.dll and tb_loader.exe
powershell -ExecutionPolicy Bypass -File .\build.ps1

# 2. Install, inject into explorer.exe, and register auto-start on login
powershell -ExecutionPolicy Bypass -File .\install.ps1
```

### Utility Commands

```powershell
# Check live injection status
.\bin\tb_loader.exe --status

# Force refresh XAML layout & region mask
.\bin\tb_loader.exe --refresh

# Temporarily restore default taskbar appearance without uninstalling
.\bin\tb_loader.exe --restore

# Rebuild and hot-reload after editing source files
powershell -ExecutionPolicy Bypass -File .\reload.ps1

# Completely uninstall and restore default Windows 11 taskbar
powershell -ExecutionPolicy Bypass -File .\uninstall.ps1
```

## Project Structure

```text
├── build.ps1          # Discovers MSVC/WinSDK via vswhere and compiles x64 binaries with /guard:cf
├── install.ps1        # Injects into explorer.exe and registers HKCU Run auto-start
├── reload.ps1         # Hot-reloads after source changes
├── uninstall.ps1      # Restores default taskbar and removes startup registration
└── src/
    ├── common.h       # Shared COM CLSID and Win32 named-event IPC constants
    ├── loader.cpp     # One-shot injector & CLI controller (tb_loader.exe)
    ├── tap_dll.cpp    # In-process XAML Diagnostics TAP, Liquid Glass & GPU auto-hide engine
    └── tap_dll.def    # COM exports (DllGetClassObject, DllCanUnloadNow)
```

## License

This project is licensed under the [MIT License](LICENSE).
