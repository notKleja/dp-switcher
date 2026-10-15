#pragma once
#include "../pch.h"
#include "IOverlayRenderer.h"
#include <atomic>
#include <memory>

// Last-resort tier: a plain black fade, no blur.
//
// Implemented via the same Windows.UI.Composition DesktopWindowTarget plumbing as Tier A,
// minus the Gaussian blur effect graph, so it works on the exact same WS_EX_NOREDIRECTIONBITMAP
// overlay window with no style conflicts. This covers the vast majority of "Tier A failed"
// cases (e.g. the backdrop effect failing on a particular GPU/driver) while still being a
// real animated fade.
//
// True Composition unavailability (pre-1803 Windows, or an RDP session where DWM composition
// itself is disabled) is rare enough in the current target range (Win10 1803+) that it is
// handled by the controller's built-in no-op renderer: the DDC switch still happens, just
// without any covering animation (see SyncNullRenderer).
class BlackFadeRenderer : public IOverlayRenderer {
public:
    ~BlackFadeRenderer() override;

    bool Initialize(HWND overlay, HMONITOR monitor) override;
    void OnMonitorChanged(HMONITOR monitor, RECT physicalRect) override;
    void AnimateTo(OverlayTarget target, std::chrono::milliseconds duration,
        std::function<void()> done) override;
    void Cancel() override;
    OverlayTarget Current() const override { return m_current; }
    const wchar_t* Name() const override { return L"C (plain black fade)"; }

private:
    void SetInstant(OverlayTarget t);

    winrt::Windows::System::DispatcherQueue m_dq{ nullptr };
    winrt::Windows::UI::Composition::Compositor m_compositor{ nullptr };
    winrt::Windows::UI::Composition::Desktop::DesktopWindowTarget m_target{ nullptr };
    winrt::Windows::UI::Composition::SpriteVisual m_dimVisual{ nullptr };
    winrt::Windows::UI::Composition::CompositionEasingFunction m_easing{ nullptr };

    OverlayTarget m_current{};
    std::atomic<uint64_t> m_generation{ 0 };
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};
