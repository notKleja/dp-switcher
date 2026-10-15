#include "BlackFadeRenderer.h"
#include "TransitionStyle.h"
#include "../util/Log.h"
#include "../util/Dispatcher.h"

using namespace winrt;
using namespace winrt::Windows::UI;
using namespace winrt::Windows::UI::Composition;
using namespace winrt::Windows::UI::Composition::Desktop;
namespace abi_desk = ABI::Windows::UI::Composition::Desktop;

BlackFadeRenderer::~BlackFadeRenderer() {
    *m_alive = false;
    m_target = nullptr;
    m_dimVisual = nullptr;
    m_easing = nullptr;
    m_compositor = nullptr;
}

bool BlackFadeRenderer::Initialize(HWND overlay, HMONITOR) try {
    m_dq = UiDispatcherQueue();
    if (!m_dq) { Log(L"Tier C: no DispatcherQueue on this thread"); return false; }

    m_compositor = Compositor();
    auto interop = m_compositor.as<abi_desk::ICompositorDesktopInterop>();
    check_hresult(interop->CreateDesktopWindowTarget(overlay, TRUE,
        reinterpret_cast<abi_desk::IDesktopWindowTarget**>(put_abi(m_target))));

    m_dimVisual = m_compositor.CreateSpriteVisual();
    m_dimVisual.RelativeSizeAdjustment({ 1.0f, 1.0f });
    m_dimVisual.Brush(m_compositor.CreateColorBrush(ColorHelper::FromArgb(255, 0, 0, 0)));
    m_dimVisual.Opacity(0.0f);
    m_target.Root(m_dimVisual);

    m_easing = m_compositor.CreateCubicBezierEasingFunction(
        { style::kBezierX1, style::kBezierY1 }, { style::kBezierX2, style::kBezierY2 });

    SetInstant({ 0.0f, 0.0f });
    return true;
} catch (hresult_error const& e) {
    Log(L"Tier C init failed: 0x%08X %s", e.code().value, e.message().c_str());
    m_target = nullptr;
    m_dimVisual = nullptr;
    m_easing = nullptr;
    m_compositor = nullptr;
    return false;
}

void BlackFadeRenderer::OnMonitorChanged(HMONITOR, RECT) {}

void BlackFadeRenderer::SetInstant(OverlayTarget t) {
    m_dimVisual.StopAnimation(L"Opacity");
    m_dimVisual.Opacity(t.dim);
    m_current = t;
}

void BlackFadeRenderer::AnimateTo(OverlayTarget t, std::chrono::milliseconds dur,
    std::function<void()> done) {
    const uint64_t gen = ++m_generation;
    auto finish = [alive = m_alive, this, gen, done = std::move(done)] {
        if (!*alive) return;
        if (gen == m_generation.load() && done) done();
    };

    if (dur.count() <= 0) {
        SetInstant(t);
        m_dq.TryEnqueue(winrt::Windows::System::DispatcherQueuePriority::Low, finish);
        return;
    }

    auto dimAnim = m_compositor.CreateScalarKeyFrameAnimation();
    dimAnim.InsertKeyFrame(1.0f, t.dim, m_easing);
    dimAnim.Duration(dur);

    auto batch = m_compositor.CreateScopedBatch(CompositionBatchTypes::Animation);
    m_dimVisual.StartAnimation(L"Opacity", dimAnim);
    batch.End();
    batch.Completed([finish](auto&&, auto&&) { finish(); });

    m_current = t;
}

void BlackFadeRenderer::Cancel() { ++m_generation; }
