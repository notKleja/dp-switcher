#include "Monitors.h"

namespace {
    // Maps a \\.\DISPLAYn adapter device name to its EDD_GET_DEVICE_INTERFACE_NAME path
    // (the actual monitor, not the adapter) by walking EnumDisplayDevicesW's child enumeration.
    std::optional<std::wstring> DeviceInterfacePathFor(const std::wstring& adapterDeviceName) {
        DISPLAY_DEVICEW dd{};
        dd.cb = sizeof(dd);
        for (DWORD i = 0; EnumDisplayDevicesW(adapterDeviceName.c_str(), i, &dd, EDD_GET_DEVICE_INTERFACE_NAME); ++i) {
            if (dd.StateFlags & DISPLAY_DEVICE_ACTIVE) {
                return std::wstring(dd.DeviceID);
            }
        }
        return std::nullopt;
    }

    // The monitor's EDID name as the OS reports it, via the DisplayConfig API: find the
    // active path whose *source* is this GDI device name, then ask for its *target* name.
    std::wstring FriendlyNameFor(const std::wstring& gdiDeviceName) {
        UINT32 pathCount = 0, modeCount = 0;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) return L"";
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
        if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) != ERROR_SUCCESS) return L"";

        for (UINT32 i = 0; i < pathCount; ++i) {
            const auto& p = paths[i];
            DISPLAYCONFIG_SOURCE_DEVICE_NAME src{};
            src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            src.header.size = sizeof(src);
            src.header.adapterId = p.sourceInfo.adapterId;
            src.header.id = p.sourceInfo.id;
            if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS) continue;
            if (gdiDeviceName != src.viewGdiDeviceName) continue;

            DISPLAYCONFIG_TARGET_DEVICE_NAME tgt{};
            tgt.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
            tgt.header.size = sizeof(tgt);
            tgt.header.adapterId = p.targetInfo.adapterId;
            tgt.header.id = p.targetInfo.id;
            if (DisplayConfigGetDeviceInfo(&tgt.header) == ERROR_SUCCESS) return tgt.monitorFriendlyDeviceName;
        }
        return L"";
    }
}

std::optional<MonitorIdentity> GetMonitorIdentity(HMONITOR mon) {
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!mon || !GetMonitorInfoW(mon, &mi)) return std::nullopt;

    MonitorIdentity id;
    id.deviceName = mi.szDevice;
    if (auto path = DeviceInterfacePathFor(mi.szDevice)) {
        id.devicePath = *path;
    } else {
        // No interface path (unusual: e.g. a virtual display). Fall back to the adapter name
        // so the monitor is at least identifiable within this session.
        id.devicePath = mi.szDevice;
    }
    return id;
}

std::optional<HMONITOR> ResolveMonitor(const MonitorIdentity& id) {
    if (id.devicePath.empty()) return std::nullopt;
    struct Ctx { const MonitorIdentity* id; HMONITOR found; };
    Ctx ctx{ &id, nullptr };

    EnumDisplayMonitors(nullptr, nullptr,
        [](HMONITOR hMon, HDC, LPRECT, LPARAM lp) -> BOOL {
            auto* ctx = reinterpret_cast<Ctx*>(lp);
            if (auto current = GetMonitorIdentity(hMon)) {
                if (current->devicePath == ctx->id->devicePath) {
                    ctx->found = hMon;
                    return FALSE;
                }
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&ctx));

    if (ctx.found) return ctx.found;
    return std::nullopt;
}

std::optional<HMONITOR> GetPrimaryOrFirstMonitor() {
    HMONITOR primary = MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    if (primary) return primary;

    HMONITOR first = nullptr;
    EnumDisplayMonitors(nullptr, nullptr,
        [](HMONITOR hMon, HDC, LPRECT, LPARAM lp) -> BOOL {
            *reinterpret_cast<HMONITOR*>(lp) = hMon;
            return FALSE;
        }, reinterpret_cast<LPARAM>(&first));
    if (first) return first;
    return std::nullopt;
}

std::vector<MonitorEntry> EnumerateMonitors() {
    std::vector<MonitorEntry> out;
    EnumDisplayMonitors(nullptr, nullptr,
        [](HMONITOR hMon, HDC, LPRECT, LPARAM lp) -> BOOL {
            auto* list = reinterpret_cast<std::vector<MonitorEntry>*>(lp);
            MONITORINFOEXW mi{};
            mi.cbSize = sizeof(mi);
            if (!GetMonitorInfoW(hMon, &mi)) return TRUE;
            MonitorEntry e;
            e.handle = hMon;
            e.rect = mi.rcMonitor;
            e.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
            if (auto id = GetMonitorIdentity(hMon)) e.identity = *id;
            e.friendlyName = FriendlyNameFor(mi.szDevice);
            list->push_back(std::move(e));
            return TRUE;
        }, reinterpret_cast<LPARAM>(&out));
    return out;
}
