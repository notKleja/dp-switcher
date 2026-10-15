#pragma once
#include "host/Interfaces.h"
#include "render/IOverlayRenderer.h"
#include "render/NullRenderer.h"
#include <memory>
#include <chrono>
#include <unordered_map>

enum class SwitchState { Idle, Covering, Switching, AwayConfirmedWaitTarget, AwayConfirmed, RevealGrace, Revealing };

const wchar_t* SwitchStateName(SwitchState s);

// Drives the whole cover -> switch -> confirm -> (stay covered until our input returns) ->
// reveal flow described in the design doc's section 7. Owns no rendering itself; delegates
// to whichever IOverlayRenderer SelectRenderer() chose, and talks to the outside world only
// through the small interfaces in host/Interfaces.h so the state machine can be tested
// headlessly (tests/SwitchControllerTests.cpp).
class SwitchController {
public:
    struct Config {
        BYTE ourInputValue = 0;    // this PC's own VCP 0x60 value (e.g. DisplayPort)
        BYTE otherInputValue = 0;  // the other computer's value (e.g. USB-C / Mac)
        // Extra wait between "DDC confirms our input is active" and starting the reveal.
        // A monitor's panel can take a while to actually lock onto the signal after the
        // input register flips; if the reveal finishes before that, the user sees the
        // desktop pop in with no animation. Tunable from the tray Settings menu.
        UINT revealGraceMs = 0;
        // Some monitor firmware changes USB routing during continuous DDC traffic.
        // Default to explicit user requests only; automatic return detection is opt-in.
        bool backgroundPolling = false;
    };

    struct Deps {
        IOverlayHost* overlay = nullptr;
        INotifier* notifier = nullptr;
        IDdcChannel* ddc = nullptr;
        ITimerHost* timers = nullptr;
        IClock* clock = nullptr;
    };

    // `renderer` may be null (every tier failed): the switch then runs without an overlay.
    // `monitor` may be null (configured monitor not connected): polling waits for it.
    void Initialize(Config cfg, Deps deps, std::unique_ptr<IOverlayRenderer> renderer, HMONITOR monitor);
    void SetRevealGraceMs(UINT ms);

    // User-initiated actions.
    void RequestSwitch(BYTE targetValue, bool instantCover);
    void RequestToggleSwitch(); // left-click: read current input, then switch to the other one
    void RequestCancel();       // click-to-reset / ESC

    // Routed from the controller window's WndProc.
    void OnDdcResult(const DdcResult& result);
    void OnTimerTick(UINT_PTR timerId);
    void OnMonitorChanged(HMONITOR monitor, RECT physicalRect);
    void OnMonitorLost();
    void OnRenderAnimDone(uint64_t generation);

    SwitchState State() const { return m_state; }
    bool MonitorPresent() const { return m_monitorPresent; }
    HMONITOR Monitor() const { return m_monitor; }

    // Never null: falls back to an internal no-op renderer when none is installed.
    IOverlayRenderer& Renderer();
    // Only meaningful while Idle. Passing null first lets the caller destroy the old
    // renderer before constructing its replacement (they compete for the same HWND).
    void ReplaceRenderer(std::unique_ptr<IOverlayRenderer> renderer);

    static constexpr UINT_PTR kFastPollTimerId = 0xDF10;
    static constexpr UINT_PTR kSlowPollTimerId = 0xDF11;
    static constexpr UINT_PTR kDeadlineTimerId = 0xDF12;
    static constexpr UINT_PTR kIdleWatchTimerId = 0xDF13;
    static constexpr UINT_PTR kRevealGraceTimerId = 0xDF14;
    static constexpr UINT_PTR kHeartbeatTimerId = 0xDF15;

    static constexpr UINT kFastPollIntervalMs = 150;
    static constexpr UINT kSlowPollIntervalMs = 500;
    static constexpr int kRequiredConsecutiveMatches = 2;
    // Idle watch: notices when the *other* computer takes the monitor without us being
    // involved, so we can pre-cover and get a proper reveal animation on the way back.
    // Slower cadence than the switch polls (nothing user-visible depends on its latency),
    // and a stricter match count, because a false positive here would black out a desktop
    // the user is actively using.
    static constexpr UINT kIdleWatchIntervalMs = 1000;
    static constexpr int kRequiredExternalAwayMatches = 3;
    // Logged bursts of up to 9 consecutive failures are normal for this monitor while it is
    // mid-switch, so the "DDC went silent" threshold has to sit comfortably above that.
    static constexpr int kMaxConsecutiveReadFailures = 10;
    static constexpr UINT kSwitchTimeoutMs = 8000;
    // Safety net for AwayConfirmed, which otherwise waits indefinitely by design: if reads
    // keep not matching our input for this long *and* there's been recent local mouse/
    // keyboard activity, reveal anyway rather than risk staying stuck on a DDC noise burst.
    static constexpr UINT kAwayConfirmedStuckTimeoutMs = 15000;
    static constexpr UINT kLocalInputRecentWindowMs = 3000;
    // A poll read that hasn't come back in this long is abandoned so polling can resume once
    // the worker unblocks (a dxva2 call can wedge on a powered-off or re-enumerating monitor).
    static constexpr UINT kPendingPollTimeoutMs = 5000;
    static constexpr UINT kHeartbeatIntervalMs = 5 * 60 * 1000;
    static constexpr UINT kWorkerStuckWarnMs = 10000;

private:
    void SetState(SwitchState next);
    void EnterCovering(BYTE target, bool instant);
    void EnterAwayFromExternalSwitch();
    void EnterSwitching();
    void EnterAwayConfirmedWaitTarget();
    void EnterAwayConfirmed();
    void EnterRevealAfterGrace();
    void EnterRevealing(std::chrono::milliseconds duration, bool pinToCoveredFirst);
    void EnterIdle();

    bool EnsureMonitorPresent();
    void ShowOverlay();
    void HideOverlay();
    void RestartPollTimerForState();
    void Heartbeat();
    void Notify(const wchar_t* text);

    void PostDdc(DdcRequest req);
    void StartTimer(UINT_PTR id, UINT intervalMs);
    void StopTimer(UINT_PTR id);
    void StopTransientTimers(); // everything except the heartbeat

    std::chrono::steady_clock::time_point Now() const;
    long long MsSince(std::chrono::steady_clock::time_point t) const;

    Config m_cfg{};
    Deps m_deps{};
    std::unique_ptr<IOverlayRenderer> m_renderer;
    SyncNullRenderer m_noRenderer;
    HMONITOR m_monitor = nullptr;
    bool m_monitorPresent = false;

    SwitchState m_state = SwitchState::Idle;
    std::chrono::steady_clock::time_point m_stateEnteredAt{};
    BYTE m_targetValue = 0;
    int m_consecutiveMatches = 0;
    int m_consecutiveReadFailures = 0;
    int m_idleAwayMatches = 0;
    int m_lastIdleLoggedValue = -1;
    UINT64 m_nextDdcRequestId = 1;
    UINT64 m_pendingWriteId = 0;
    UINT64 m_pendingPollReadId = 0;
    UINT64 m_pendingToggleReadId = 0;
    std::chrono::steady_clock::time_point m_pendingPollPostedAt{};
    std::chrono::steady_clock::time_point m_phaseDeadline{};
    std::chrono::steady_clock::time_point m_awayConfirmedEnteredAt{};
    std::chrono::steady_clock::time_point m_lastHeartbeatAt{};

    // Idle-watch reads fire forever; individual results are only logged when the value
    // changes, and these counters are summarized (then reset) by each heartbeat line.
    struct IdleStats { unsigned reads = 0; unsigned failures = 0; int failStreak = 0; };
    IdleStats m_idleStats;

    struct DdcPostInfo { std::chrono::steady_clock::time_point postedAt; DdcRequest::Kind kind; };
    std::unordered_map<UINT64, DdcPostInfo> m_ddcPostInfo;
};
