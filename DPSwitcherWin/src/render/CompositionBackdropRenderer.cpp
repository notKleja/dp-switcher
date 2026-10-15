#include "CompositionBackdropRenderer.h"
#include "GaussianBlurEffect.h"
#include "TransitionStyle.h"
#include "../util/Log.h"
#include "../util/Dispatcher.h"

using namespace winrt;
using namespace winrt::Windows::UI;
using namespace winrt::Windows::UI::Composition;
using namespace winrt::Windows::UI::Composition::Desktop;
namespace abi_desk = ABI::Windows::UI::Composition::Desktop;

static constexpr wchar_t kBlurProp[] = L"Blur.BlurAmount";

CompositionBackdropRenderer::~CompositionBackdropRenderer() {
    *m_alive = false;
    Reset();
}

bool CompositionBackdropRenderer::Initialize(HWND overlay, HMONITOR) try {
    // 1) The UI thread's DispatcherQueue (created once per process by main).
    m_dq = UiDispatcherQueue();
    if (!m_dq) { Log(L"Tier A: no DispatcherQueue on this thread"); return false; }

    // 2) Compositor + desktop target (topmost layer over the HWND).
    m_compositor = Compositor();
    auto interop = m_compositor.as<abi_desk::ICompositorDesktopInterop>();
    check_hresult(interop->CreateDesktopWindowTarget(overlay, TRUE,
        reinterpret_cast<abi_desk::IDesktopWindowTarget**>(put_abi(m_target))));

    m_root = m_compositor.CreateContainerVisual();
    m_root.RelativeSizeAdjustment({ 1.0f, 1.0f });
    m_target.Root(m_root);

    // 3) Effect graph: BackdropBrush -> GaussianBlur (BlurAmount animatable).
    auto fx = make_self<GaussianBlurEffect>();
    fx->Name(L"Blur");
    fx->BlurAmount = 0.0f;
    fx->Source = CompositionEffectSourceParameter(L"backdrop");

    auto factory = m_compositor.CreateEffectFactory(
        fx.as<winrt::Windows::Graphics::Effects::IGraphicsEffect>(), { hstring(kBlurProp) });
    m_blurBrush = factory.CreateBrush();
    m_blurBrush.SetSourceParameter(L"backdrop", m_compositor.CreateBackdropBrush());

    m_blurVisual = m_compositor.CreateSpriteVisual();
    m_blurVisual.RelativeSizeAdjustment({ 1.0f, 1.0f });
    m_blurVisual.Brush(m_blurBrush);

    // 4) Black dim layer on top.
    m_dimVisual = m_compositor.CreateSpriteVisual();
    m_dimVisual.RelativeSizeAdjustment({ 1.0f, 1.0f });
    m_dimVisual.Brush(m_compositor.CreateColorBrush(ColorHelper::FromArgb(255, 0, 0, 0)));
    m_dimVisual.Opacity(0.0f);

    m_root.Children().InsertAtTop(m_blurVisual);
    m_root.Children().InsertAtTop(m_dimVisual);

    // 5) Same curve as macOS CAMediaTimingFunction(0.93, 0.02, 0.74, 0.98).
    m_easing = m_compositor.CreateCubicBezierEasingFunction(
        { style::kBezierX1, style::kBezierY1 }, { style::kBezierX2, style::kBezierY2 });

    SetInstant({ 0.0f, 0.0f });
    return true;
} catch (hresult_error const& e) {
    Log(L"Tier A init failed: 0x%08X %s", e.code().value, e.message().c_str());
    Reset();
    return false;
}

void CompositionBackdropRenderer::Reset() {
    m_target = nullptr;
    m_root = nullptr;
    m_blurBrush = nullptr;
    m_blurVisual = nullptr;
    m_dimVisual = nullptr;
    m_easing = nullptr;
    m_compositor = nullptr;
}

void CompositionBackdropRenderer::OnMonitorChanged(HMONITOR, RECT) {
    // The desktop window target tracks the HWND's own bounds, which OverlayWindow repositions
    // on WM_DISPLAYCHANGE; nothing renderer-side needs to change here.
}

void CompositionBackdropRenderer::SetInstant(OverlayTarget t) {
    m_blurBrush.StopAnimation(kBlurProp);
    m_dimVisual.StopAnimation(L"Opacity");
    m_blurBrush.Properties().InsertScalar(kBlurProp, t.blurDip);
    m_dimVisual.Opacity(t.dim);
    m_current = t;
}

void CompositionBackdropRenderer::AnimateTo(OverlayTarget t, std::chrono::milliseconds dur,
    std::function<void()> done) {
    const uint64_t gen = ++m_generation;
    Log(L"render: animate blur %.1f->%.1f dim %.2f->%.2f over %lldms (gen %llu)",
        m_current.blurDip, t.blurDip, m_current.dim, t.dim, static_cast<long long>(dur.count()), gen);
    auto finish = [alive = m_alive, this, gen, done = std::move(done)] {
        if (!*alive) return; // renderer was replaced; the controller has moved on
        const bool current = (gen == m_generation.load());
        Log(L"render: animation gen %llu completed (%s)", gen, current ? L"current" : L"superseded");
        if (current && done) done();
    };

    if (dur.count() <= 0) {
        SetInstant(t);
        // Complete after the commit is handed to DWM.
        m_dq.TryEnqueue(winrt::Windows::System::DispatcherQueuePriority::Low, finish);
        return;
    }

    // Key frame at 1.0 only => animation starts from the property's CURRENT value,
    // so reversing mid-flight is seamless (no jump).
    auto blurAnim = m_compositor.CreateScalarKeyFrameAnimation();
    blurAnim.InsertKeyFrame(1.0f, t.blurDip, m_easing);
    blurAnim.Duration(dur);

    auto dimAnim = m_compositor.CreateScalarKeyFrameAnimation();
    dimAnim.InsertKeyFrame(1.0f, t.dim, m_easing);
    dimAnim.Duration(dur);

    auto batch = m_compositor.CreateScopedBatch(CompositionBatchTypes::Animation);
    m_blurBrush.StartAnimation(kBlurProp, blurAnim);
    m_dimVisual.StartAnimation(L"Opacity", dimAnim);
    batch.End();
    batch.Completed([finish](auto&&, auto&&) { finish(); });   // raised on the UI thread

    m_current = t;   // logical target; the presented value is owned by DWM
}

void CompositionBackdropRenderer::Cancel() { ++m_generation; }
