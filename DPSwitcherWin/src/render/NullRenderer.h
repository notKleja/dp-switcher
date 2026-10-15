#pragma once
#include "IOverlayRenderer.h"

// Used when every real tier failed to initialize (or none was supplied): the DDC switch
// still happens, just without any covering animation. Completion is synchronous, which the
// controller tolerates because nothing follows an AnimateTo call inside its Enter* methods.
class SyncNullRenderer final : public IOverlayRenderer {
public:
    bool Initialize(HWND, HMONITOR) override { return true; }
    void OnMonitorChanged(HMONITOR, RECT) override {}
    void AnimateTo(OverlayTarget target, std::chrono::milliseconds, std::function<void()> done) override {
        m_current = target;
        if (done) done();
    }
    void Cancel() override {}
    OverlayTarget Current() const override { return m_current; }
    const wchar_t* Name() const override { return L"none (no overlay; DDC switch only)"; }
    bool HasVisuals() const override { return false; }

private:
    OverlayTarget m_current{};
};
