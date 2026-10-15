#pragma once
#include "../pch.h"
#include <optional>
#include <string>
#include <vector>

// Tracks a monitor by its stable device interface path (not HMONITOR, which is invalidated
// by display reconfiguration and can churn during an input switch's hot-plug-detect pulse).
struct MonitorIdentity {
    std::wstring devicePath;   // from DISPLAY_DEVICEW::DeviceID via EDD_GET_DEVICE_INTERFACE_NAME
    std::wstring deviceName;   // \\.\DISPLAY1 style, informational only
};

struct MonitorEntry {
    HMONITOR handle = nullptr;
    MonitorIdentity identity;
    std::wstring friendlyName;  // e.g. "DELL U2723QE"; empty when the OS has none
    RECT rect{};
    bool primary = false;
};

std::optional<MonitorIdentity> GetMonitorIdentity(HMONITOR mon);
// Matches strictly by device path: a monitor that is not currently connected resolves to
// nothing rather than to whichever other display happens to have inherited its name.
std::optional<HMONITOR> ResolveMonitor(const MonitorIdentity& id);
std::optional<HMONITOR> GetPrimaryOrFirstMonitor();
std::vector<MonitorEntry> EnumerateMonitors();
