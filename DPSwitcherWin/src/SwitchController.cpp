#include "SwitchController.h"
#include "render/TransitionStyle.h"
#include "util/Log.h"

const wchar_t* SwitchStateName(SwitchState s) {
    switch (s) {
    case SwitchState::Idle: return L"Idle";
    case SwitchState::Covering: return L"Covering";
    case SwitchState::Switching: return L"Switching";
    case SwitchState::AwayConfirmedWaitTarget: return L"AwayConfirmedWaitTarget";
    case SwitchState::AwayConfirmed: return L"AwayConfirmed";
    case SwitchState::RevealGrace: return L"RevealGrace";
    case SwitchState::Revealing: return L"Revealing";
    }
    return L"?";
}

namespace {
    long long MsBetween(std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(to - from).count();
    }
}

std::chrono::steady_clock::time_point SwitchController::Now() const { return m_deps.clock->Now(); }
long long SwitchController::MsSince(std::chrono::steady_clock::time_point t) const { return MsBetween(t, Now()); }

void SwitchController::Initialize(Config cfg, Deps deps, std::unique_ptr<IOverlayRenderer> renderer, HMONITOR monitor) {
    m_cfg = cfg;
    m_deps = deps;
    m_renderer = std::move(renderer);
    m_monitor = monitor;
    m_monitorPresent = (monitor != nullptr);
    m_stateEnteredAt = Now();
    m_lastHeartbeatAt = Now();

    Log(L"controller: our=0x%02X other=0x%02X revealGraceMs=%u renderer=%s monitor=%s",
        m_cfg.ourInputValue, m_cfg.otherInputValue, m_cfg.revealGraceMs, Renderer().Name(),
        m_monitorPresent ? L"present" : L"absent");
    if (m_monitorPresent) StartTimer(kIdleWatchTimerId, kIdleWatchIntervalMs);
    StartTimer(kHeartbeatTimerId, kHeartbeatIntervalMs);
}

void SwitchController::SetRevealGraceMs(UINT ms) {
    m_cfg.revealGraceMs = ms;
    Log(L"controller: revealGraceMs=%u", ms);
}

IOverlayRenderer& SwitchController::Renderer() {
    return m_renderer ? *m_renderer : static_cast<IOverlayRenderer&>(m_noRenderer);
}

void SwitchController::ReplaceRenderer(std::unique_ptr<IOverlayRenderer> renderer) {
    if (m_renderer) m_renderer->Cancel();
    m_renderer = std::move(renderer);
    Log(L"controller: renderer is now %s", Renderer().Name());
}

void SwitchController::SetState(SwitchState next) {
    Log(L"state: %s -> %s (after %lldms)", SwitchStateName(m_state), SwitchStateName(next), MsSince(m_stateEnteredAt));
    m_state = next;
    m_stateEnteredAt = Now();
}

void SwitchController::PostDdc(DdcRequest req) {
    m_ddcPostInfo[req.id] = { Now(), req.kind };
    m_deps.ddc->Post(req);
}

void SwitchController::Notify(const wchar_t* text) {
    m_deps.notifier->ShowBalloon(L"Monitor Switcher", text);
}

bool SwitchController::EnsureMonitorPresent() {
    if (m_monitorPresent) return true;
    Log(L"request refused: target monitor is not connected");
    Notify(L"The target monitor is not connected.");
    return false;
}

void SwitchController::ShowOverlay() {
    if (!Renderer().HasVisuals()) return;
    m_deps.overlay->PlaceOn(m_monitor);
    m_deps.overlay->Show();
    Renderer().SetVisible(true);
}

void SwitchController::HideOverlay() {
    if (!Renderer().HasVisuals()) return;
    m_deps.overlay->Hide();
    Renderer().SetVisible(false);
}

void SwitchController::RequestToggleSwitch() {
    Log(L"request: toggle (state=%s)", SwitchStateName(m_state));
    if (m_state != SwitchState::Idle) { RequestCancel(); return; } // click during a transition = cancel it
    if (!EnsureMonitorPresent()) return;
    m_pendingToggleReadId = m_nextDdcRequestId++;
    PostDdc(DdcRequest{ DdcRequest::Read, m_monitor, 0x60, 0, m_pendingToggleReadId });
}

void SwitchController::RequestSwitch(BYTE targetValue, bool instantCover) {
    Log(L"request: switch to 0x%02X instant=%d (state=%s)", targetValue, instantCover, SwitchStateName(m_state));
    if (!m_monitorPresent) {
        if (m_state != SwitchState::Idle) RequestCancel();
        EnsureMonitorPresent();
        return;
    }
    if (m_state != SwitchState::Idle) {
        // A switch is already in flight; redirect to the new target. The cover animation
        // continues from wherever the overlay currently is (AnimateTo always starts from the
        // presented value), so this is seamless rather than a snap to black.
        Log(L"redirecting in-flight switch to 0x%02X", targetValue);
        Renderer().Cancel();
        StopTransientTimers();
        m_pendingWriteId = 0;
    }
    EnterCovering(targetValue, instantCover);
}

void SwitchController::RequestCancel() {
    if (m_state == SwitchState::Idle) return;
    if (m_state == SwitchState::Revealing) {
        Log(L"request: cancel ignored, already revealing");
        return;
    }
    Log(L"request: cancel (state=%s)", SwitchStateName(m_state));
    Renderer().Cancel();
    // Coming out of a long "fully black" stretch the presented value is pinned to covered
    // first so the reveal always starts from the same place; mid-cover, reverse from
    // wherever the animation got to, quickly.
    const bool pin = (m_state == SwitchState::AwayConfirmed || m_state == SwitchState::RevealGrace);
    const auto dur = (m_state == SwitchState::Covering) ? style::kCancelDuration : style::kDuration;
    EnterRevealing(dur, pin);
}

void SwitchController::EnterCovering(BYTE target, bool instant) {
    SetState(SwitchState::Covering);
    m_targetValue = target;
    m_consecutiveMatches = 0;
    m_consecutiveReadFailures = 0;
    m_idleAwayMatches = 0;
    StopTimer(kIdleWatchTimerId);

    ShowOverlay();

    OverlayTarget to{ style::kMaxBlurDip, style::kMaxDim };
    auto dur = instant ? std::chrono::milliseconds{ 0 } : style::kDuration;
    Renderer().AnimateTo(to, dur, [this] {
        Log(L"cover animation done (state=%s)", SwitchStateName(m_state));
        if (m_state == SwitchState::Covering) EnterSwitching();
    });
}

// The other computer took the monitor without us being involved (e.g. the Mac's own tray
// menu). Nobody can see the Windows desktop at this point, so cover instantly -- the only
// purpose is to be already-black when the input comes back, so the return gets the real
// reveal animation instead of the desktop just popping into view.
void SwitchController::EnterAwayFromExternalSwitch() {
    Log(L"monitor taken by input 0x%02X externally, pre-covering", m_cfg.otherInputValue);
    m_idleAwayMatches = 0;
    StopTimer(kIdleWatchTimerId);

    ShowOverlay();
    Renderer().AnimateTo({ style::kMaxBlurDip, style::kMaxDim }, std::chrono::milliseconds{ 0 }, nullptr);

    EnterAwayConfirmed();
}

void SwitchController::EnterSwitching() {
    SetState(SwitchState::Switching);
    m_pendingWriteId = m_nextDdcRequestId++;
    PostDdc(DdcRequest{ DdcRequest::Write, m_monitor, 0x60, m_targetValue, m_pendingWriteId });
    m_phaseDeadline = Now() + std::chrono::milliseconds(kSwitchTimeoutMs);
    StartTimer(kDeadlineTimerId, kSwitchTimeoutMs);
}

void SwitchController::EnterAwayConfirmedWaitTarget() {
    SetState(SwitchState::AwayConfirmedWaitTarget);
    m_consecutiveMatches = 0;
    StartTimer(kFastPollTimerId, kFastPollIntervalMs);
}

void SwitchController::EnterAwayConfirmed() {
    SetState(SwitchState::AwayConfirmed);
    m_consecutiveMatches = 0;
    m_consecutiveReadFailures = 0;
    m_awayConfirmedEnteredAt = Now();
    StopTimer(kFastPollTimerId);
    StopTimer(kDeadlineTimerId);
    StopTimer(kIdleWatchTimerId);
    StartTimer(kSlowPollTimerId, kSlowPollIntervalMs);
}

void SwitchController::EnterRevealAfterGrace() {
    if (m_cfg.revealGraceMs == 0) {
        EnterRevealing(style::kDuration, true);
        return;
    }
    SetState(SwitchState::RevealGrace);
    StopTransientTimers();
    StartTimer(kRevealGraceTimerId, m_cfg.revealGraceMs);
}

void SwitchController::EnterRevealing(std::chrono::milliseconds duration, bool pinToCoveredFirst) {
    SetState(SwitchState::Revealing);
    StopTransientTimers();
    if (pinToCoveredFirst) {
        // Pin the presented value to fully-covered before starting the reveal, rather than
        // trusting that nothing drifted it in the (sometimes long, indefinite) time spent in
        // AwayConfirmed -- a zero-duration AnimateTo is a no-op if it was already there, and a
        // guaranteed clean start if it wasn't.
        Renderer().AnimateTo({ style::kMaxBlurDip, style::kMaxDim }, std::chrono::milliseconds{ 0 }, nullptr);
    }
    OverlayTarget to{ 0.0f, 0.0f };
    Renderer().AnimateTo(to, duration, [this] {
        Log(L"reveal animation done (state=%s)", SwitchStateName(m_state));
        if (m_state == SwitchState::Revealing) EnterIdle();
    });
}

void SwitchController::EnterIdle() {
    SetState(SwitchState::Idle);
    StopTransientTimers();
    HideOverlay();
    m_idleAwayMatches = 0;
    m_lastIdleLoggedValue = -1;
    if (m_monitorPresent) StartTimer(kIdleWatchTimerId, kIdleWatchIntervalMs);
}

void SwitchController::StartTimer(UINT_PTR id, UINT intervalMs) {
    if (!m_cfg.backgroundPolling &&
        (id == kFastPollTimerId || id == kSlowPollTimerId || id == kIdleWatchTimerId)) return;
    m_deps.timers->Start(id, intervalMs);
}
void SwitchController::StopTimer(UINT_PTR id) { m_deps.timers->Stop(id); }

void SwitchController::StopTransientTimers() {
    StopTimer(kFastPollTimerId);
    StopTimer(kSlowPollTimerId);
    StopTimer(kDeadlineTimerId);
    StopTimer(kIdleWatchTimerId);
    StopTimer(kRevealGraceTimerId);
}

void SwitchController::RestartPollTimerForState() {
    switch (m_state) {
    case SwitchState::Idle: StartTimer(kIdleWatchTimerId, kIdleWatchIntervalMs); break;
    case SwitchState::AwayConfirmedWaitTarget: StartTimer(kFastPollTimerId, kFastPollIntervalMs); break;
    case SwitchState::AwayConfirmed: StartTimer(kSlowPollTimerId, kSlowPollIntervalMs); break;
    default: break; // Covering/Switching/RevealGrace/Revealing don't poll
    }
}

void SwitchController::OnDdcResult(const DdcResult& result) {
    long long latencyMs = -1;
    DdcRequest::Kind kind = DdcRequest::Read;
    if (auto it = m_ddcPostInfo.find(result.id); it != m_ddcPostInfo.end()) {
        latencyMs = MsSince(it->second.postedAt);
        kind = it->second.kind;
        m_ddcPostInfo.erase(it);
    }

    // Idle-watch reads fire forever, and this monitor fails roughly every other one, so
    // logging each result (or each change between "failed" and "ok") would fill the log
    // with noise. Successes are logged only when the value changes; failures only when a
    // streak reaches the "DDC went silent" threshold. The heartbeat summarizes the rest.
    const bool isIdlePoll = (m_state == SwitchState::Idle && result.id == m_pendingPollReadId);
    bool shouldLog = true;
    if (isIdlePoll) {
        m_idleStats.reads++;
        if (FAILED(result.hr)) {
            m_idleStats.failures++;
            m_idleStats.failStreak++;
            shouldLog = (m_idleStats.failStreak == kMaxConsecutiveReadFailures);
        } else {
            m_idleStats.failStreak = 0;
            const int value = static_cast<int>(result.current & 0xFF);
            shouldLog = (value != m_lastIdleLoggedValue);
            if (shouldLog) m_lastIdleLoggedValue = value;
        }
    }
    if (shouldLog) {
        Log(L"ddc: %s id=%llu state=%s hr=0x%08X value=%lu latency=%lldms%s",
            kind == DdcRequest::Write ? L"WRITE" : L"read", result.id, SwitchStateName(m_state),
            result.hr, result.current, latencyMs,
            (isIdlePoll && FAILED(result.hr)) ? L" (idle watch: DDC has gone silent)" : L"");
    }

    if (result.id == m_pendingWriteId && m_state == SwitchState::Switching) {
        m_pendingWriteId = 0;
        if (FAILED(result.hr)) {
            Log(L"write to 0x%02X failed hr=0x%08X, revealing", m_targetValue, result.hr);
            Notify(L"Failed to switch input. See log for details.");
            EnterRevealing(style::kDuration, false);
            return;
        }
        if (!m_cfg.backgroundPolling) {
            // Do not keep querying the monitor or leave a cover waiting for return
            // detection. A successful write is not a verified input/KVM state.
            EnterRevealAfterGrace();
            return;
        }
        EnterAwayConfirmedWaitTarget();
        return;
    }

    if (result.id == m_pendingToggleReadId) {
        m_pendingToggleReadId = 0;
        if (m_state != SwitchState::Idle) return; // a cancel/other request beat this read back
        // Unknown or failed read defaults to switching toward the other computer, matching
        // the original PowerShell prototype's behavior for an indeterminate current state.
        BYTE target = m_cfg.otherInputValue;
        if (SUCCEEDED(result.hr) && result.current == m_cfg.otherInputValue) {
            target = m_cfg.ourInputValue;
        }
        EnterCovering(target, false);
        return;
    }

    if (result.id == m_pendingPollReadId) {
        m_pendingPollReadId = 0;

        if (FAILED(result.hr)) {
            // A failed read is not a contradicting value, so it does NOT reset the match
            // counters. This monitor fails roughly every other DDC read (in bursts around a
            // switch), and requiring two *uninterrupted* successes was measurably delaying
            // detection; only a successful read of a different value resets the count.
            m_consecutiveReadFailures++;
        } else {
            m_consecutiveReadFailures = 0;
            BYTE current = static_cast<BYTE>(result.current);

            if (m_state == SwitchState::Idle) {
                if (current == m_cfg.otherInputValue) {
                    if (++m_idleAwayMatches >= kRequiredExternalAwayMatches) EnterAwayFromExternalSwitch();
                } else {
                    m_idleAwayMatches = 0;
                }
            } else if (m_state == SwitchState::AwayConfirmedWaitTarget) {
                if (current == m_targetValue) {
                    if (++m_consecutiveMatches >= kRequiredConsecutiveMatches) {
                        EnterAwayConfirmed();
                        // The value we just confirmed may already be our own input (e.g. the
                        // user picked "switch to DisplayPort", targeting this PC) -- in that
                        // case reveal immediately instead of waiting for a second read.
                        if (m_targetValue == m_cfg.ourInputValue) EnterRevealAfterGrace();
                    }
                } else {
                    m_consecutiveMatches = 0;
                }
            } else if (m_state == SwitchState::AwayConfirmed) {
                if (current == m_cfg.ourInputValue) {
                    if (++m_consecutiveMatches >= kRequiredConsecutiveMatches) {
                        Log(L"our input confirmed back after %lldms away", MsSince(m_awayConfirmedEnteredAt));
                        EnterRevealAfterGrace();
                    }
                } else {
                    m_consecutiveMatches = 0;
                }
            }
        }

        // Safety net: AwayConfirmed otherwise waits indefinitely by design (it may genuinely
        // stay away for hours), but DDC reads can be noisy for an extended stretch right after
        // a physical input switch (documented I2C burst-noise behavior) -- either returning
        // read failures or just never landing 2 clean consecutive matches. If we've been stuck
        // a while *and* there's recent local mouse/keyboard activity (the user is actually
        // sitting at this PC), reveal anyway rather than leave them staring at black.
        if (m_state == SwitchState::AwayConfirmed) {
            bool stuckTooLong = MsSince(m_awayConfirmedEnteredAt) > static_cast<long long>(kAwayConfirmedStuckTimeoutMs);
            bool manyFailures = m_consecutiveReadFailures >= kMaxConsecutiveReadFailures;
            if ((stuckTooLong || manyFailures) && m_deps.clock->LocalInputWithin(kLocalInputRecentWindowMs)) {
                Log(L"AwayConfirmed stuck (stuckTooLong=%d manyFailures=%d), revealing on local input activity",
                    stuckTooLong, manyFailures);
                EnterRevealing(style::kDuration, true);
            }
        }
        return;
    }

    // A result that doesn't match any pending write/poll/toggle id: an abandoned poll that
    // eventually came back, or a request superseded by a cancel. Nothing tracks it, so drop it.
    Log(L"ddc result for unrecognized request id=%llu (abandoned or superseded)", result.id);
}

void SwitchController::OnTimerTick(UINT_PTR timerId) {
    if (timerId == kDeadlineTimerId) {
        if (Now() >= m_phaseDeadline &&
            (m_state == SwitchState::Switching || m_state == SwitchState::AwayConfirmedWaitTarget)) {
            Log(L"switch timed out waiting for target 0x%02X, revealing", m_targetValue);
            Notify(L"Switch timed out. See log for details.");
            EnterRevealing(style::kDuration, false);
        }
        return;
    }

    if (timerId == kRevealGraceTimerId) {
        StopTimer(kRevealGraceTimerId);
        if (m_state == SwitchState::RevealGrace) EnterRevealing(style::kDuration, true);
        return;
    }

    if (timerId == kHeartbeatTimerId) {
        Heartbeat();
        return;
    }

    if (timerId != kFastPollTimerId && timerId != kSlowPollTimerId && timerId != kIdleWatchTimerId) return;
    if (!m_cfg.backgroundPolling) return; // also reject already-queued timer messages
    if (!m_monitorPresent) return;

    if (m_pendingPollReadId != 0) {
        const long long age = MsSince(m_pendingPollPostedAt);
        if (age < static_cast<long long>(kPendingPollTimeoutMs)) return; // previous poll read hasn't come back yet
        Log(L"ddc: poll read id=%llu outstanding for %lldms (worker busy %lldms, queue %zu); abandoning it",
            m_pendingPollReadId, age, m_deps.ddc->BusyMs(), m_deps.ddc->QueueDepth());
        m_pendingPollReadId = 0;
    }

    m_pendingPollReadId = m_nextDdcRequestId++;
    m_pendingPollPostedAt = Now();
    PostDdc(DdcRequest{ DdcRequest::Read, m_monitor, 0x60, 0, m_pendingPollReadId });
}

void SwitchController::Heartbeat() {
    const long long busy = m_deps.ddc->BusyMs();
    const double failPct = m_idleStats.reads ? 100.0 * m_idleStats.failures / m_idleStats.reads : 0.0;
    Log(L"heartbeat: state=%s for %llds, monitor=%s, idle reads=%u failures=%u (%.0f%%) lastValue=%d, "
        L"ddc busy=%lldms queue=%zu pendingPoll=%llu, renderer=%s",
        SwitchStateName(m_state), MsSince(m_stateEnteredAt) / 1000, m_monitorPresent ? L"present" : L"absent",
        m_idleStats.reads, m_idleStats.failures, failPct, m_lastIdleLoggedValue,
        busy, m_deps.ddc->QueueDepth(), m_pendingPollReadId, Renderer().Name());
    if (busy > static_cast<long long>(kWorkerStuckWarnMs)) {
        Log(L"heartbeat: WARNING the DDC worker has been inside one dxva2 call for %lldms; the driver may be wedged", busy);
    }
    m_idleStats = {};
    m_lastHeartbeatAt = Now();

    // Bookkeeping for requests whose results never arrived (abandoned polls, worker stuck).
    for (auto it = m_ddcPostInfo.begin(); it != m_ddcPostInfo.end();) {
        if (MsSince(it->second.postedAt) > 60000) it = m_ddcPostInfo.erase(it);
        else ++it;
    }
}

void SwitchController::OnMonitorChanged(HMONITOR monitor, RECT physicalRect) {
    Log(L"monitor changed: hmon=%p rect=(%ld,%ld)-(%ld,%ld) state=%s (was %s)", monitor,
        physicalRect.left, physicalRect.top, physicalRect.right, physicalRect.bottom, SwitchStateName(m_state),
        m_monitorPresent ? L"present" : L"absent");
    const bool wasPresent = m_monitorPresent;
    m_monitorPresent = true;
    m_monitor = monitor;
    Renderer().OnMonitorChanged(monitor, physicalRect);
    if (m_state != SwitchState::Idle && Renderer().HasVisuals()) m_deps.overlay->PlaceOn(monitor);
    if (!wasPresent) RestartPollTimerForState();
}

// The tracked monitor disappeared from the desktop (hot-plug pulse, cable, the Mac taking
// it with HPD). Polling pauses rather than silently retargeting some other physical monitor;
// the overlay (if up) stays as it is and OnMonitorChanged resumes everything when it returns.
void SwitchController::OnMonitorLost() {
    if (!m_monitorPresent) return;
    Log(L"monitor lost (state=%s); pausing DDC polling until it returns", SwitchStateName(m_state));
    m_monitorPresent = false;
    StopTimer(kFastPollTimerId);
    StopTimer(kSlowPollTimerId);
    StopTimer(kIdleWatchTimerId);
}

void SwitchController::OnRenderAnimDone(uint64_t generation) {
    Renderer().OnAnimDoneMessage(generation);
}
