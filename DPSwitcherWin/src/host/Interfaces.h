#pragma once
#include <windows.h>
#include <chrono>
#include "../DdcTypes.h"

// The controller's view of the outside world. Each interface is deliberately tiny so the
// real Win32 objects (OverlayWindow, TrayIcon, DdcWorker, SetTimer, GetLastInputInfo) can
// be swapped for in-memory fakes in tests/SwitchControllerTests.cpp.

struct IOverlayHost {
    virtual ~IOverlayHost() = default;
    virtual void PlaceOn(HMONITOR monitor) = 0;
    virtual void Show() = 0;
    virtual void Hide() = 0;
};

struct INotifier {
    virtual ~INotifier() = default;
    virtual void ShowBalloon(const wchar_t* title, const wchar_t* text) = 0;
};

struct IDdcChannel {
    virtual ~IDdcChannel() = default;
    virtual void Post(const DdcRequest& request) = 0;
    // How long the worker has been inside its current dxva2 call (0 when idle). The
    // controller's heartbeat uses this to spot a wedged DDC/CI driver call.
    virtual long long BusyMs() const = 0;
    virtual size_t QueueDepth() const = 0;
};

struct ITimerHost {
    virtual ~ITimerHost() = default;
    virtual void Start(UINT_PTR id, UINT intervalMs) = 0;
    virtual void Stop(UINT_PTR id) = 0;
};

struct IClock {
    virtual ~IClock() = default;
    virtual std::chrono::steady_clock::time_point Now() const = 0;
    virtual bool LocalInputWithin(DWORD ms) const = 0;
};
