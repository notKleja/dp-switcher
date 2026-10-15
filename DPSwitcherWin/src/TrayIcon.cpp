#include "TrayIcon.h"
#include "AppMessages.h"
#include <gdiplus.h>

namespace {
    constexpr UINT ID_SWITCH_USBC = 1001;
    constexpr UINT ID_SWITCH_DP = 1002;
    constexpr UINT ID_EXIT = 1004;
    constexpr UINT ID_DIAG_FORCE_A = 1011;
    constexpr UINT ID_DIAG_FORCE_B = 1012;
    constexpr UINT ID_DIAG_FORCE_C = 1013;
    constexpr UINT ID_DIAG_FREEZE = 1014;
    constexpr UINT ID_DIAG_OPENLOG = 1015;
    constexpr UINT ID_DIAG_FORCE_AUTO = 1016;
    constexpr UINT ID_SET_AUTOSTART = 1020;
    constexpr UINT ID_MONITOR_PRIMARY = 1030;
    constexpr UINT ID_MONITOR_FIRST = 1031;
    constexpr UINT ID_MONITOR_LAST = 1062;
    constexpr UINT ID_GRACE_FIRST = 1070;
    constexpr UINT kGracePresetsMs[] = { 0, 100, 250, 500, 1000 };
    constexpr UINT ID_GRACE_LAST = ID_GRACE_FIRST + ARRAYSIZE(kGracePresetsMs) - 1;

    // Segoe Fluent Icons (Win11) / Segoe MDL2 Assets (Win10, same codepoint) glyph name
    // "TVMonitor" -- the direct Windows-icon-font equivalent of the mac app's SF Symbol
    // NSImage(systemSymbolName: "tv").
    constexpr wchar_t kGlyphTvMonitor = L'';

    UINT Checked(bool on) { return on ? MF_CHECKED : MF_UNCHECKED; }
}

bool TrayIcon::IsLightTaskbarTheme() {
    DWORD value = 1, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS) {
        return true; // Windows' own default when the value is absent
    }
    return value != 0;
}

HICON TrayIcon::MakeSegoeFluentIcon(wchar_t glyph, bool forLightTaskbar) {
    using namespace Gdiplus;

    int size = GetSystemMetrics(SM_CXSMICON);
    if (size <= 0) size = 16;

    Bitmap bitmap(size, size, PixelFormat32bppPARGB);
    Graphics g(&bitmap);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    g.Clear(Color(0, 0, 0, 0));

    FontFamily fluentFamily(L"Segoe Fluent Icons");
    FontFamily mdl2Family(L"Segoe MDL2 Assets");
    FontFamily* family = (fluentFamily.GetLastStatus() == Ok) ? &fluentFamily : &mdl2Family;

    // Icon-font glyphs are drawn near the full em box; a slightly-larger-than-icon point
    // size with center alignment reliably fills a small tray icon without clipping.
    Font font(family, static_cast<REAL>(size) * 0.82f, FontStyleRegular, UnitPixel);

    Color glyphColor = forLightTaskbar ? Color(255, 0, 0, 0) : Color(255, 255, 255, 255);
    SolidBrush brush(glyphColor);

    StringFormat format;
    format.SetAlignment(StringAlignmentCenter);
    format.SetLineAlignment(StringAlignmentCenter);

    wchar_t text[2] = { glyph, L'\0' };
    RectF layoutRect(0, 0, static_cast<REAL>(size), static_cast<REAL>(size));
    g.DrawString(text, 1, &font, layoutRect, &format, &brush);

    HICON icon = nullptr;
    bitmap.GetHICON(&icon);
    return icon;
}

void TrayIcon::ApplyIconForCurrentTheme() {
    bool isLight = IsLightTaskbarTheme();
    HICON newIcon = MakeSegoeFluentIcon(kGlyphTvMonitor, isLight);
    if (!newIcon) return;

    HICON old = m_icon;
    m_icon = newIcon;
    m_lastIsLightTheme = isLight;
    m_nid.hIcon = m_icon;

    if (m_created) {
        m_nid.uFlags = NIF_ICON;
        Shell_NotifyIconW(NIM_MODIFY, &m_nid);
    }
    if (old) DestroyIcon(old);
}

bool TrayIcon::Create(HWND owner) {
    m_owner = owner;
    m_lastIsLightTheme = IsLightTaskbarTheme();
    m_icon = MakeSegoeFluentIcon(kGlyphTvMonitor, m_lastIsLightTheme);

    m_nid.cbSize = sizeof(m_nid);
    m_nid.hWnd = owner;
    m_nid.uID = 1;
    m_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    m_nid.uCallbackMessage = WM_APP_TRAYICON;
    m_nid.hIcon = m_icon;
    wcscpy_s(m_nid.szTip, L"Monitor Switcher");

    m_created = Shell_NotifyIconW(NIM_ADD, &m_nid) != FALSE;
    if (m_created) {
        m_nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &m_nid);
    }
    return m_created;
}

void TrayIcon::Destroy() {
    if (m_created) Shell_NotifyIconW(NIM_DELETE, &m_nid);
    if (m_icon) DestroyIcon(m_icon);
    m_created = false;
    m_icon = nullptr;
}

void TrayIcon::HandleThemeChange() {
    bool isLight = IsLightTaskbarTheme();
    if (isLight != m_lastIsLightTheme) {
        ApplyIconForCurrentTheme();
    }
}

HMENU TrayIcon::BuildMenu() {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, ID_SWITCH_USBC, L"Switch to USB-C (Mac)");
    AppendMenuW(menu, MF_STRING, ID_SWITCH_DP, L"Switch to DisplayPort (PC)");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    // ---- Settings ----
    HMENU settings = CreatePopupMenu();
    const bool autostart = QueryAutostart ? QueryAutostart() : false;
    AppendMenuW(settings, MF_STRING | Checked(autostart), ID_SET_AUTOSTART, L"Start with Windows");
    AppendMenuW(settings, MF_SEPARATOR, 0, nullptr);

    HMENU monitors = CreatePopupMenu();
    m_menuMonitorPaths.clear();
    std::vector<TrayMonitorEntry> entries = QueryMonitors ? QueryMonitors() : std::vector<TrayMonitorEntry>{};
    bool anySelected = false;
    for (const auto& e : entries) anySelected = anySelected || e.selected;
    AppendMenuW(monitors, MF_STRING | Checked(!anySelected), ID_MONITOR_PRIMARY, L"Primary monitor (automatic)");
    for (const auto& e : entries) {
        if (m_menuMonitorPaths.size() > ID_MONITOR_LAST - ID_MONITOR_FIRST) break;
        const UINT id = ID_MONITOR_FIRST + static_cast<UINT>(m_menuMonitorPaths.size());
        m_menuMonitorPaths.push_back(e.devicePath);
        AppendMenuW(monitors, MF_STRING | Checked(e.selected), id, e.label.c_str());
    }
    AppendMenuW(settings, MF_POPUP, reinterpret_cast<UINT_PTR>(monitors), L"Target monitor");

    HMENU grace = CreatePopupMenu();
    const UINT currentGrace = QueryRevealGrace ? QueryRevealGrace() : 0;
    bool graceMatched = false;
    for (UINT i = 0; i < ARRAYSIZE(kGracePresetsMs); ++i) {
        wchar_t label[32];
        swprintf_s(label, kGracePresetsMs[i] == 0 ? L"Off" : L"%u ms", kGracePresetsMs[i]);
        const bool on = (currentGrace == kGracePresetsMs[i]);
        graceMatched = graceMatched || on;
        AppendMenuW(grace, MF_STRING | Checked(on), ID_GRACE_FIRST + i, label);
    }
    if (!graceMatched) {
        wchar_t label[48];
        swprintf_s(label, L"%u ms (custom, from registry)", currentGrace);
        AppendMenuW(grace, MF_STRING | MF_CHECKED | MF_GRAYED, 0, label);
    }
    AppendMenuW(settings, MF_POPUP, reinterpret_cast<UINT_PTR>(grace), L"Reveal grace");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(settings), L"Settings");

    // ---- Diagnostics ----
    HMENU diag = CreatePopupMenu();
    HMENU renderer = CreatePopupMenu();
    const std::wstring forced = QueryForcedRenderer ? QueryForcedRenderer() : L"";
    AppendMenuW(renderer, MF_STRING | Checked(forced.empty()), ID_DIAG_FORCE_AUTO, L"Automatic (A, then B, then C)");
    AppendMenuW(renderer, MF_STRING | Checked(forced == L"A"), ID_DIAG_FORCE_A, L"Force A (Composition backdrop)");
    AppendMenuW(renderer, MF_STRING | Checked(forced == L"B"), ID_DIAG_FORCE_B, L"Force B (Desktop Duplication)");
    AppendMenuW(renderer, MF_STRING | Checked(forced == L"C"), ID_DIAG_FORCE_C, L"Force C (plain black fade)");
    AppendMenuW(diag, MF_POPUP, reinterpret_cast<UINT_PTR>(renderer), L"Renderer");
    AppendMenuW(diag, MF_STRING, ID_DIAG_FREEZE, L"Freeze at max blur (5s)");
    AppendMenuW(diag, MF_STRING, ID_DIAG_OPENLOG, L"Open log");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(diag), L"Diagnostics");

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_EXIT, L"Exit");
    return menu;
}

void TrayIcon::ShowBalloon(const wchar_t* title, const wchar_t* text) {
    NOTIFYICONDATAW nid = m_nid;
    nid.uFlags = NIF_INFO;
    wcscpy_s(nid.szInfoTitle, title);
    wcscpy_s(nid.szInfo, text);
    nid.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void TrayIcon::HandleCallback(WPARAM, LPARAM lParam) {
    UINT msg = LOWORD(lParam);
    if (msg == WM_LBUTTONUP) {
        if (OnLeftClick) OnLeftClick();
    } else if (msg == WM_RBUTTONUP || msg == WM_CONTEXTMENU) {
        POINT pt;
        GetCursorPos(&pt);
        HMENU menu = BuildMenu();
        SetForegroundWindow(m_owner); // required so the menu dismisses on outside click
        TrackPopupMenuEx(menu, TPM_RIGHTBUTTON, pt.x, pt.y, m_owner, nullptr);
        PostMessageW(m_owner, WM_NULL, 0, 0);
        DestroyMenu(menu); // destroys the submenus too; the WM_COMMAND has already been queued
    }
}

void TrayIcon::HandleCommand(WPARAM wParam) {
    const UINT id = LOWORD(wParam);
    switch (id) {
    case ID_SWITCH_USBC: if (OnSwitchToUsbc) OnSwitchToUsbc(); return;
    case ID_SWITCH_DP: if (OnSwitchToDisplayPort) OnSwitchToDisplayPort(); return;
    case ID_EXIT: if (OnExit) OnExit(); return;
    case ID_SET_AUTOSTART: if (OnSetAutostart) OnSetAutostart(!(QueryAutostart && QueryAutostart())); return;
    case ID_MONITOR_PRIMARY: if (OnSelectMonitor) OnSelectMonitor(L""); return;
    case ID_DIAG_FORCE_AUTO: if (OnForceRenderer) OnForceRenderer(L""); return;
    case ID_DIAG_FORCE_A: if (OnForceRenderer) OnForceRenderer(L"A"); return;
    case ID_DIAG_FORCE_B: if (OnForceRenderer) OnForceRenderer(L"B"); return;
    case ID_DIAG_FORCE_C: if (OnForceRenderer) OnForceRenderer(L"C"); return;
    case ID_DIAG_FREEZE: if (OnFreezeMaxBlur) OnFreezeMaxBlur(); return;
    case ID_DIAG_OPENLOG: if (OnOpenLog) OnOpenLog(); return;
    default: break;
    }
    if (id >= ID_MONITOR_FIRST && id <= ID_MONITOR_LAST) {
        const size_t index = id - ID_MONITOR_FIRST;
        if (index < m_menuMonitorPaths.size() && OnSelectMonitor) OnSelectMonitor(m_menuMonitorPaths[index]);
        return;
    }
    if (id >= ID_GRACE_FIRST && id <= ID_GRACE_LAST) {
        if (OnSetRevealGrace) OnSetRevealGrace(kGracePresetsMs[id - ID_GRACE_FIRST]);
        return;
    }
}
