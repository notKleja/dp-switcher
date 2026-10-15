#pragma once
#include <windows.h>
#include <string>

// Persisted under HKCU\Software\DPSwitcher. Everything here is user-tunable from the tray
// menu's Settings/Diagnostics submenus; the values are read once at startup and kept in sync
// by the Save* calls.
struct Settings {
    std::wstring monitorDevicePath;   // empty = primary monitor at startup
    UINT revealGraceMs = 0;
    std::wstring forcedRenderer;      // "A" | "B" | "C" | "" (automatic)
    bool ddcCacheHandles = true;      // keep the PHYSICAL_MONITOR handle open between reads
};

Settings LoadSettings();
void SaveMonitorDevicePath(const std::wstring& path);
void SaveRevealGraceMs(UINT ms);
void SaveForcedRenderer(const std::wstring& tier);

// HKCU\...\CurrentVersion\Run entry pointing at this executable.
bool IsAutostartEnabled();
bool SetAutostartEnabled(bool enabled);
