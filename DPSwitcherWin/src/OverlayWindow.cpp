#include "OverlayWindow.h"
#include "AppMessages.h"

namespace {
    OverlayWindow* g_instanceForWndProc = nullptr; // single overlay instance for the process
}

LRESULT CALLBACK OverlayWindow::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_NCHITTEST:     return HTCLIENT;           // whole window is clickable (click-to-reset)
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;      // never steal focus
    case WM_SETCURSOR:     SetCursor(LoadCursorW(nullptr, IDC_ARROW)); return TRUE;
    case WM_ERASEBKGND:    return 1;                  // no GDI; there is no redirection surface anyway
    case WM_PAINT:         ValidateRect(h, nullptr); return 0;
    case WM_LBUTTONUP:
        if (g_instanceForWndProc && g_instanceForWndProc->OnClicked) g_instanceForWndProc->OnClicked();
        return 0;
    case WM_DPICHANGED:    return 0;                  // geometry is always set explicitly in physical px
    case WM_APP_RENDER_ANIM_DONE_MSG:
        // Renderers only know this window's handle (it's what they were initialized with),
        // but the controller lives behind the owner window's WndProc. Forward it there.
        if (g_instanceForWndProc && g_instanceForWndProc->m_owner) {
            return SendMessageW(g_instanceForWndProc->m_owner, m, w, l);
        }
        return 0;
    case WM_TIMER:
        if (w == kReassertTopmostTimerId) {
            // Explorer's taskbar (Shell_TrayWnd) periodically re-asserts its own topmost
            // z-order; a WS_EX_TOOLWINDOW overlay like this one is excluded from the shell's
            // "fullscreen app, suppress the taskbar" heuristic, so without this the taskbar
            // can end up rendered back on top of the blur/black cover. Keep re-topping.
            SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

bool OverlayWindow::Create(HINSTANCE hi, HWND ownerController) {
    m_owner = ownerController;

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hi;
    wc.lpszClassName = L"DPSwitcher.Overlay";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    RegisterClassExW(&wc);

    m_hwnd = CreateWindowExW(
        WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        wc.lpszClassName, L"", WS_POPUP,
        0, 0, 1, 1, nullptr, nullptr, hi, nullptr);
    if (!m_hwnd) return false;

    g_instanceForWndProc = this;

    BOOL on = TRUE;
    DwmSetWindowAttribute(m_hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, &on, sizeof(on)); // no open/close zoom-fade
    DwmSetWindowAttribute(m_hwnd, DWMWA_EXCLUDED_FROM_PEEK, &on, sizeof(on));
    return true;
}

void OverlayWindow::PlaceOn(HMONITOR mon) {
    MONITORINFO mi{ sizeof(mi) };
    if (!mon || !GetMonitorInfoW(mon, &mi)) return;   // physical px (PerMonitorV2)
    const RECT& r = mi.rcMonitor;
    SetWindowPos(m_hwnd, HWND_TOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top,
        SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

void OverlayWindow::Show() {
    ShowWindow(m_hwnd, SW_SHOWNA);
    SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetTimer(m_hwnd, kReassertTopmostTimerId, 250, nullptr);
    RegisterHotKey(m_owner, kEscapeHotkeyId, MOD_NOREPEAT, VK_ESCAPE);
}

void OverlayWindow::Hide() {
    KillTimer(m_hwnd, kReassertTopmostTimerId);
    UnregisterHotKey(m_owner, kEscapeHotkeyId);
    ShowWindow(m_hwnd, SW_HIDE);
}

void OverlayWindow::HandleHotkey() {
    if (OnEscape) OnEscape();
}
