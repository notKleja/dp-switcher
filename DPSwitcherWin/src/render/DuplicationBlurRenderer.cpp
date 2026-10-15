#include "DuplicationBlurRenderer.h"
#include "../util/Log.h"
#include "../AppMessages.h"
#include <dxgi1_2.h>
#include <avrt.h>

#pragma comment(lib, "avrt.lib")

namespace {
    constexpr DWORD kRecreateRetryDelayMs = 500;

    D2D1::Matrix3x2F RotationTransform(DXGI_MODE_ROTATION rot, UINT width, UINT height) {
        using namespace D2D1;
        switch (rot) {
        case DXGI_MODE_ROTATION_ROTATE90:
            return Matrix3x2F::Rotation(90.0f) * Matrix3x2F::Translation((float)width, 0);
        case DXGI_MODE_ROTATION_ROTATE180:
            return Matrix3x2F::Rotation(180.0f) * Matrix3x2F::Translation((float)width, (float)height);
        case DXGI_MODE_ROTATION_ROTATE270:
            return Matrix3x2F::Rotation(270.0f) * Matrix3x2F::Translation(0, (float)height);
        default:
            return Matrix3x2F::Identity();
        }
    }

    void ReadGeometry(HMONITOR mon, const RECT& rect, UINT& width, UINT& height, float& dpiScale) {
        width = rect.right - rect.left;
        height = rect.bottom - rect.top;
        UINT dpiX = 96, dpiY = 96;
        GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
        dpiScale = dpiX / 96.0f;
    }
}

DuplicationBlurRenderer::~DuplicationBlurRenderer() {
    m_quit = true;
    Wake();
    if (m_thread.joinable()) m_thread.join();
    if (m_wakeEvent) CloseHandle(m_wakeEvent);
    // Tier A/C don't need the overlay excluded from capture; leave the window as we found it
    // so a later renderer switch doesn't inherit the affinity.
    if (m_affinitySet) SetWindowDisplayAffinity(m_hwnd, WDA_NONE);
}

bool DuplicationBlurRenderer::Initialize(HWND overlay, HMONITOR mon) try {
    m_hwnd = overlay;
    m_notifyHwnd = overlay;

    // The overlay must be invisible to Desktop Duplication, otherwise it captures (and
    // blurs) its own previous frame in a feedback loop.
    if (!SetWindowDisplayAffinity(overlay, WDA_EXCLUDEFROMCAPTURE)) {
        Log(L"Tier B: SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE) failed %lu", GetLastError());
        return false;
    }
    m_affinitySet = true;

    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    ReadGeometry(mon, mi.rcMonitor, m_geom.width, m_geom.height, m_geom.dpiScale);
    m_pendingGeom = m_geom;

    if (!CreateDevices(mon)) return false;
    m_deviceReady = true;

    m_wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_thread = std::thread([this] { ThreadMain(); });
    return true;
} catch (winrt::hresult_error const& e) {
    Log(L"Tier B init failed: 0x%08X %s", e.code().value, e.message().c_str());
    return false;
}

void DuplicationBlurRenderer::Wake() {
    if (m_wakeEvent) SetEvent(m_wakeEvent);
}

// UI thread. Everything device-related is owned by the render thread, so only record the
// new geometry and ask it to rebuild on its next iteration.
void DuplicationBlurRenderer::OnMonitorChanged(HMONITOR monitor, RECT physicalRect) {
    {
        std::lock_guard<std::mutex> lock(m_pendingGeomMutex);
        ReadGeometry(monitor, physicalRect, m_pendingGeom.width, m_pendingGeom.height, m_pendingGeom.dpiScale);
    }
    m_recreateRequested = true;
    Wake();
}

void DuplicationBlurRenderer::SetVisible(bool visible) {
    m_visible = visible;
    if (visible) Wake();
}

void DuplicationBlurRenderer::AnimateTo(OverlayTarget t, std::chrono::milliseconds dur,
    std::function<void()> done) {
    const uint64_t gen = ++m_generation;
    const auto now = std::chrono::steady_clock::now();

    {
        std::lock_guard<std::mutex> lock(m_doneMutex);
        m_pendingDone = std::move(done);
    }

    if (dur.count() <= 0) {
        m_anim.SetInstant(t, gen);
        m_current = t;
        Wake();
        // Tier A applies the same "don't strictly wait for the presented pixel" trade-off
        // for the instant path; mirror it here rather than round-tripping through the
        // render thread for a completion signal.
        PostMessageW(m_notifyHwnd, WM_APP_RENDER_ANIM_DONE_MSG, (WPARAM)gen, 0);
        return;
    }

    // Start from what is actually on screen right now (not the previous logical target),
    // so reversing or redirecting mid-flight is seamless.
    OverlayTarget from = m_anim.CurrentValue(now);
    m_anim.Start(from, t, dur, now, gen);
    m_current = t;
    Wake();
}

void DuplicationBlurRenderer::Cancel() {
    ++m_generation;
    std::lock_guard<std::mutex> lock(m_doneMutex);
    m_pendingDone = nullptr;
}

void DuplicationBlurRenderer::OnAnimDoneMessage(uint64_t generation) {
    std::function<void()> done;
    {
        std::lock_guard<std::mutex> lock(m_doneMutex);
        if (generation != m_generation.load()) return; // superseded by a later AnimateTo/Cancel
        done = std::move(m_pendingDone);
        m_pendingDone = nullptr;
    }
    if (done) done();
}

bool DuplicationBlurRenderer::CreateDevices(HMONITOR mon) {
    com_ptr<IDXGIFactory6> f;
    winrt::check_hresult(CreateDXGIFactory2(0, IID_PPV_ARGS(f.put())));

    bool found = false;
    for (UINT a = 0; !found; ++a) {
        com_ptr<IDXGIAdapter1> ad;
        if (f->EnumAdapterByGpuPreference(a, DXGI_GPU_PREFERENCE_MINIMUM_POWER,
            IID_PPV_ARGS(ad.put())) == DXGI_ERROR_NOT_FOUND) break;
        for (UINT o = 0; ; ++o) {
            com_ptr<IDXGIOutput> out;
            if (ad->EnumOutputs(o, out.put()) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_OUTPUT_DESC d{};
            out->GetDesc(&d);
            if (d.Monitor == mon) {
                m_adapter = ad;
                m_output = out;
                found = true;
                break;
            }
        }
    }
    if (!found) {
        Log(L"Tier B: no adapter/output owns this monitor");
        return false;
    }

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT hr = D3D11CreateDevice(m_adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags,
        nullptr, 0, D3D11_SDK_VERSION, m_d3d.put(), nullptr, m_ctx.put());
    if (FAILED(hr)) { Log(L"Tier B: D3D11CreateDevice failed 0x%08X", hr); return false; }

    auto dxgiDev = m_d3d.as<IDXGIDevice>();
    com_ptr<ID2D1Factory7> d2f;
    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(d2f.put()));
    if (FAILED(hr)) { Log(L"Tier B: D2D1CreateFactory failed 0x%08X", hr); return false; }
    hr = d2f->CreateDevice(dxgiDev.get(), m_d2dDevice.put());
    if (FAILED(hr)) { Log(L"Tier B: CreateDevice(D2D) failed 0x%08X", hr); return false; }
    hr = m_d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, m_d2d.put());
    if (FAILED(hr)) { Log(L"Tier B: CreateDeviceContext failed 0x%08X", hr); return false; }
    m_d2d->SetDpi(96.0f, 96.0f); // 1 DIP == 1 px; sigma is pre-scaled by dpiScale ourselves

    hr = m_d2d->CreateEffect(CLSID_D2D1GaussianBlur, m_blur.put());
    if (FAILED(hr)) { Log(L"Tier B: CreateEffect(GaussianBlur) failed 0x%08X", hr); return false; }
    m_blur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD);

    hr = m_d2d->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), m_black.put());
    if (FAILED(hr)) { Log(L"Tier B: CreateSolidColorBrush failed 0x%08X", hr); return false; }

    return CreateDuplication() && CreateSwapChain();
}

bool DuplicationBlurRenderer::CreateDuplication() {
    m_dupl = nullptr;
    const DXGI_FORMAT fmts[] = { DXGI_FORMAT_B8G8R8A8_UNORM };
    HRESULT hr = E_FAIL;
    if (auto o5 = m_output.try_as<IDXGIOutput5>())
        hr = o5->DuplicateOutput1(m_d3d.get(), 0, ARRAYSIZE(fmts), fmts, m_dupl.put());
    if (FAILED(hr)) {
        auto o1 = m_output.as<IDXGIOutput1>();
        hr = o1->DuplicateOutput(m_d3d.get(), m_dupl.put());
    }
    if (FAILED(hr)) {
        Log(L"Tier B: DuplicateOutput failed 0x%08X", hr);
        return false;
    }

    DXGI_OUTDUPL_DESC dd{};
    m_dupl->GetDesc(&dd);
    m_rotation = dd.Rotation;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = dd.ModeDesc.Width;
    td.Height = dd.ModeDesc.Height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc = { 1, 0 };
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    m_frameTex = nullptr;
    hr = m_d3d->CreateTexture2D(&td, nullptr, m_frameTex.put());
    if (FAILED(hr)) { Log(L"Tier B: CreateTexture2D failed 0x%08X", hr); return false; }

    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    m_frameBmp = nullptr;
    hr = m_d2d->CreateBitmapFromDxgiSurface(m_frameTex.as<IDXGISurface>().get(), &bp, m_frameBmp.put());
    if (FAILED(hr)) { Log(L"Tier B: CreateBitmapFromDxgiSurface(frame) failed 0x%08X", hr); return false; }
    m_blur->SetInput(0, m_frameBmp.get());
    return true;
}

bool DuplicationBlurRenderer::CreateSwapChain() {
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = m_geom.width;
    sd.Height = m_geom.height;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc = { 1, 0 };
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    com_ptr<IDXGIFactory2> f;
    HRESULT hr = m_adapter->GetParent(IID_PPV_ARGS(f.put()));
    if (FAILED(hr)) { Log(L"Tier B: GetParent(IDXGIFactory2) failed 0x%08X", hr); return false; }

    com_ptr<IDXGISwapChain1> sc;
    hr = f->CreateSwapChainForComposition(m_d3d.get(), &sd, nullptr, sc.put());
    if (FAILED(hr)) { Log(L"Tier B: CreateSwapChainForComposition failed 0x%08X", hr); return false; }
    m_swap = sc.as<IDXGISwapChain2>();
    m_swap->SetMaximumFrameLatency(1);
    m_waitable.attach(m_swap->GetFrameLatencyWaitableObject());

    hr = DCompositionCreateDevice(m_d3d.as<IDXGIDevice>().get(), IID_PPV_ARGS(m_dcomp.put()));
    if (FAILED(hr)) { Log(L"Tier B: DCompositionCreateDevice failed 0x%08X", hr); return false; }
    hr = m_dcomp->CreateTargetForHwnd(m_hwnd, TRUE, m_dtarget.put());
    if (FAILED(hr)) { Log(L"Tier B: CreateTargetForHwnd failed 0x%08X", hr); return false; }
    hr = m_dcomp->CreateVisual(m_dvisual.put());
    if (FAILED(hr)) { Log(L"Tier B: CreateVisual failed 0x%08X", hr); return false; }
    hr = m_dvisual->SetContent(m_swap.get());
    if (FAILED(hr)) { Log(L"Tier B: SetContent failed 0x%08X", hr); return false; }
    hr = m_dtarget->SetRoot(m_dvisual.get());
    if (FAILED(hr)) { Log(L"Tier B: SetRoot failed 0x%08X", hr); return false; }
    hr = m_dcomp->Commit();
    if (FAILED(hr)) { Log(L"Tier B: Commit failed 0x%08X", hr); return false; }

    com_ptr<IDXGISurface> bb;
    hr = m_swap->GetBuffer(0, IID_PPV_ARGS(bb.put()));
    if (FAILED(hr)) { Log(L"Tier B: GetBuffer failed 0x%08X", hr); return false; }
    auto tp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    m_target = nullptr;
    hr = m_d2d->CreateBitmapFromDxgiSurface(bb.get(), &tp, m_target.put());
    if (FAILED(hr)) { Log(L"Tier B: CreateBitmapFromDxgiSurface(target) failed 0x%08X", hr); return false; }
    m_d2d->SetTarget(m_target.get());
    return true;
}

bool DuplicationBlurRenderer::RecreateAll() {
    {
        std::lock_guard<std::mutex> lock(m_pendingGeomMutex);
        m_geom = m_pendingGeom;
    }
    m_dupl = nullptr;
    m_frameBmp = nullptr;
    m_frameTex = nullptr;
    m_target = nullptr;
    m_waitable.close();
    m_swap = nullptr;
    m_dvisual = nullptr;
    m_dtarget = nullptr;
    m_dcomp = nullptr;
    m_d2d->SetTarget(nullptr);
    m_deviceReady = CreateDuplication() && CreateSwapChain();
    if (!m_deviceReady) Log(L"Tier B: recreate failed; retrying in %lums", kRecreateRetryDelayMs);
    return m_deviceReady;
}

void DuplicationBlurRenderer::PollDuplication() {
    if (!m_dupl && !CreateDuplication()) return; // keep last good frame, retry next tick
    DXGI_OUTDUPL_FRAME_INFO fi{};
    com_ptr<IDXGIResource> res;
    HRESULT hr = m_dupl->AcquireNextFrame(0, &fi, res.put());
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return;
    if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_INVALID_CALL) { m_dupl = nullptr; return; }
    if (FAILED(hr)) return;
    if (fi.LastPresentTime.QuadPart != 0) {
        m_ctx->CopyResource(m_frameTex.get(), res.as<ID3D11Texture2D>().get());
    }
    m_dupl->ReleaseFrame();
}

void DuplicationBlurRenderer::ThreadMain() {
    SetThreadDescription(GetCurrentThread(), L"OverlayRender");
    DWORD mmcssTask = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Games", &mmcssTask);

    while (!m_quit.load()) {
        if (!m_visible.load()) {
            WaitForSingleObject(m_wakeEvent, INFINITE);
            continue;
        }

        // Device (re)creation happens here and only here, so the UI thread never touches
        // D3D/D2D/DComp objects while a frame is being drawn.
        bool recreate = m_recreateRequested.exchange(false);
        if (!m_deviceReady) {
            WaitForSingleObject(m_wakeEvent, kRecreateRetryDelayMs);
            if (m_quit.load()) break;
            recreate = true;
        }
        if (recreate && !RecreateAll()) continue;

        WaitForSingleObjectEx(m_waitable.get(), 1000, TRUE);

        PollDuplication();

        const auto s = m_anim.Sample_(std::chrono::steady_clock::now());
        m_d2d->BeginDraw();
        if (s.blur < 0.05f && s.dim < 0.002f) {
            m_d2d->Clear(D2D1::ColorF(0, 0, 0, 0));
        } else {
            const float sigmaPx = s.blur * m_geom.dpiScale;
            m_blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, sigmaPx);
            m_blur->SetValue(D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION,
                sigmaPx > 8 ? D2D1_GAUSSIANBLUR_OPTIMIZATION_SPEED : D2D1_GAUSSIANBLUR_OPTIMIZATION_BALANCED);
            m_d2d->SetTransform(RotationTransform(m_rotation, m_geom.width, m_geom.height));
            m_d2d->DrawImage(m_blur.get());
            m_d2d->SetTransform(D2D1::Matrix3x2F::Identity());
            m_black->SetOpacity(s.dim);
            m_d2d->FillRectangle(D2D1::RectF(0, 0, (float)m_geom.width, (float)m_geom.height), m_black.get());
        }
        HRESULT hr = m_d2d->EndDraw();
        if (hr == D2DERR_RECREATE_TARGET) { m_deviceReady = false; continue; }
        hr = m_swap->Present(1, 0);
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) { m_deviceReady = false; continue; }

        if (s.finished) {
            PostMessageW(m_notifyHwnd, WM_APP_RENDER_ANIM_DONE_MSG, (WPARAM)s.generation, 0);
        }
    }

    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
}
