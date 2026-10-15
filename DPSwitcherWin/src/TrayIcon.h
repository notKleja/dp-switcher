#pragma once
#include "pch.h"
#include "host/Interfaces.h"
#include <functional>
#include <string>
#include <vector>

struct TrayMonitorEntry {
    std::wstring label;
    std::wstring devicePath;
    bool selected = false;
};

// Shell_NotifyIconW-based tray icon + context menu. Owns no window of its own; messages
// arrive at the owner HWND's WndProc and are forwarded here via HandleCallback/HandleCommand.
//
// Deliberately static: one fixed icon and tooltip for the process lifetime. It does not
// reflect current input state (the user doesn't want it to), so it never needs anything
// from the controller just to keep the tray display up to date.
//
// The icon itself is a real Segoe Fluent Icons glyph (Windows' icon-font system, the actual
// equivalent of macOS's SF Symbols -- same idea the original mac app used via
// NSImage(systemSymbolName: "tv")), rendered through GDI+ for proper per-pixel alpha
// antialiasing rather than raw GDI text. It's monochrome and tracks the taskbar's light/dark
// theme live, mirroring the mac version's .preferringMonochrome() template-image behavior.
//
// The context menu is rebuilt on every right-click so the Settings submenu always reflects
// the current monitor list and persisted values (queried through the Query* callbacks).
class TrayIcon : public INotifier {
public:
    bool Create(HWND owner);
    void Destroy();

    // INotifier
    void ShowBalloon(const wchar_t* title, const wchar_t* text) override;

    // Forward from WndProc for WM_APP_TRAYICON (icon callback), WM_COMMAND (menu clicks),
    // and WM_SETTINGCHANGE (taskbar theme changed -> re-render the icon's color).
    void HandleCallback(WPARAM wParam, LPARAM lParam);
    void HandleCommand(WPARAM wParam);
    void HandleThemeChange();

    std::function<void()> OnLeftClick;
    std::function<void()> OnSwitchToUsbc;
    std::function<void()> OnSwitchToDisplayPort;
    std::function<void()> OnExit;

    // Settings submenu.
    std::function<bool()> QueryAutostart;
    std::function<void(bool)> OnSetAutostart;
    std::function<std::vector<TrayMonitorEntry>()> QueryMonitors;
    std::function<void(const std::wstring&)> OnSelectMonitor;   // "" = primary
    std::function<UINT()> QueryRevealGrace;
    std::function<void(UINT)> OnSetRevealGrace;

    // Diagnostics submenu.
    std::function<std::wstring()> QueryForcedRenderer;          // "A" | "B" | "C" | ""
    std::function<void(const std::wstring&)> OnForceRenderer;
    std::function<void()> OnFreezeMaxBlur;
    std::function<void()> OnOpenLog;

private:
    HICON MakeSegoeFluentIcon(wchar_t glyph, bool forLightTaskbar);
    HMENU BuildMenu();
    void ApplyIconForCurrentTheme();
    static bool IsLightTaskbarTheme();

    HWND m_owner = nullptr;
    NOTIFYICONDATAW m_nid{};
    HICON m_icon = nullptr;
    bool m_created = false;
    bool m_lastIsLightTheme = false;
    std::vector<std::wstring> m_menuMonitorPaths; // index -> device path for the monitor radio items
};
