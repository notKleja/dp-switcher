#include "pch.h"
#include "AppMessages.h"
#include "TrayIcon.h"
#include "OverlayWindow.h"
#include "Ddc.h"
#include "Settings.h"
#include "SwitchController.h"
#include "util/Log.h"
#include "util/Monitors.h"
#include "util/Dispatcher.h"
#include "util/SingleInstance.h"
#include "render/CompositionBackdropRenderer.h"
#include "render/DuplicationBlurRenderer.h"
#include "render/BlackFadeRenderer.h"
#include "render/TransitionStyle.h"
#include <memory>
#include <shellapi.h>
#include <gdiplus.h>

namespace {
    // Matches MonitorDDC.psm1's original values: this PC's own DisplayPort input vs. the
    // Mac's USB-C input on the same VCP 0x60 feature.
    constexpr BYTE kDpValue = 15;
    constexpr BYTE kUsbcValue = 26;

    // GUID_CONSOLE_DISPLAY_STATE {6FE69556-704A-47A0-8F24-C28D936FDA47}: Windows' own view of
    // whether the display is on/off/dimmed. Logged purely as a diagnostic, so we can see
    // whether the OS notices anything at all when the monitor switches inputs (e.g. an HPD
    // pulse) and how that lines up with our DDC-based detection.
    constexpr GUID kGuidConsoleDisplayState =
        { 0x6fe69556, 0x704a, 0x47a0, { 0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47 } };

    constexpr UINT_PTR kFreezeDiagTimerId = 0xDF99;
    constexpr wchar_t kSingleInstanceMutex[] = L"Local\\DPSwitcher.SingleInstance";

    struct Win32TimerHost final : ITimerHost {
        HWND hwnd = nullptr;
        void Start(UINT_PTR id, UINT intervalMs) override { SetTimer(hwnd, id, intervalMs, nullptr); }
        void Stop(UINT_PTR id) override { KillTimer(hwnd, id); }
    };

    struct SystemClock final : IClock {
        std::chrono::steady_clock::time_point Now() const override { return std::chrono::steady_clock::now(); }
        bool LocalInputWithin(DWORD ms) const override {
            LASTINPUTINFO lii{ sizeof(lii) };
            return GetLastInputInfo(&lii) && (GetTickCount() - lii.dwTime) < ms;
        }
    };

    OverlayWindow g_overlay;
    TrayIcon g_tray;
    DdcWorker g_ddc;
    SwitchController g_controller;
    Win32TimerHost g_timers;
    SystemClock g_clock;
    Settings g_settings;
    MonitorIdentity g_monitorIdentity;
    HWND g_controllerHwnd = nullptr;

    // RtlGetVersion bypasses the GetVersionEx app-compat shim; declared in ntdll but not in
    // a public header, so it's resolved via GetProcAddress.
    DWORD GetOsBuildNumber() {
        using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOEXW*);
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (!ntdll) return 0;
        auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
        if (!fn) return 0;
        OSVERSIONINFOEXW vi{ sizeof(vi) };
        return (fn(&vi) == 0) ? vi.dwBuildNumber : 0;
    }

    std::unique_ptr<IOverlayRenderer> TryTierA(HWND overlay, HMONITOR mon) {
        if (GetSystemMetrics(SM_REMOTESESSION) || GetOsBuildNumber() < 17134) return nullptr;
        auto r = std::make_unique<CompositionBackdropRenderer>();
        if (r->Initialize(overlay, mon)) return r;
        return nullptr;
    }

    std::unique_ptr<IOverlayRenderer> TryTierB(HWND overlay, HMONITOR mon) {
        if (GetSystemMetrics(SM_REMOTESESSION) || GetOsBuildNumber() < 19041) return nullptr;
        auto r = std::make_unique<DuplicationBlurRenderer>();
        if (r->Initialize(overlay, mon)) return r;
        return nullptr;
    }

    std::unique_ptr<IOverlayRenderer> TryTierC(HWND overlay, HMONITOR mon) {
        auto r = std::make_unique<BlackFadeRenderer>();
        if (r->Initialize(overlay, mon)) return r;
        return nullptr;
    }

    // Returns null when every tier failed; the controller then runs without an overlay.
    std::unique_ptr<IOverlayRenderer> SelectRenderer(HWND overlay, HMONITOR mon, const std::wstring& forced) {
        if (forced == L"A") { if (auto r = TryTierA(overlay, mon)) return r; }
        if (forced == L"B") { if (auto r = TryTierB(overlay, mon)) return r; }
        if (forced == L"C") { if (auto r = TryTierC(overlay, mon)) return r; }
        if (!forced.empty()) Log(L"Forced renderer %s unavailable, falling back to Tier C", forced.c_str());

        if (forced.empty()) {
            if (auto r = TryTierA(overlay, mon)) return r;
            Log(L"Tier A unavailable, falling back to Tier B");
            if (auto r = TryTierB(overlay, mon)) return r;
            Log(L"Tier B unavailable, falling back to Tier C");
        }
        auto r = TryTierC(overlay, mon);
        if (!r) Log(L"All renderer tiers failed to initialize; switching will run without an overlay");
        return r;
    }

    RECT MonitorRect(HMONITOR mon) {
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(mon, &mi);
        return mi.rcMonitor;
    }

    // The monitor renderers should bind to: the tracked one, or (only while it is absent)
    // whatever is available so a renderer can at least be constructed.
    HMONITOR RenderMonitor() {
        if (auto m = ResolveMonitor(g_monitorIdentity)) return *m;
        return GetPrimaryOrFirstMonitor().value_or(nullptr);
    }

    void RebuildRenderer() {
        if (g_controller.State() != SwitchState::Idle) {
            g_tray.ShowBalloon(L"Monitor Switcher", L"Finish or cancel the current switch before changing the renderer.");
            return;
        }
        // Destroy the old renderer *before* constructing its replacement: DirectComposition
        // allows a single topmost target per HWND, and Tier B must drop its display affinity.
        g_controller.ReplaceRenderer(nullptr);
        HMONITOR mon = RenderMonitor();
        if (!mon) return;
        auto renderer = SelectRenderer(g_overlay.Handle(), mon, g_settings.forcedRenderer);
        Log(L"Active renderer: %s", renderer ? renderer->Name() : L"(none)");
        g_controller.ReplaceRenderer(std::move(renderer));
    }

    // Re-resolve the tracked monitor and tell the controller whether it is present.
    void RefreshMonitorTarget() {
        if (auto m = ResolveMonitor(g_monitorIdentity)) {
            g_controller.OnMonitorChanged(*m, MonitorRect(*m));
        } else {
            g_controller.OnMonitorLost();
        }
    }

    MonitorIdentity IdentityForSetting(const std::wstring& devicePath) {
        if (!devicePath.empty()) return MonitorIdentity{ devicePath, L"" };
        if (auto primary = GetPrimaryOrFirstMonitor()) {
            if (auto id = GetMonitorIdentity(*primary)) return *id;
        }
        return MonitorIdentity{};
    }

    void ApplyMonitorSelection(const std::wstring& devicePath) {
        if (g_controller.State() != SwitchState::Idle) {
            g_tray.ShowBalloon(L"Monitor Switcher", L"Finish or cancel the current switch before changing the target monitor.");
            return;
        }
        g_settings.monitorDevicePath = devicePath;
        SaveMonitorDevicePath(devicePath);
        g_monitorIdentity = IdentityForSetting(devicePath);
        Log(L"settings: target monitor -> %s", devicePath.empty() ? L"(primary)" : devicePath.c_str());
        RefreshMonitorTarget();
        RebuildRenderer(); // Tier B is bound to a specific DXGI output
    }

    std::vector<TrayMonitorEntry> BuildMonitorEntries() {
        std::vector<TrayMonitorEntry> out;
        for (const auto& m : EnumerateMonitors()) {
            wchar_t size[48];
            swprintf_s(size, L" (%ldx%ld%s)", m.rect.right - m.rect.left, m.rect.bottom - m.rect.top,
                m.primary ? L", primary" : L"");
            std::wstring label = (m.friendlyName.empty() ? m.identity.deviceName : m.friendlyName) + size;
            const bool selected = !g_settings.monitorDevicePath.empty() && m.identity.devicePath == g_settings.monitorDevicePath;
            out.push_back({ label, m.identity.devicePath, selected });
        }
        return out;
    }

    LRESULT CALLBACK ControllerWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
        switch (m) {
        case WM_APP_TRAYICON:
            g_tray.HandleCallback(w, l);
            return 0;
        case WM_COMMAND:
            g_tray.HandleCommand(w);
            return 0;
        case WM_APP_DDC_RESULT_MSG: {
            std::unique_ptr<DdcResult> result(reinterpret_cast<DdcResult*>(w));
            g_controller.OnDdcResult(*result);
            return 0;
        }
        case WM_APP_RENDER_ANIM_DONE_MSG:
            g_controller.OnRenderAnimDone(static_cast<uint64_t>(w));
            return 0;
        case WM_HOTKEY:
            if (w == OverlayWindow::kEscapeHotkeyId) g_overlay.HandleHotkey();
            return 0;
        case WM_TIMER:
            if (w == kFreezeDiagTimerId) {
                KillTimer(h, w);
                // The controller owns the overlay again the moment a switch starts; only
                // finish the diagnostic if nothing else took over meanwhile.
                if (g_controller.State() == SwitchState::Idle) {
                    g_controller.Renderer().AnimateTo({ 0.0f, 0.0f }, style::kDuration, [] { g_overlay.Hide(); });
                }
            } else {
                g_controller.OnTimerTick(w);
            }
            return 0;
        case WM_SETTINGCHANGE:
            if (l && wcscmp(reinterpret_cast<LPCWSTR>(l), L"ImmersiveColorSet") == 0) {
                g_tray.HandleThemeChange();
            }
            return 0;
        case WM_POWERBROADCAST:
            if (w == PBT_POWERSETTINGCHANGE && l) {
                auto* pbs = reinterpret_cast<POWERBROADCAST_SETTING*>(l);
                if (IsEqualGUID(pbs->PowerSetting, kGuidConsoleDisplayState) && pbs->DataLength >= sizeof(DWORD)) {
                    DWORD state = *reinterpret_cast<DWORD*>(pbs->Data); // 0 off, 1 on, 2 dimmed
                    Log(L"os: console display state -> %lu (%s)", state,
                        state == 0 ? L"off" : state == 1 ? L"on" : state == 2 ? L"dimmed" : L"?");
                }
            } else if (w == PBT_APMSUSPEND || w == PBT_APMRESUMEAUTOMATIC) {
                Log(L"os: power event %s", w == PBT_APMSUSPEND ? L"suspend" : L"resume");
            }
            return TRUE;
        case WM_DISPLAYCHANGE:
            Log(L"os: WM_DISPLAYCHANGE %ux%u @%ubpp", LOWORD(l), HIWORD(l), static_cast<UINT>(w));
            RefreshMonitorTarget();
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(h, m, w, l);
    }

    HWND CreateControllerWindow(HINSTANCE hi) {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = ControllerWndProc;
        wc.hInstance = hi;
        wc.lpszClassName = L"DPSwitcher.Controller";
        RegisterClassExW(&wc);
        // A real top-level window (not HWND_MESSAGE) is required to receive the
        // WM_DISPLAYCHANGE broadcast; it is simply never shown.
        return CreateWindowExW(0, wc.lpszClassName, L"DPSwitcher", WS_OVERLAPPED,
            0, 0, 0, 0, nullptr, nullptr, hi, nullptr);
    }
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    const bool logOk = LogInit();
    Log(L"DPSwitcher starting");

    SingleInstanceGuard instance;
    if (!instance.Acquire(kSingleInstanceMutex)) {
        Log(L"another instance is already running; exiting");
        MessageBoxW(nullptr, L"Monitor Switcher is already running (look for its icon in the notification area).",
            L"Monitor Switcher", MB_OK | MB_ICONINFORMATION);
        return 0;
    }
    if (!logOk) {
        MessageBoxW(nullptr, L"Monitor Switcher could not create its log file. It will keep running, but diagnostics will only be visible in a debugger.",
            L"Monitor Switcher", MB_OK | MB_ICONWARNING);
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken = 0;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, nullptr);

    winrt::init_apartment(winrt::apartment_type::single_threaded);
    EnsureUiDispatcherQueue();

    g_controllerHwnd = CreateControllerWindow(hInstance);
    if (!g_controllerHwnd) { Log(L"Failed to create controller window"); return 1; }
    g_timers.hwnd = g_controllerHwnd;
    RegisterPowerSettingNotification(g_controllerHwnd, &kGuidConsoleDisplayState, DEVICE_NOTIFY_WINDOW_HANDLE);

    if (!g_overlay.Create(hInstance, g_controllerHwnd)) { Log(L"Failed to create overlay window"); return 1; }
    if (!g_tray.Create(g_controllerHwnd)) { Log(L"Failed to create tray icon"); return 1; }

    g_settings = LoadSettings();
    Log(L"settings: monitor=%s revealGraceMs=%u renderer=%s ddcCacheHandles=%d",
        g_settings.monitorDevicePath.empty() ? L"(primary)" : g_settings.monitorDevicePath.c_str(),
        g_settings.revealGraceMs, g_settings.forcedRenderer.empty() ? L"auto" : g_settings.forcedRenderer.c_str(),
        g_settings.ddcCacheHandles);

    g_ddc.SetCacheHandles(g_settings.ddcCacheHandles);
    g_ddc.Start(g_controllerHwnd);

    g_monitorIdentity = IdentityForSetting(g_settings.monitorDevicePath);
    auto targetMonitor = ResolveMonitor(g_monitorIdentity);
    if (!targetMonitor) {
        Log(L"target monitor %s is not connected; waiting for it", g_monitorIdentity.devicePath.c_str());
    }

    HMONITOR renderMonitor = RenderMonitor();
    std::unique_ptr<IOverlayRenderer> renderer;
    if (renderMonitor) renderer = SelectRenderer(g_overlay.Handle(), renderMonitor, g_settings.forcedRenderer);
    else Log(L"No monitors found at startup");
    Log(L"Active renderer: %s", renderer ? renderer->Name() : L"(none)");

    SwitchController::Config cfg{ kDpValue, kUsbcValue, g_settings.revealGraceMs };
    SwitchController::Deps deps{ &g_overlay, &g_tray, &g_ddc, &g_timers, &g_clock };
    g_controller.Initialize(cfg, deps, std::move(renderer), targetMonitor.value_or(nullptr));

    g_overlay.OnClicked = [] { Log(L"overlay clicked"); g_controller.RequestCancel(); };
    g_overlay.OnEscape = [] { Log(L"ESC pressed"); g_controller.RequestCancel(); };

    g_tray.OnLeftClick = [] {
        // Toggle direction is decided by a fresh DDC read (see RequestToggleSwitch), not by
        // trusting any last-known state, which can be stale if DDC went briefly unresponsive.
        g_controller.RequestToggleSwitch();
    };
    g_tray.OnSwitchToUsbc = [] { g_controller.RequestSwitch(kUsbcValue, true); };
    g_tray.OnSwitchToDisplayPort = [] { g_controller.RequestSwitch(kDpValue, false); };
    g_tray.OnExit = [] { DestroyWindow(g_controllerHwnd); };

    g_tray.QueryAutostart = [] { return IsAutostartEnabled(); };
    g_tray.OnSetAutostart = [](bool on) {
        if (!SetAutostartEnabled(on)) g_tray.ShowBalloon(L"Monitor Switcher", L"Could not update the Start with Windows setting.");
        Log(L"settings: autostart -> %d", on);
    };
    g_tray.QueryMonitors = [] { return BuildMonitorEntries(); };
    g_tray.OnSelectMonitor = [](const std::wstring& path) { ApplyMonitorSelection(path); };
    g_tray.QueryRevealGrace = [] { return g_settings.revealGraceMs; };
    g_tray.OnSetRevealGrace = [](UINT ms) {
        g_settings.revealGraceMs = ms;
        SaveRevealGraceMs(ms);
        g_controller.SetRevealGraceMs(ms);
    };

    g_tray.QueryForcedRenderer = [] { return g_settings.forcedRenderer; };
    g_tray.OnForceRenderer = [](const std::wstring& tier) {
        g_settings.forcedRenderer = tier;
        SaveForcedRenderer(tier);
        RebuildRenderer();
    };
    g_tray.OnOpenLog = [] {
        if (*LogPath()) ShellExecuteW(nullptr, L"open", LogPath(), nullptr, nullptr, SW_SHOWNORMAL);
    };
    g_tray.OnFreezeMaxBlur = [] {
        if (g_controller.State() != SwitchState::Idle || !g_controller.Renderer().HasVisuals()) return;
        HMONITOR mon = RenderMonitor();
        if (!mon) return;
        g_overlay.PlaceOn(mon);
        g_overlay.Show();
        g_controller.Renderer().SetVisible(true);
        g_controller.Renderer().AnimateTo({ style::kMaxBlurDip, style::kMaxDim },
            std::chrono::milliseconds{ 0 }, [] {});
        SetTimer(g_controllerHwnd, kFreezeDiagTimerId, 5000, nullptr);
    };

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_ddc.Stop();
    g_controller.ReplaceRenderer(nullptr);
    g_tray.Destroy();
    Gdiplus::GdiplusShutdown(gdiplusToken);
    Log(L"DPSwitcher exiting");
    return static_cast<int>(msg.wParam);
}
