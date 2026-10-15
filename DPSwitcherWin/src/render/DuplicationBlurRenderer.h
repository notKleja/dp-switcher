#pragma once
#include "../pch.h"
#include "IOverlayRenderer.h"
#include "CubicBezier.h"
#include <atomic>
#include <thread>
#include <mutex>

// Lock-free-ish (mutex-guarded, contention is negligible at 60fps) animation state shared
// between the UI thread (writer, via AnimateTo/Cancel) and the render thread (reader, via
// Sample). Sampling at time `now` reads the *actual* current value if `now` is mid-flight,
// which is what makes AnimateTo interruption seamless (no snap-back).
class AnimSampler {
public:
    struct Sample { float blur; float dim; bool finished; uint64_t generation; };

    void Start(OverlayTarget from, OverlayTarget to, std::chrono::milliseconds duration,
        std::chrono::steady_clock::time_point now, uint64_t generation) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_from = from; m_to = to; m_start = now; m_duration = duration;
        m_generation = generation; m_finishedReported = false;
    }

    void SetInstant(OverlayTarget t, uint64_t generation) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_from = m_to = t; m_duration = std::chrono::milliseconds{ 0 };
        m_generation = generation; m_finishedReported = true; // nothing to animate
    }

    // The value on screen right now (UI thread, when starting a new animation from it).
    OverlayTarget CurrentValue(std::chrono::steady_clock::time_point now) {
        std::lock_guard<std::mutex> lock(m_mutex);
        bool done = false;
        return Interpolate(now, done);
    }

    Sample Sample_(std::chrono::steady_clock::time_point now) {
        std::lock_guard<std::mutex> lock(m_mutex);
        bool done = false;
        OverlayTarget v = Interpolate(now, done);
        bool report = done && !m_finishedReported;
        if (done) m_finishedReported = true;
        return { v.blurDip, v.dim, report, m_generation };
    }

private:
    OverlayTarget Interpolate(std::chrono::steady_clock::time_point now, bool& done) const {
        if (m_duration.count() <= 0) { done = false; return m_to; }
        double t = std::chrono::duration<double, std::milli>(now - m_start).count() / m_duration.count();
        done = t >= 1.0;
        t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
        double eased = m_ease(t);
        return { static_cast<float>(m_from.blurDip + (m_to.blurDip - m_from.blurDip) * eased),
                 static_cast<float>(m_from.dim + (m_to.dim - m_from.dim) * eased) };
    }

    std::mutex m_mutex;
    OverlayTarget m_from{}, m_to{};
    std::chrono::steady_clock::time_point m_start{};
    std::chrono::milliseconds m_duration{};
    uint64_t m_generation = 0;
    bool m_finishedReported = true;
    CubicBezier m_ease{ 0.93, 0.02, 0.74, 0.98 };
};

// Tier B: Desktop Duplication capture of the target output, blurred with Direct2D, presented
// through a DirectComposition swap chain on a render thread. All D3D/D2D/DComp objects are
// owned by that thread after Initialize(); the UI thread only posts geometry changes and
// animation targets to it.
class DuplicationBlurRenderer : public IOverlayRenderer {
public:
    ~DuplicationBlurRenderer() override;

    bool Initialize(HWND overlay, HMONITOR monitor) override;
    void OnMonitorChanged(HMONITOR monitor, RECT physicalRect) override;
    void AnimateTo(OverlayTarget target, std::chrono::milliseconds duration,
        std::function<void()> done) override;
    void Cancel() override;
    OverlayTarget Current() const override { return m_current; }
    const wchar_t* Name() const override { return L"B (Desktop Duplication + Direct2D)"; }
    void OnAnimDoneMessage(uint64_t generation) override;
    void SetVisible(bool visible) override;

private:
    struct Geometry { UINT width = 0, height = 0; float dpiScale = 1.0f; };

    bool CreateDevices(HMONITOR mon);
    bool CreateDuplication();
    bool CreateSwapChain();
    bool RecreateAll();       // render thread only
    void PollDuplication();   // render thread only
    void ThreadMain();
    void Wake();

    HWND m_hwnd = nullptr;
    bool m_affinitySet = false;

    // Geometry the render thread is using vs. the latest one the UI thread has seen.
    Geometry m_geom;                    // render thread only (and Initialize, before the thread starts)
    std::mutex m_pendingGeomMutex;
    Geometry m_pendingGeom;
    std::atomic<bool> m_recreateRequested{ false };
    bool m_deviceReady = false;         // render thread only

    DXGI_MODE_ROTATION m_rotation = DXGI_MODE_ROTATION_IDENTITY;

    com_ptr<IDXGIAdapter1> m_adapter;
    com_ptr<IDXGIOutput> m_output;
    com_ptr<ID3D11Device> m_d3d;
    com_ptr<ID3D11DeviceContext> m_ctx;
    com_ptr<ID2D1Device6> m_d2dDevice;
    com_ptr<ID2D1DeviceContext6> m_d2d;
    com_ptr<ID2D1Effect> m_blur;
    com_ptr<ID2D1SolidColorBrush> m_black;

    com_ptr<IDXGIOutputDuplication> m_dupl;
    com_ptr<ID3D11Texture2D> m_frameTex;
    com_ptr<ID2D1Bitmap1> m_frameBmp;

    com_ptr<IDXGISwapChain2> m_swap;
    winrt::handle m_waitable;
    com_ptr<IDCompositionDevice> m_dcomp;
    com_ptr<IDCompositionTarget> m_dtarget;
    com_ptr<IDCompositionVisual> m_dvisual;
    com_ptr<ID2D1Bitmap1> m_target;

    AnimSampler m_anim;
    OverlayTarget m_current{};
    std::atomic<uint64_t> m_generation{ 0 };
    std::function<void()> m_pendingDone;
    std::mutex m_doneMutex;

    std::thread m_thread;
    std::atomic<bool> m_quit{ false };
    std::atomic<bool> m_visible{ false };
    HANDLE m_wakeEvent = nullptr;
    HWND m_notifyHwnd = nullptr;
};
