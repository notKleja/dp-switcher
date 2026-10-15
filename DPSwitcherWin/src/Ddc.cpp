#include "Ddc.h"
#include "util/Log.h"
#include <vector>
#include <optional>

namespace {
    // Retry transient failures a few times with spacing -- DDC/CI over real hardware is slow
    // and noisy, and a single failed read/write is not conclusive.
    constexpr int kMaxRetries = 3;
    constexpr DWORD kRetrySpacingMs = 40;
    // After this many whole requests (all retries) failing back to back, assume the cached
    // handle went stale (monitor re-enumerated) and reopen it. Deliberately above the
    // strictly-alternating fail/ok pattern this monitor shows at rest.
    constexpr int kFailedRequestsBeforeReopen = 3;

    std::optional<HANDLE> OpenFirstPhysicalMonitor(HMONITOR mon) {
        DWORD count = 0;
        if (!GetNumberOfPhysicalMonitorsFromHMONITOR(mon, &count) || count == 0) {
            return std::nullopt;
        }
        std::vector<PHYSICAL_MONITOR> monitors(count);
        if (!GetPhysicalMonitorsFromHMONITOR(mon, count, monitors.data())) {
            return std::nullopt;
        }
        // Close the rest immediately; only the first is used (matches the existing
        // PowerShell implementation's behavior of using mons[0]).
        for (DWORD i = 1; i < count; ++i) {
            DestroyPhysicalMonitor(monitors[i].hPhysicalMonitor);
        }
        return monitors[0].hPhysicalMonitor;
    }

    HRESULT LastErrorAsHresult() {
        HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
        return SUCCEEDED(hr) ? E_FAIL : hr; // some drivers fail without setting a last error
    }
}

bool DdcWorker::AcquireHandle(HMONITOR mon, HANDLE& out) {
    if (m_cachedHandle && m_cachedMon == mon && m_cacheHandles.load()) {
        out = m_cachedHandle;
        return true;
    }
    ReleaseHandle(true);
    auto h = OpenFirstPhysicalMonitor(mon);
    if (!h) return false;
    m_cachedHandle = *h;
    m_cachedMon = mon;
    out = *h;
    return true;
}

void DdcWorker::ReleaseHandle(bool force) {
    if (!m_cachedHandle) return;
    if (!force && m_cacheHandles.load()) return;
    DestroyPhysicalMonitor(m_cachedHandle);
    m_cachedHandle = nullptr;
    m_cachedMon = nullptr;
}

void DdcWorker::NoteRequestOutcome(HRESULT hr) {
    if (SUCCEEDED(hr)) {
        m_consecutiveFailedRequests = 0;
        ReleaseHandle(false);
        return;
    }
    // Not logged: on this monitor runs of 3+ failures are routine, so a line per reopen
    // would be noise. The controller's heartbeat carries the failure rate instead.
    const bool reopen = (++m_consecutiveFailedRequests >= kFailedRequestsBeforeReopen);
    if (reopen) m_consecutiveFailedRequests = 0;
    ReleaseHandle(reopen);
}

DdcResult DdcWorker::DoRead(const DdcRequest& req) {
    DdcResult result{ req.id, E_FAIL, 0, 0 };
    HANDLE h = nullptr;
    if (!AcquireHandle(req.mon, h)) { result.hr = E_HANDLE; return result; }

    HRESULT hr = E_FAIL;
    for (int attempt = 0; attempt < kMaxRetries; ++attempt) {
        DWORD current = 0, maximum = 0;
        if (GetVCPFeatureAndVCPFeatureReply(h, req.vcp, nullptr, &current, &maximum)) {
            result.current = current & 0xFF; // some monitors set the high byte to junk
            result.maximum = maximum;
            hr = S_OK;
            break;
        }
        hr = LastErrorAsHresult();
        if (attempt + 1 < kMaxRetries) Sleep(kRetrySpacingMs);
    }
    result.hr = hr;
    NoteRequestOutcome(hr);
    return result;
}

DdcResult DdcWorker::DoWrite(const DdcRequest& req) {
    DdcResult result{ req.id, E_FAIL, 0, 0 };
    HANDLE h = nullptr;
    if (!AcquireHandle(req.mon, h)) { result.hr = E_HANDLE; return result; }

    // DWM presents asynchronously; by the time the cover animation's completion callback
    // fires on the UI thread, the fully-black frame may not actually be on screen yet.
    // Two flushes (one full composition pass each) guarantee it was presented before we
    // flip the input away, so the user never sees a flash of the real desktop mid-switch.
    DwmFlush();
    DwmFlush();

    HRESULT hr = E_FAIL;
    for (int attempt = 0; attempt < kMaxRetries; ++attempt) {
        if (SetVCPFeature(h, req.vcp, req.value)) {
            hr = S_OK;
            break;
        }
        hr = LastErrorAsHresult();
        if (attempt + 1 < kMaxRetries) Sleep(kRetrySpacingMs);
    }
    result.hr = hr;
    NoteRequestOutcome(hr);
    return result;
}

void DdcWorker::Start(HWND notify) {
    m_notify = notify;
    m_quit = false;
    m_thread = std::thread([this] { ThreadMain(); });
}

void DdcWorker::Stop() {
    m_quit = true;
    m_cv.notify_all();
    if (m_thread.joinable()) m_thread.join();
}

void DdcWorker::Post(const DdcRequest& r) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_queue.size() >= kMaxQueueDepth) {
            Log(L"ddc worker: queue full (%zu), dropping request id=%llu", m_queue.size(), r.id);
            return;
        }
        m_queue.push_back(r);
    }
    m_cv.notify_one();
}

long long DdcWorker::BusyMs() const {
    const ULONGLONG since = m_busySinceTick.load();
    return since ? static_cast<long long>(GetTickCount64() - since) : 0;
}

size_t DdcWorker::QueueDepth() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_queue.size();
}

void DdcWorker::ThreadMain() {
    SetThreadDescription(GetCurrentThread(), L"DdcWorker");
    while (!m_quit.load()) {
        DdcRequest req;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this] { return m_quit.load() || !m_queue.empty(); });
            if (m_quit.load()) break;
            req = m_queue.front();
            m_queue.pop_front();
        }

        m_busySinceTick = GetTickCount64();
        DdcResult result = (req.kind == DdcRequest::Read) ? DoRead(req) : DoWrite(req);
        m_busySinceTick = 0;

        if (m_notify) {
            auto* heapResult = new DdcResult(result);
            if (!PostMessageW(m_notify, WM_APP_DDC_RESULT_MSG, reinterpret_cast<WPARAM>(heapResult), 0)) {
                delete heapResult; // window gone (shutdown); nobody would ever free it
            }
        }
    }
    ReleaseHandle(true);
}
