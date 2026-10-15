#pragma once
#include "pch.h"
#include "AppMessages.h"
#include "DdcTypes.h"
#include "host/Interfaces.h"
#include <thread>
#include <mutex>
#include <deque>
#include <condition_variable>
#include <atomic>

// All dxva2 calls run on a dedicated worker thread; results are posted back to `notify`.
// A single SetVCPFeature can block for 50-200ms (real hardware over DDC/CI is slow and
// noisy), which would stall the UI thread's message pump and Composition batch callbacks
// if called directly there.
//
// Posted as WM_APP_DDC_RESULT_MSG with wParam = pointer to a heap-allocated DdcResult
// (owned by the receiver, must call `delete` after reading) and lParam = 0.
class DdcWorker : public IDdcChannel {
public:
    ~DdcWorker() override { Stop(); }

    void Start(HWND notify);
    void Stop();

    // IDdcChannel
    void Post(const DdcRequest& r) override;
    long long BusyMs() const override;
    size_t QueueDepth() const override;

    // Keep the PHYSICAL_MONITOR handle open across requests instead of re-enumerating it
    // for every read. Toggle-able at runtime so DdcProbe can A/B the two behaviours.
    void SetCacheHandles(bool on) { m_cacheHandles = on; }
    bool CacheHandles() const { return m_cacheHandles.load(); }

    // Requests beyond this are dropped (and logged) rather than queued indefinitely behind a
    // wedged dxva2 call.
    static constexpr size_t kMaxQueueDepth = 16;

private:
    void ThreadMain();
    bool AcquireHandle(HMONITOR mon, HANDLE& out);
    void ReleaseHandle(bool force);
    void NoteRequestOutcome(HRESULT hr);
    DdcResult DoRead(const DdcRequest& req);
    DdcResult DoWrite(const DdcRequest& req);

    HWND m_notify = nullptr;
    std::thread m_thread;
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<DdcRequest> m_queue;
    std::atomic<bool> m_quit{ false };
    std::atomic<bool> m_cacheHandles{ true };
    std::atomic<ULONGLONG> m_busySinceTick{ 0 };

    // Worker-thread only.
    HMONITOR m_cachedMon = nullptr;
    HANDLE m_cachedHandle = nullptr;
    int m_consecutiveFailedRequests = 0;
};
