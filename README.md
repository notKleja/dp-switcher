# DP Switcher for Windows

A Windows system tray app for switching a monitor between a PC on DisplayPort and a Mac on USB-C using DDC/CI. Includes a transition overlay and monitor selection.

The current input values are configured in `DPSwitcherWin/src/main.cpp`: DisplayPort `0x0F` and USB-C `0x1A`. These values match the intended ASUS PG32UCDM setup; other monitors may use different values.

## Usage

Run `DPSwitcher.exe`, then right-click its tray icon to select **Switch to USB-C (Mac)** or **Switch to DisplayPort (PC)**. Left-clicking the tray icon reads the current input and toggles it. Choose **Exit** to close the app.

The app switches video input only; it does not explicitly route the monitor's USB/KVM connection. Background DDC polling is disabled by default following observed KVM overrides while the app was running. Only explicit switch requests contact the monitor. Consequently, automatic return detection and the associated return animation are disabled. The monitor-level cause and hardware effectiveness of this mitigation still require confirmation.

Settings are stored under `HKEY_CURRENT_USER\Software\DPSwitcher`. Logs are written to `%LOCALAPPDATA%\DPSwitcher\log.txt`, with a temporary-directory fallback.

## Build and test

Requires Windows, Visual Studio 2022 with the C++ desktop workload and Windows SDK, and CMake 3.24 or newer.

```powershell
cd DPSwitcherWin
cmake --preset vs2022
cmake --build --preset release
ctest --preset release
```

The standalone executable is produced at `DPSwitcherWin/build/Release/DPSwitcher.exe`. The MSVC runtime is linked statically.

The build also includes `DdcProbe`, a diagnostic utility that performs live monitor queries, and headless controller tests. The tests use fake monitor, timer, clock, and renderer interfaces; they do not switch physical monitor inputs.

Generated builds and executable backups are excluded from version control.
