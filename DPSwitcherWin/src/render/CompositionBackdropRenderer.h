#pragma once
#include "../pch.h"
#include "IOverlayRenderer.h"
#include <atomic>
#include <memory>

class CompositionBackdropRenderer : public IOverlayRenderer {
public:
    ~CompositionBackdropRenderer() override;

    bool Initialize(HWND overlay, HMONITOR monitor) override;
    void OnMonitorChanged(HMONITOR monitor, RECT physicalRect) override;
    void AnimateTo(OverlayTarget target, std::chrono::milliseconds duration,
        std::function<void()> done) override;
    void Cancel() override;
    OverlayTarget Current() const override { return m_current; }
    const wchar_t* Name() const override { return L"A (Composition backdrop)"; }

private:
    void SetInstant(OverlayTarget t);
    void Reset();

    winrt::Windows::System::DispatcherQueue m_dq{ nullptr };
    winrt::Windows::UI::Composition::Compositor m_compositor{ nullptr };
    winrt::Windows::UI::Composition::Desktop::DesktopWindowTarget m_target{ nullptr };
    winrt::Windows::UI::Composition::ContainerVisual m_root{ nullptr };
    winrt::Windows::UI::Composition::CompositionEffectBrush m_blurBrush{ nullptr };
    winrt::Windows::UI::Composition::SpriteVisual m_blurVisual{ nullptr };
    winrt::Windows::UI::Composition::SpriteVisual m_dimVisual{ nullptr };
    winrt::Windows::UI::Composition::CompositionEasingFunction m_easing{ nullptr };

    OverlayTarget m_current{};
    std::atomic<uint64_t> m_generation{ 0 };
    // Batch-completed and dispatcher callbacks capture `this`; they may still fire after
    // this renderer was replaced and destroyed. The token lets them bail out safely.
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};
