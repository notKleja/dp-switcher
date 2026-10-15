#pragma once
#include "pch.h"
#include "host/Interfaces.h"
#include <functional>

// Borderless, topmost, click-through-disabled popup window used as the renderer target.
// Created once and kept alive for the process lifetime; only shown/hidden and repositioned.
class OverlayWindow : public IOverlayHost {
public:
    static constexpr int kEscapeHotkeyId = 0xDF02;
    static constexpr UINT_PTR kReassertTopmostTimerId = 0xDF03;

    bool Create(HINSTANCE hi, HWND ownerController);
    HWND Handle() const { return m_hwnd; }

    // IOverlayHost
    void PlaceOn(HMONITOR mon) override;
    void Show() override;
    void Hide() override;

    // main.cpp's WndProc forwards WM_HOTKEY (id == kEscapeHotkeyId) here, since
    // RegisterHotKey targets the owner/controller window, not the overlay itself
    // (the overlay never takes focus, so it can't own a hotkey registration).
    void HandleHotkey();

    std::function<void()> OnClicked;
    std::function<void()> OnEscape;

private:
    static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l);

    HWND m_hwnd = nullptr;
    HWND m_owner = nullptr;
};
