// Headless tests for SwitchController. Every Win32 dependency is replaced by an in-memory
// fake (host/Interfaces.h), the renderer by one whose animations complete on demand, and
// time by a clock the test advances by hand.
#include "TestFramework.h"
#include "SwitchController.h"
#include "render/TransitionStyle.h"
#include <map>
#include <vector>
#include <string>

namespace {
    constexpr BYTE kOur = 0x0F;
    constexpr BYTE kOther = 0x1A;
    const HMONITOR kMonitor = reinterpret_cast<HMONITOR>(static_cast<UINT_PTR>(0x1234));
    const HMONITOR kMonitor2 = reinterpret_cast<HMONITOR>(static_cast<UINT_PTR>(0x5678));

    using ms = std::chrono::milliseconds;
    using Clock = std::chrono::steady_clock;

    struct FakeOverlay final : IOverlayHost {
        int shows = 0, hides = 0, places = 0;
        HMONITOR placedOn = nullptr;
        bool visible = false;
        void PlaceOn(HMONITOR mon) override { ++places; placedOn = mon; }
        void Show() override { ++shows; visible = true; }
        void Hide() override { ++hides; visible = false; }
    };

    struct FakeNotifier final : INotifier {
        std::vector<std::wstring> balloons;
        void ShowBalloon(const wchar_t*, const wchar_t* text) override { balloons.emplace_back(text); }
    };

    struct FakeDdc final : IDdcChannel {
        std::vector<DdcRequest> posted;
        long long busyMs = 0;
        void Post(const DdcRequest& r) override { posted.push_back(r); }
        long long BusyMs() const override { return busyMs; }
        size_t QueueDepth() const override { return 0; }
        const DdcRequest& Last() const { CHECK(!posted.empty()); return posted.back(); }
    };

    struct FakeTimers final : ITimerHost {
        std::map<UINT_PTR, UINT> active;
        void Start(UINT_PTR id, UINT intervalMs) override { active[id] = intervalMs; }
        void Stop(UINT_PTR id) override { active.erase(id); }
        bool Running(UINT_PTR id) const { return active.count(id) != 0; }
    };

    struct FakeClock final : IClock {
        Clock::time_point now = Clock::time_point{} + std::chrono::hours(1);
        bool recentInput = false;
        Clock::time_point Now() const override { return now; }
        bool LocalInputWithin(DWORD) const override { return recentInput; }
        void Advance(long long milliseconds) { now += ms(milliseconds); }
    };

    struct Animation { OverlayTarget target; ms duration; };

    // Animations never complete on their own; the test calls Complete() to run the pending
    // `done`, mirroring how Tier A's batch callback or Tier B's posted message arrives later.
    struct FakeRenderer final : IOverlayRenderer {
        std::vector<Animation> animations;
        std::function<void()> pending;
        OverlayTarget current{};
        int cancels = 0;
        int visibleSets = 0;
        bool visible = false;
        bool hasVisuals = true;

        bool Initialize(HWND, HMONITOR) override { return true; }
        void OnMonitorChanged(HMONITOR, RECT) override {}
        void AnimateTo(OverlayTarget t, ms d, std::function<void()> done) override {
            animations.push_back({ t, d });
            current = t;
            pending = std::move(done);
        }
        void Cancel() override { ++cancels; pending = nullptr; }
        OverlayTarget Current() const override { return current; }
        const wchar_t* Name() const override { return L"fake"; }
        void SetVisible(bool v) override { ++visibleSets; visible = v; }
        bool HasVisuals() const override { return hasVisuals; }

        void Complete() { auto d = std::move(pending); pending = nullptr; if (d) d(); }
        bool IsCovered(const Animation& a) const { return a.target.dim >= style::kMaxDim && a.target.blurDip >= style::kMaxBlurDip; }
        bool IsClear(const Animation& a) const { return a.target.dim == 0.0f && a.target.blurDip == 0.0f; }
    };

    struct Harness {
        FakeOverlay overlay;
        FakeNotifier notifier;
        FakeDdc ddc;
        FakeTimers timers;
        FakeClock clock;
        FakeRenderer* renderer = nullptr; // owned by the controller
        SwitchController c;

        explicit Harness(UINT revealGraceMs = 0, HMONITOR monitor = kMonitor, bool withRenderer = true,
            bool backgroundPolling = true) {
            std::unique_ptr<IOverlayRenderer> r;
            if (withRenderer) { auto fr = std::make_unique<FakeRenderer>(); renderer = fr.get(); r = std::move(fr); }
            c.Initialize({ kOur, kOther, revealGraceMs, backgroundPolling }, { &overlay, &notifier, &ddc, &timers, &clock }, std::move(r), monitor);
        }

        void Reply(UINT64 id, HRESULT hr, DWORD value) { c.OnDdcResult({ id, hr, value, 0xFF }); }
        void ReplyToLast(HRESULT hr, DWORD value) { Reply(ddc.Last().id, hr, value); }

        // Fire a poll timer and answer the read it produced.
        void Poll(UINT_PTR timerId, HRESULT hr, DWORD value) {
            const size_t before = ddc.posted.size();
            c.OnTimerTick(timerId);
            CHECK(ddc.posted.size() == before + 1);
            ReplyToLast(hr, value);
        }

        // Drive a switch from Idle up to AwayConfirmed (the other computer has the monitor).
        void GoAway() {
            c.RequestSwitch(kOther, false);
            renderer->Complete();                    // cover done -> Switching
            CHECK(c.State() == SwitchState::Switching);
            ReplyToLast(S_OK, 0);                    // write ok -> WaitTarget
            CHECK(c.State() == SwitchState::AwayConfirmedWaitTarget);
            Poll(SwitchController::kFastPollTimerId, S_OK, kOther);
            Poll(SwitchController::kFastPollTimerId, S_OK, kOther);
            CHECK(c.State() == SwitchState::AwayConfirmed);
        }
    };

    bool Is(SwitchState a, SwitchState b) { return a == b; }
}

TEST_CASE(ManualModeNeverPollsAndReleasesOverlayAfterSwitch) {
    CHECK(!SwitchController::Config{}.backgroundPolling);
    Harness h(0, kMonitor, true, false);
    CHECK(!h.timers.Running(SwitchController::kIdleWatchTimerId));
    h.c.OnTimerTick(SwitchController::kIdleWatchTimerId);
    h.c.OnMonitorLost();
    h.c.OnMonitorChanged(kMonitor, RECT{ 0, 0, 1920, 1080 });
    CHECK(!h.timers.Running(SwitchController::kIdleWatchTimerId));
    CHECK(h.ddc.posted.empty());
    for (BYTE target : { kOther, kOur }) {
        const auto before = h.ddc.posted.size();
        h.c.RequestSwitch(target, false);
        h.renderer->Complete();
        CHECK(h.ddc.posted.size() == before + 1);
        CHECK(h.ddc.Last().kind == DdcRequest::Write);
        CHECK(h.ddc.Last().value == target);
        h.ReplyToLast(S_OK, 0);
        CHECK(h.c.State() == SwitchState::Revealing);
        h.renderer->Complete();
        CHECK(h.c.State() == SwitchState::Idle);
        CHECK(!h.overlay.visible);
        for (auto timer : { SwitchController::kIdleWatchTimerId,
            SwitchController::kFastPollTimerId, SwitchController::kSlowPollTimerId }) {
            CHECK(!h.timers.Running(timer));
            h.c.OnTimerTick(timer);
        }
        CHECK(h.ddc.posted.size() == before + 1);
    }
}

TEST_CASE(InitialStateIsIdleWithIdleWatchAndHeartbeat) {
    Harness h;
    CHECK(Is(h.c.State(), SwitchState::Idle));
    CHECK(h.timers.Running(SwitchController::kIdleWatchTimerId));
    CHECK(h.timers.Running(SwitchController::kHeartbeatTimerId));
    CHECK(h.overlay.shows == 0);
}

TEST_CASE(NormalSwitchAwayAndBack) {
    Harness h;
    h.c.RequestSwitch(kOther, false);
    CHECK(Is(h.c.State(), SwitchState::Covering));
    CHECK(h.overlay.visible);
    CHECK(h.overlay.placedOn == kMonitor);
    CHECK(h.renderer->visible);
    CHECK(!h.timers.Running(SwitchController::kIdleWatchTimerId));
    CHECK(h.renderer->animations.size() == 1);
    CHECK(h.renderer->IsCovered(h.renderer->animations[0]));
    CHECK(h.renderer->animations[0].duration == style::kDuration);

    h.renderer->Complete();
    CHECK(Is(h.c.State(), SwitchState::Switching));
    CHECK(h.ddc.Last().kind == DdcRequest::Write);
    CHECK(h.ddc.Last().value == kOther);
    CHECK(h.ddc.Last().vcp == 0x60);
    CHECK(h.timers.Running(SwitchController::kDeadlineTimerId));

    h.ReplyToLast(S_OK, 0);
    CHECK(Is(h.c.State(), SwitchState::AwayConfirmedWaitTarget));
    CHECK(h.timers.Running(SwitchController::kFastPollTimerId));

    // Failed reads don't reset the match count; only a contradicting value does.
    h.Poll(SwitchController::kFastPollTimerId, S_OK, kOther);
    h.Poll(SwitchController::kFastPollTimerId, E_FAIL, 0);
    CHECK(Is(h.c.State(), SwitchState::AwayConfirmedWaitTarget));
    h.Poll(SwitchController::kFastPollTimerId, S_OK, kOther);
    CHECK(Is(h.c.State(), SwitchState::AwayConfirmed));
    CHECK(!h.timers.Running(SwitchController::kFastPollTimerId));
    CHECK(!h.timers.Running(SwitchController::kDeadlineTimerId));
    CHECK(h.timers.Running(SwitchController::kSlowPollTimerId));

    // Our input comes back: two clean matches -> reveal (grace 0).
    h.Poll(SwitchController::kSlowPollTimerId, S_OK, kOther);
    h.Poll(SwitchController::kSlowPollTimerId, S_OK, kOur);
    CHECK(Is(h.c.State(), SwitchState::AwayConfirmed));
    h.Poll(SwitchController::kSlowPollTimerId, S_OK, kOur);
    CHECK(Is(h.c.State(), SwitchState::Revealing));
    const auto& anims = h.renderer->animations;
    CHECK(anims.size() >= 2);
    CHECK(h.renderer->IsCovered(anims[anims.size() - 2]) && anims[anims.size() - 2].duration == ms(0)); // pinned first
    CHECK(h.renderer->IsClear(anims.back()) && anims.back().duration == style::kDuration);

    h.renderer->Complete();
    CHECK(Is(h.c.State(), SwitchState::Idle));
    CHECK(!h.overlay.visible);
    CHECK(!h.renderer->visible);
    CHECK(h.timers.Running(SwitchController::kIdleWatchTimerId));
    CHECK(h.timers.Running(SwitchController::kHeartbeatTimerId));
}

TEST_CASE(ToggleReadsCurrentInputThenSwitchesToTheOther) {
    Harness h;
    h.c.RequestToggleSwitch();
    CHECK(Is(h.c.State(), SwitchState::Idle));
    CHECK(h.ddc.Last().kind == DdcRequest::Read);
    h.ReplyToLast(S_OK, kOther);
    CHECK(Is(h.c.State(), SwitchState::Covering));
    h.renderer->Complete();
    CHECK(h.ddc.Last().kind == DdcRequest::Write);
    CHECK(h.ddc.Last().value == kOur);
}

TEST_CASE(ToggleReadFailureDefaultsToOtherInput) {
    Harness h;
    h.c.RequestToggleSwitch();
    h.ReplyToLast(E_FAIL, 0);
    CHECK(Is(h.c.State(), SwitchState::Covering));
    h.renderer->Complete();
    CHECK(h.ddc.Last().value == kOther);
}

TEST_CASE(ToggleDuringTransitionCancels) {
    Harness h;
    h.c.RequestSwitch(kOther, false);
    h.c.RequestToggleSwitch();
    CHECK(Is(h.c.State(), SwitchState::Revealing));
}

TEST_CASE(WriteFailureRevealsAndNotifies) {
    Harness h;
    h.c.RequestSwitch(kOther, false);
    h.renderer->Complete();
    h.ReplyToLast(E_FAIL, 0);
    CHECK(Is(h.c.State(), SwitchState::Revealing));
    CHECK(h.notifier.balloons.size() == 1);
    CHECK(h.renderer->IsClear(h.renderer->animations.back()));
    h.renderer->Complete();
    CHECK(Is(h.c.State(), SwitchState::Idle));
}

TEST_CASE(SwitchTimesOutWhileWaitingForWrite) {
    Harness h;
    h.c.RequestSwitch(kOther, false);
    h.renderer->Complete();
    CHECK(Is(h.c.State(), SwitchState::Switching));
    h.c.OnTimerTick(SwitchController::kDeadlineTimerId); // too early: deadline not reached
    CHECK(Is(h.c.State(), SwitchState::Switching));
    h.clock.Advance(SwitchController::kSwitchTimeoutMs + 1);
    h.c.OnTimerTick(SwitchController::kDeadlineTimerId);
    CHECK(Is(h.c.State(), SwitchState::Revealing));
    CHECK(h.notifier.balloons.size() == 1);
}

TEST_CASE(SwitchTimesOutWhileWaitingForTargetConfirmation) {
    Harness h;
    h.c.RequestSwitch(kOther, false);
    h.renderer->Complete();
    h.ReplyToLast(S_OK, 0);
    CHECK(Is(h.c.State(), SwitchState::AwayConfirmedWaitTarget));
    h.Poll(SwitchController::kFastPollTimerId, S_OK, kOur); // still reads our input
    h.clock.Advance(SwitchController::kSwitchTimeoutMs + 1);
    h.c.OnTimerTick(SwitchController::kDeadlineTimerId);
    CHECK(Is(h.c.State(), SwitchState::Revealing));
}

TEST_CASE(SwitchingToOurOwnInputRevealsAsSoonAsConfirmed) {
    Harness h;
    h.c.RequestSwitch(kOur, false);
    h.renderer->Complete();
    h.ReplyToLast(S_OK, 0);
    h.Poll(SwitchController::kFastPollTimerId, S_OK, kOur);
    h.Poll(SwitchController::kFastPollTimerId, S_OK, kOur);
    CHECK(Is(h.c.State(), SwitchState::Revealing));
}

TEST_CASE(ExternalSwitchDetectedFromIdlePreCoversInstantly) {
    Harness h;
    h.Poll(SwitchController::kIdleWatchTimerId, S_OK, kOther);
    h.Poll(SwitchController::kIdleWatchTimerId, E_FAIL, 0);         // failure doesn't reset
    h.Poll(SwitchController::kIdleWatchTimerId, S_OK, kOther);
    CHECK(Is(h.c.State(), SwitchState::Idle));
    h.Poll(SwitchController::kIdleWatchTimerId, S_OK, kOur);        // contradicting value resets
    h.Poll(SwitchController::kIdleWatchTimerId, S_OK, kOther);
    h.Poll(SwitchController::kIdleWatchTimerId, S_OK, kOther);
    CHECK(Is(h.c.State(), SwitchState::Idle));
    h.Poll(SwitchController::kIdleWatchTimerId, S_OK, kOther);
    CHECK(Is(h.c.State(), SwitchState::AwayConfirmed));
    CHECK(h.overlay.visible);
    CHECK(h.renderer->animations.size() == 1);
    CHECK(h.renderer->IsCovered(h.renderer->animations[0]));
    CHECK(h.renderer->animations[0].duration == ms(0));
    CHECK(h.timers.Running(SwitchController::kSlowPollTimerId));
    CHECK(!h.timers.Running(SwitchController::kIdleWatchTimerId));
}

TEST_CASE(StuckInAwayConfirmedRevealsOnLocalInputAfterTimeout) {
    Harness h;
    h.GoAway();
    h.clock.Advance(SwitchController::kAwayConfirmedStuckTimeoutMs + 1);
    h.Poll(SwitchController::kSlowPollTimerId, S_OK, kOther);       // no local input: stay
    CHECK(Is(h.c.State(), SwitchState::AwayConfirmed));
    h.clock.recentInput = true;
    h.Poll(SwitchController::kSlowPollTimerId, S_OK, kOther);
    CHECK(Is(h.c.State(), SwitchState::Revealing));
}

TEST_CASE(StuckInAwayConfirmedRevealsOnLocalInputAfterManyFailures) {
    Harness h;
    h.GoAway();
    h.clock.recentInput = true;
    for (int i = 0; i < SwitchController::kMaxConsecutiveReadFailures - 1; ++i) {
        h.Poll(SwitchController::kSlowPollTimerId, E_FAIL, 0);
        CHECK(Is(h.c.State(), SwitchState::AwayConfirmed));
    }
    h.Poll(SwitchController::kSlowPollTimerId, E_FAIL, 0);
    CHECK(Is(h.c.State(), SwitchState::Revealing));
}

TEST_CASE(RevealGraceDelaysTheReveal) {
    Harness h(500);
    h.GoAway();
    h.Poll(SwitchController::kSlowPollTimerId, S_OK, kOur);
    h.Poll(SwitchController::kSlowPollTimerId, S_OK, kOur);
    CHECK(Is(h.c.State(), SwitchState::RevealGrace));
    CHECK(h.timers.Running(SwitchController::kRevealGraceTimerId));
    CHECK(h.timers.active[SwitchController::kRevealGraceTimerId] == 500);
    CHECK(!h.timers.Running(SwitchController::kSlowPollTimerId));
    h.c.OnTimerTick(SwitchController::kRevealGraceTimerId);
    CHECK(Is(h.c.State(), SwitchState::Revealing));
    CHECK(!h.timers.Running(SwitchController::kRevealGraceTimerId));
}

TEST_CASE(SetRevealGraceTakesEffectForNextReveal) {
    Harness h;
    h.c.SetRevealGraceMs(250);
    h.GoAway();
    h.Poll(SwitchController::kSlowPollTimerId, S_OK, kOur);
    h.Poll(SwitchController::kSlowPollTimerId, S_OK, kOur);
    CHECK(Is(h.c.State(), SwitchState::RevealGrace));
}

TEST_CASE(CancelWhileIdleIsANoOp) {
    Harness h;
    h.c.RequestCancel();
    CHECK(Is(h.c.State(), SwitchState::Idle));
    CHECK(h.renderer->animations.empty());
}

TEST_CASE(CancelWhileCoveringReversesSmoothlyWithoutPinning) {
    Harness h;
    h.c.RequestSwitch(kOther, false);
    h.c.RequestCancel();
    CHECK(Is(h.c.State(), SwitchState::Revealing));
    CHECK(h.renderer->cancels == 1);
    CHECK(h.renderer->animations.size() == 2);                       // cover, then reveal; no pin
    CHECK(h.renderer->IsClear(h.renderer->animations[1]));
    CHECK(h.renderer->animations[1].duration == style::kCancelDuration);
    h.renderer->Complete();
    CHECK(Is(h.c.State(), SwitchState::Idle));
    CHECK(!h.overlay.visible);
}

TEST_CASE(CancelWhileSwitchingRevealsAndIgnoresLateWrite) {
    Harness h;
    h.c.RequestSwitch(kOther, false);
    h.renderer->Complete();
    const UINT64 writeId = h.ddc.Last().id;
    h.c.RequestCancel();
    CHECK(Is(h.c.State(), SwitchState::Revealing));
    CHECK(!h.timers.Running(SwitchController::kDeadlineTimerId));
    CHECK(h.renderer->animations.back().duration == style::kDuration);
    h.Reply(writeId, S_OK, 0);                                        // late result: no effect
    CHECK(Is(h.c.State(), SwitchState::Revealing));
}

TEST_CASE(CancelWhileWaitingForTargetReveals) {
    Harness h;
    h.c.RequestSwitch(kOther, false);
    h.renderer->Complete();
    h.ReplyToLast(S_OK, 0);
    h.c.RequestCancel();
    CHECK(Is(h.c.State(), SwitchState::Revealing));
    CHECK(!h.timers.Running(SwitchController::kFastPollTimerId));
}

TEST_CASE(CancelWhileAwayConfirmedPinsThenReveals) {
    Harness h;
    h.GoAway();
    const size_t before = h.renderer->animations.size();
    h.c.RequestCancel();
    CHECK(Is(h.c.State(), SwitchState::Revealing));
    CHECK(h.renderer->animations.size() == before + 2);
    CHECK(h.renderer->IsCovered(h.renderer->animations[before]) && h.renderer->animations[before].duration == ms(0));
    CHECK(h.renderer->IsClear(h.renderer->animations[before + 1]) && h.renderer->animations[before + 1].duration == style::kDuration);
    CHECK(!h.timers.Running(SwitchController::kSlowPollTimerId));
}

TEST_CASE(CancelDuringRevealGraceReveals) {
    Harness h(500);
    h.GoAway();
    h.Poll(SwitchController::kSlowPollTimerId, S_OK, kOur);
    h.Poll(SwitchController::kSlowPollTimerId, S_OK, kOur);
    CHECK(Is(h.c.State(), SwitchState::RevealGrace));
    h.c.RequestCancel();
    CHECK(Is(h.c.State(), SwitchState::Revealing));
    CHECK(!h.timers.Running(SwitchController::kRevealGraceTimerId));
    h.c.OnTimerTick(SwitchController::kRevealGraceTimerId);          // stale tick: ignored
    CHECK(Is(h.c.State(), SwitchState::Revealing));
}

TEST_CASE(CancelWhileRevealingIsIgnored) {
    Harness h;
    h.c.RequestSwitch(kOther, false);
    h.c.RequestCancel();
    const size_t before = h.renderer->animations.size();
    h.c.RequestCancel();
    CHECK(Is(h.c.State(), SwitchState::Revealing));
    CHECK(h.renderer->animations.size() == before);
    h.renderer->Complete();
    CHECK(Is(h.c.State(), SwitchState::Idle));
}

TEST_CASE(RedirectMidCoverContinuesWithoutSnapping) {
    Harness h;
    h.c.RequestSwitch(kOther, false);
    h.c.RequestSwitch(kOur, false);
    CHECK(Is(h.c.State(), SwitchState::Covering));
    CHECK(h.renderer->cancels == 1);
    CHECK(h.renderer->animations.size() == 2);                       // no instant reveal/pin in between
    CHECK(h.renderer->IsCovered(h.renderer->animations[1]));
    h.renderer->Complete();
    CHECK(Is(h.c.State(), SwitchState::Switching));
    CHECK(h.ddc.Last().value == kOur);
}

TEST_CASE(RedirectWhileSwitchingDropsTheOldWrite) {
    Harness h;
    h.c.RequestSwitch(kOther, false);
    h.renderer->Complete();
    const UINT64 oldWrite = h.ddc.Last().id;
    h.c.RequestSwitch(kOur, false);
    CHECK(Is(h.c.State(), SwitchState::Covering));
    CHECK(!h.timers.Running(SwitchController::kDeadlineTimerId));
    h.Reply(oldWrite, S_OK, 0);                                       // stale: must not advance state
    CHECK(Is(h.c.State(), SwitchState::Covering));
    h.renderer->Complete();
    CHECK(Is(h.c.State(), SwitchState::Switching));
    CHECK(h.ddc.Last().value == kOur);
}

TEST_CASE(MonitorLostPausesPollingAndRefusesSwitches) {
    Harness h;
    h.c.OnMonitorLost();
    CHECK(!h.c.MonitorPresent());
    CHECK(!h.timers.Running(SwitchController::kIdleWatchTimerId));
    const size_t posted = h.ddc.posted.size();
    h.c.OnTimerTick(SwitchController::kIdleWatchTimerId);            // stale tick posts nothing
    CHECK(h.ddc.posted.size() == posted);
    h.c.RequestSwitch(kOther, false);
    CHECK(Is(h.c.State(), SwitchState::Idle));
    CHECK(h.notifier.balloons.size() == 1);
    h.c.RequestToggleSwitch();
    CHECK(h.ddc.posted.size() == posted);
    CHECK(h.notifier.balloons.size() == 2);

    h.c.OnMonitorChanged(kMonitor2, RECT{ 0, 0, 100, 100 });
    CHECK(h.c.MonitorPresent());
    CHECK(h.c.Monitor() == kMonitor2);
    CHECK(h.timers.Running(SwitchController::kIdleWatchTimerId));
    h.c.RequestSwitch(kOther, false);
    CHECK(Is(h.c.State(), SwitchState::Covering));
    CHECK(h.overlay.placedOn == kMonitor2);
}

TEST_CASE(MonitorLostWhileAwayResumesSlowPollingOnReturn) {
    Harness h;
    h.GoAway();
    h.c.OnMonitorLost();
    CHECK(Is(h.c.State(), SwitchState::AwayConfirmed));
    CHECK(!h.timers.Running(SwitchController::kSlowPollTimerId));
    CHECK(h.overlay.visible);                                         // stays covered
    h.c.OnMonitorChanged(kMonitor, RECT{});
    CHECK(h.timers.Running(SwitchController::kSlowPollTimerId));
    CHECK(h.overlay.places >= 2);                                     // re-placed on return
}

TEST_CASE(StartingWithoutAMonitorWaitsForIt) {
    Harness h(0, nullptr);
    CHECK(!h.c.MonitorPresent());
    CHECK(!h.timers.Running(SwitchController::kIdleWatchTimerId));
    CHECK(h.timers.Running(SwitchController::kHeartbeatTimerId));
    h.c.OnMonitorChanged(kMonitor, RECT{});
    CHECK(h.timers.Running(SwitchController::kIdleWatchTimerId));
}

TEST_CASE(OutstandingPollIsAbandonedAfterTimeout) {
    Harness h;
    h.c.OnTimerTick(SwitchController::kIdleWatchTimerId);
    const UINT64 first = h.ddc.Last().id;
    h.c.OnTimerTick(SwitchController::kIdleWatchTimerId);            // still pending: nothing new
    CHECK(h.ddc.posted.size() == 1);
    h.clock.Advance(SwitchController::kPendingPollTimeoutMs + 1);
    h.c.OnTimerTick(SwitchController::kIdleWatchTimerId);
    CHECK(h.ddc.posted.size() == 2);
    CHECK(h.ddc.Last().id != first);
    h.Reply(first, S_OK, kOther);                                     // late reply: dropped
    CHECK(Is(h.c.State(), SwitchState::Idle));
    h.ReplyToLast(S_OK, kOur);
    h.c.OnTimerTick(SwitchController::kIdleWatchTimerId);            // pending cleared: polls again
    CHECK(h.ddc.posted.size() == 3);
}

TEST_CASE(HeartbeatSurvivesAFullCycleAndDoesNotChangeState) {
    Harness h;
    h.ddc.busyMs = SwitchController::kWorkerStuckWarnMs + 1;
    h.c.OnTimerTick(SwitchController::kHeartbeatTimerId);
    CHECK(Is(h.c.State(), SwitchState::Idle));
    h.GoAway();
    h.c.OnTimerTick(SwitchController::kHeartbeatTimerId);
    CHECK(Is(h.c.State(), SwitchState::AwayConfirmed));
    CHECK(h.timers.Running(SwitchController::kHeartbeatTimerId));
    h.c.RequestCancel();
    h.renderer->Complete();
    CHECK(Is(h.c.State(), SwitchState::Idle));
    CHECK(h.timers.Running(SwitchController::kHeartbeatTimerId));
}

TEST_CASE(WithoutARendererTheSwitchStillHappensAndNoOverlayIsShown) {
    Harness h(0, kMonitor, false);
    CHECK(!h.c.Renderer().HasVisuals());
    h.c.RequestSwitch(kOther, false);
    CHECK(Is(h.c.State(), SwitchState::Switching));                   // cover completed synchronously
    CHECK(h.overlay.shows == 0);
    CHECK(h.ddc.Last().kind == DdcRequest::Write);
    h.ReplyToLast(S_OK, 0);
    h.Poll(SwitchController::kFastPollTimerId, S_OK, kOther);
    h.Poll(SwitchController::kFastPollTimerId, S_OK, kOther);
    CHECK(Is(h.c.State(), SwitchState::AwayConfirmed));
    h.Poll(SwitchController::kSlowPollTimerId, S_OK, kOur);
    h.Poll(SwitchController::kSlowPollTimerId, S_OK, kOur);
    CHECK(Is(h.c.State(), SwitchState::Idle));                        // reveal completed synchronously
    CHECK(h.overlay.hides == 0);
    h.c.RequestCancel();                                              // no crash, no-op
    CHECK(Is(h.c.State(), SwitchState::Idle));
}

TEST_CASE(ReplaceRendererWithNullFallsBackAndBack) {
    Harness h;
    h.c.ReplaceRenderer(nullptr);
    CHECK(!h.c.Renderer().HasVisuals());
    auto fr = std::make_unique<FakeRenderer>();
    FakeRenderer* raw = fr.get();
    h.c.ReplaceRenderer(std::move(fr));
    CHECK(&h.c.Renderer() == raw);
    h.c.RequestSwitch(kOther, false);
    CHECK(raw->animations.size() == 1);
}

TEST_CASE(UnrecognizedResultIsIgnored) {
    Harness h;
    h.Reply(999, S_OK, kOther);
    CHECK(Is(h.c.State(), SwitchState::Idle));
    CHECK(h.renderer->animations.empty());
}

int main() {
    std::printf("SwitchController tests\n");
    return RunAllTests();
}
