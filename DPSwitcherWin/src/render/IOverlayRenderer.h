#pragma once
#include <windows.h>
#include <chrono>
#include <functional>

struct OverlayTarget {
    float blurDip = 0.0f;   // Gaussian sigma in DIPs (0 = sharp)
    float dim = 0.0f;       // black layer opacity 0..1
};

class IOverlayRenderer {
public:
    virtual ~IOverlayRenderer() = default;
    virtual bool Initialize(HWND overlay, HMONITOR monitor) = 0;   // false => try next tier
    virtual void OnMonitorChanged(HMONITOR monitor, RECT physicalRect) = 0;
    // Animate from the *current presented value* to target. durationMs == 0 => apply instantly.
    // `done` runs on the UI thread exactly once, unless superseded by a later AnimateTo/Cancel.
    virtual void AnimateTo(OverlayTarget target, std::chrono::milliseconds duration,
        std::function<void()> done) = 0;
    virtual void Cancel() = 0;           // drop pending `done`, keep current visuals
    virtual OverlayTarget Current() const = 0;
    virtual const wchar_t* Name() const = 0;

    // Renderers whose AnimateTo completion is signaled asynchronously from another thread
    // (Tier B's render thread) post WM_APP_RENDER_ANIM_DONE_MSG with the generation as
    // wParam; the owner's WndProc forwards it here so the renderer can invoke its stored
    // `done` callback on the UI thread. Renderers that complete synchronously (Tier A/C,
    // via ScopedBatch.Completed) don't need this and can leave the default no-op.
    virtual void OnAnimDoneMessage(uint64_t /*generation*/) {}

    // Tier B's render thread idles (blocked on its wake event) while the overlay is hidden,
    // to avoid burning a core capturing/blurring frames nobody sees. Tier A/C do no per-frame
    // work regardless, so this is a no-op for them.
    virtual void SetVisible(bool /*visible*/) {}

    // False for the no-op fallback renderer: the controller then never shows the overlay
    // window (nothing would be drawn into it, and it would only swallow clicks and ESC).
    virtual bool HasVisuals() const { return true; }
};
