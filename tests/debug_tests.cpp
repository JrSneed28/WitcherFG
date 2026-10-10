#include "support.hpp"
#include "fixtures/verified_identity.hpp"
#include "wfg/debug_session.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <type_traits>

namespace {
using namespace wfg;
Error DebugError() { return {ErrorCode::DebugFailure, Stage::Debug, "injected_debug_failure"}; }
void IgnoreHandle(void*) noexcept {}
class Backend final : public IDebugBackend {
public:
  std::chrono::steady_clock::time_point Now() const override { const auto snapshot = now; now += now_step; return snapshot; }
  Result<Unit> Attach(const OwnedProcess&) override { calls.push_back("attach"); return denied ? Result<Unit>{DebugError()} : Result<Unit>{Unit{}}; }
  Result<Unit> SetKillOnExit(bool enabled) override {
    calls.push_back(enabled ? "true" : "false");
    if (fail_policy) return DebugError();
    kill = enabled;
    return Unit{};
  }
  Result<std::optional<DebugEvent>> Wait(std::chrono::milliseconds slice) override {
    calls.push_back("wait"); last_slice = slice; max_slice = std::max(max_slice, slice); now += slice;
    if (fail_wait) return DebugError();
    if (events.empty()) return std::optional<DebugEvent>{};
    auto event = events.front(); events.pop_front(); return std::optional<DebugEvent>{event};
  }
  Result<Unit> Continue(const DebugEvent& event, Continuation continuation) override {
    calls.push_back("continue");
    if (fail_continue) return DebugError();
    continued.push_back({event, continuation}); return Unit{};
  }
  Result<Unit> Detach() override { calls.push_back("detach"); if (detach_failures-- > 0) return DebugError(); detached = true; return Unit{}; }
  Result<Unit> AbortOwnedStartup() override { calls.push_back("abort"); if (fail_abort) return DebugError(); aborted = true; return Unit{}; }
  mutable std::chrono::steady_clock::time_point now{};
  std::chrono::milliseconds now_step{};
  std::chrono::milliseconds last_slice{};
  std::chrono::milliseconds max_slice{};
  std::vector<std::string> calls;
  std::deque<DebugEvent> events;
  std::vector<std::pair<DebugEvent, Continuation>> continued;
  bool denied{}, fail_policy{}, fail_wait{}, fail_continue{}, detached{}, aborted{}, kill{true};
  bool fail_abort{};
  int detach_failures{};
};
DebugEvent Event(DebugEventKind kind = DebugEventKind::Other) { return {kind, 42, 7}; }
DebugEvent Exception(bool attach = false, std::uint32_t code = 0x80000003U, bool first = true) {
  auto event = Event(DebugEventKind::Exception);
  event.exception_status = code; event.exception_address = 0x1234; event.first_chance = first; event.attach_breakpoint_identity = attach;
  return event;
}
struct Fixture {
  int handle{};
  OwnedProcess owned = VerifiedIdentityTestFactory::Owned(&handle, IgnoreHandle);
  Backend backend;
  DebugSession session{backend};
  Deadline deadline() const { return {backend.Now() + std::chrono::milliseconds(250)}; }
  DebugStop Stop() {
    auto result = session.WaitStopped(deadline()); CHECK(IsOk(result));
    return std::move(std::get<DebugStop>(result));
  }
};
}

TEST("debug.attach_sets_false_before_any_inspection_and_scope_exit_detaches") {
  Backend backend;
  int handle{};
  auto owned = VerifiedIdentityTestFactory::Owned(&handle, IgnoreHandle);
  {
    DebugSession session(backend);
    REQUIRE_OK(session.Attach(owned));
    CHECK((backend.calls == std::vector<std::string>{"attach", "false"}));
    CHECK(!backend.kill);
  }
  CHECK(backend.detached);
  CHECK(!backend.aborted);
}
TEST("debug.first_false_failure_aborts_and_never_exposes_stop") {
  Fixture fixture; fixture.backend.fail_policy = true;
  REQUIRE_ERROR(fixture.session.Attach(fixture.owned), ErrorCode::DebugFailure);
  CHECK(fixture.backend.aborted);
  REQUIRE_ERROR(fixture.session.WaitStopped(fixture.deadline()), ErrorCode::DebugFailure);
}
TEST("debug.attach_denial_does_not_change_policy_or_abort") {
  Fixture fixture; fixture.backend.denied = true;
  REQUIRE_ERROR(fixture.session.Attach(fixture.owned), ErrorCode::DebugFailure);
  CHECK((fixture.backend.calls == std::vector<std::string>{"attach"}));
}
TEST("debug.only_identified_initial_attach_breakpoint_can_be_handled") {
  Fixture fixture; REQUIRE_OK(fixture.session.Attach(fixture.owned));
  fixture.backend.events = {Event(DebugEventKind::ProcessCreated), Exception(true), Exception(true), Exception(false), Exception(false, 0xC0000005U)};
  auto created = fixture.Stop(); REQUIRE_OK(fixture.session.Continue(created, Continuation::Handled));
  auto initial = fixture.Stop(); CHECK(initial.continuation() == Continuation::Handled);
  REQUIRE_OK(fixture.session.Continue(initial, Continuation::Handled));
  for (int index = 0; index < 3; ++index) {
    auto fault = fixture.Stop(); CHECK(fault.continuation() == Continuation::NotHandled);
    REQUIRE_ERROR(fixture.session.Continue(fault, Continuation::Handled), ErrorCode::DebugFailure);
    REQUIRE_OK(fixture.session.Continue(fault, Continuation::NotHandled));
  }
}
TEST("debug.breakpoint_requires_choreography_code_and_first_chance") {
  for (int variant = 0; variant < 3; ++variant) {
    Fixture fixture; REQUIRE_OK(fixture.session.Attach(fixture.owned));
    if (variant != 0) {
      fixture.backend.events.push_back(Event(DebugEventKind::ProcessCreated));
      auto created = fixture.Stop(); REQUIRE_OK(fixture.session.Continue(created, Continuation::Handled));
    }
    fixture.backend.events.push_back(Exception(true, variant == 1 ? 0xC0000005U : 0x80000003U, variant != 2));
    auto stop = fixture.Stop(); CHECK(stop.continuation() == Continuation::NotHandled);
  }
}
TEST("debug.token_move_single_use_foreign_and_outliving_session") {
  Fixture fixture, foreign; REQUIRE_OK(fixture.session.Attach(fixture.owned));
  REQUIRE_OK(foreign.session.Attach(foreign.owned)); fixture.backend.events.push_back(Event());
  auto original = fixture.Stop(); auto moved = std::move(original);
  CHECK(!original.active()); CHECK(moved.active());
  REQUIRE_ERROR(foreign.session.Continue(moved, Continuation::Handled), ErrorCode::DebugFailure);
  REQUIRE_OK(fixture.session.Continue(moved, Continuation::Handled)); CHECK(!moved.active());
  REQUIRE_ERROR(fixture.session.Continue(moved, Continuation::Handled), ErrorCode::DebugFailure);
  std::optional<DebugStop> survivor;
  Backend backend; int handle{}; auto owned = VerifiedIdentityTestFactory::Owned(&handle, IgnoreHandle);
  { DebugSession session(backend); REQUIRE_OK(session.Attach(owned)); backend.events.push_back(Event());
    auto result = session.WaitStopped({backend.Now() + std::chrono::seconds(1)}); CHECK(IsOk(result)); survivor.emplace(std::move(std::get<DebugStop>(result))); }
  CHECK(!survivor->active()); CHECK(backend.detached); CHECK(backend.continued.size() == 1);
}
TEST("debug.owner_thread_rejects_every_operation_without_backend_calls") {
  Fixture fixture; REQUIRE_OK(fixture.session.Attach(fixture.owned)); fixture.backend.events.push_back(Event());
  auto stop = fixture.Stop(); const auto before = fixture.backend.calls.size();
  bool rejected{};
  std::thread worker([&] {
    rejected = !IsOk(fixture.session.Attach(fixture.owned)) && !IsOk(fixture.session.WaitStopped(fixture.deadline())) &&
      !IsOk(fixture.session.SetKillOnExit(true)) && !IsOk(fixture.session.Continue(stop, Continuation::Handled)) &&
      !IsOk(fixture.session.Detach()) && !IsOk(fixture.session.AbortOwnedStartup());
  }); worker.join(); CHECK(rejected); CHECK(fixture.backend.calls.size() == before);
}
TEST("debug.wait_deadline_is_sliced_and_keeps_inspection_unmodified") {
  Fixture fixture; REQUIRE_OK(fixture.session.Attach(fixture.owned));
  REQUIRE_ERROR(fixture.session.WaitStopped(fixture.deadline()), ErrorCode::TimedOut);
  CHECK(fixture.backend.max_slice <= std::chrono::milliseconds(50)); CHECK(!fixture.backend.kill);
  CHECK(fixture.backend.now == std::chrono::steady_clock::time_point{} + std::chrono::milliseconds(250));
}
TEST("debug.held_event_blocks_wait_and_failed_continue_preserves_token") {
  Fixture fixture; REQUIRE_OK(fixture.session.Attach(fixture.owned)); fixture.backend.events.push_back(Event());
  auto stop = fixture.Stop(); REQUIRE_ERROR(fixture.session.WaitStopped(fixture.deadline()), ErrorCode::DebugFailure);
  fixture.backend.fail_continue = true; REQUIRE_ERROR(fixture.session.Continue(stop, Continuation::Handled), ErrorCode::DebugFailure);
  CHECK(stop.active()); fixture.backend.fail_continue = false; REQUIRE_OK(fixture.session.Continue(stop, Continuation::Handled));
}
TEST("debug.mutation_policy_blocks_resume_and_detach_until_false") {
  Fixture fixture; REQUIRE_OK(fixture.session.Attach(fixture.owned));
  REQUIRE_ERROR(fixture.session.SetKillOnExit(true), ErrorCode::DebugFailure);
  fixture.backend.events.push_back(Event()); auto stop = fixture.Stop(); REQUIRE_OK(fixture.session.SetKillOnExit(true));
  REQUIRE_ERROR(fixture.session.Continue(stop, Continuation::Handled), ErrorCode::DebugFailure);
  REQUIRE_ERROR(fixture.session.Detach(), ErrorCode::DebugFailure);
  fixture.backend.fail_policy = true; REQUIRE_ERROR(fixture.session.SetKillOnExit(false), ErrorCode::DebugFailure);
  CHECK(fixture.backend.kill); REQUIRE_OK(fixture.session.AbortOwnedStartup()); CHECK(fixture.backend.aborted);
}
TEST("debug.process_exit_is_continued_and_invalidates_session") {
  Fixture fixture; REQUIRE_OK(fixture.session.Attach(fixture.owned)); fixture.backend.events.push_back(Event(DebugEventKind::ProcessExited));
  REQUIRE_ERROR(fixture.session.WaitStopped(fixture.deadline()), ErrorCode::NoMatchingProcess);
  CHECK(fixture.backend.continued.size() == 1); REQUIRE_ERROR(fixture.session.SetKillOnExit(true), ErrorCode::DebugFailure);
}
TEST("debug.detach_failure_services_events_then_retries_with_bound") {
  Fixture fixture; REQUIRE_OK(fixture.session.Attach(fixture.owned)); fixture.backend.detach_failures = 2;
  fixture.backend.events.push_back(Exception()); REQUIRE_OK(fixture.session.Detach(fixture.deadline()));
  CHECK(fixture.backend.detached); CHECK(fixture.backend.continued.size() == 1);
  CHECK(fixture.backend.continued.front().second == Continuation::NotHandled);
  CHECK(fixture.backend.max_slice <= std::chrono::milliseconds(50));
}
TEST("debug.persistent_detach_failure_returns_timeout_and_can_retry") {
  Fixture fixture; REQUIRE_OK(fixture.session.Attach(fixture.owned)); fixture.backend.detach_failures = 1000;
  REQUIRE_ERROR(fixture.session.Detach(fixture.deadline()), ErrorCode::TimedOut);
  CHECK(!fixture.backend.detached); fixture.backend.detach_failures = 0; REQUIRE_OK(fixture.session.Detach());
}
TEST("debug.cancellation_while_stopped_releases_on_owner_with_safe_continuation") {
  Backend backend; std::stop_source source; int handle{}; auto owned = VerifiedIdentityTestFactory::Owned(&handle, IgnoreHandle);
  DebugSession session(backend, source.get_token()); REQUIRE_OK(session.Attach(owned)); backend.events.push_back(Exception());
  auto result = session.WaitStopped({backend.Now() + std::chrono::seconds(1)}); CHECK(IsOk(result));
  auto stop = std::move(std::get<DebugStop>(result)); source.request_stop(); REQUIRE_OK(session.Detach());
  CHECK(!stop.active()); CHECK(backend.continued.front().second == Continuation::NotHandled); CHECK(!backend.aborted);
}
TEST("debug.cancellation_before_wait_never_enters_backend") {
  Backend backend; std::stop_source source; int handle{}; auto owned = VerifiedIdentityTestFactory::Owned(&handle, IgnoreHandle);
  DebugSession session(backend, source.get_token()); REQUIRE_OK(session.Attach(owned)); source.request_stop();
  REQUIRE_ERROR(session.WaitStopped({backend.Now() + std::chrono::seconds(1)}), ErrorCode::TimedOut);
  CHECK(backend.calls.size() == 2);
}
TEST("debug.mutation_scope_exit_aborts_instead_of_resuming") {
  Backend backend; int handle{}; auto owned = VerifiedIdentityTestFactory::Owned(&handle, IgnoreHandle);
  { DebugSession session(backend); REQUIRE_OK(session.Attach(owned)); backend.events.push_back(Event());
    auto result = session.WaitStopped({backend.Now() + std::chrono::seconds(1)}); CHECK(IsOk(result)); REQUIRE_OK(session.SetKillOnExit(true)); }
  CHECK(backend.aborted); CHECK(backend.continued.empty()); CHECK(!backend.detached);
}
TEST("debug.production_api_has_no_pid_attach_or_public_stop_constructor") {
  CHECK(!std::is_default_constructible_v<DebugStop>); CHECK(!std::is_copy_constructible_v<DebugStop>);
  CHECK((!std::is_invocable_v<decltype(&DebugSession::Attach), DebugSession&, const ObservedProcess&>));
  NativeDebugBackend backend; DebugSession session(backend);
  REQUIRE_ERROR(session.SetKillOnExit(false), ErrorCode::DebugFailure);
}

TEST("debug.cancellation_refuses_new_mutation_but_permits_cleanup") {
  Backend backend; std::stop_source source; int handle{}; auto owned = VerifiedIdentityTestFactory::Owned(&handle, IgnoreHandle);
  DebugSession session(backend, source.get_token()); REQUIRE_OK(session.Attach(owned)); backend.events.push_back(Event());
  auto waited = session.WaitStopped({backend.Now() + std::chrono::seconds(1)}); CHECK(IsOk(waited));
  source.request_stop(); REQUIRE_ERROR(session.SetKillOnExit(true), ErrorCode::TimedOut);
  CHECK(!backend.kill); REQUIRE_OK(session.Detach());
}
TEST("debug.deadline_expired_detach_does_not_resume_held_event") {
  Fixture fixture; REQUIRE_OK(fixture.session.Attach(fixture.owned)); fixture.backend.events.push_back(Event());
  auto stop = fixture.Stop(); REQUIRE_ERROR(fixture.session.Detach({fixture.backend.Now()}), ErrorCode::TimedOut);
  CHECK(stop.active()); CHECK(fixture.backend.continued.empty()); REQUIRE_OK(fixture.session.Detach());
}
TEST("debug.wait_error_preserves_safe_policy_and_allows_detach") {
  Fixture fixture; REQUIRE_OK(fixture.session.Attach(fixture.owned)); fixture.backend.fail_wait = true;
  REQUIRE_ERROR(fixture.session.WaitStopped(fixture.deadline()), ErrorCode::DebugFailure);
  CHECK(!fixture.backend.kill); REQUIRE_OK(fixture.session.Detach());
}
TEST("debug.foreign_event_never_continues_or_aborts_unrelated_target") {
  Fixture fixture; REQUIRE_OK(fixture.session.Attach(fixture.owned)); auto event = Event(); event.pid = 99;
  fixture.backend.events.push_back(event); REQUIRE_ERROR(fixture.session.WaitStopped(fixture.deadline()), ErrorCode::IdentityChanged);
  CHECK(fixture.backend.continued.empty()); CHECK(!fixture.backend.aborted);
}
TEST("debug.non_owner_native_operations_fail_before_any_win32_call") {
  NativeDebugBackend backend; int handle{}; auto owned = VerifiedIdentityTestFactory::Owned(&handle, IgnoreHandle);
  bool rejected{};
  std::thread worker([&] {
    rejected = !IsOk(backend.Attach(owned)) && !IsOk(backend.SetKillOnExit(false)) &&
      !IsOk(backend.Wait(std::chrono::milliseconds(50))) && !IsOk(backend.Continue(Event(), Continuation::Handled)) &&
      !IsOk(backend.Detach()) && !IsOk(backend.AbortOwnedStartup());
  }); worker.join(); CHECK(rejected);
}
namespace {
class WatchdogFixture final : public IWatchdogBackend {
public:
  explicit WatchdogFixture(std::stop_source& cancellation) : cancellation_(cancellation) {}
  bool WaitUntil(Deadline deadline, std::stop_token disarmed) override {
    std::unique_lock lock(mutex_);
    deadlines.push_back(deadline.value);
    if (deadlines.size() == 2) cancelled_before_grace = cancellation_.stop_requested();
    condition_.notify_all();
    condition_.wait(lock, disarmed, [&] { return permits > 0; });
    if (disarmed.stop_requested()) { lock.unlock(); std::this_thread::sleep_for(stop_delay); return false; }
    --permits;
    return true;
  }
  void ExitHelper() noexcept override { std::lock_guard lock(mutex_); ++exits; condition_.notify_all(); }
  bool Await(unsigned count, bool exited = false) {
    std::unique_lock lock(mutex_);
    return condition_.wait_for(lock, std::chrono::seconds(2), [&] { return deadlines.size() >= count && (!exited || exits > 0); });
  }
  void Advance() { std::lock_guard lock(mutex_); ++permits; condition_.notify_all(); }
  std::mutex mutex_;
  std::condition_variable_any condition_;
  std::stop_source& cancellation_;
  std::vector<std::chrono::steady_clock::time_point> deadlines;
  unsigned permits{}, exits{};
  std::chrono::milliseconds stop_delay{};
  bool cancelled_before_grace{};
};
}
TEST("debug.watchdog_cancels_then_exits_only_helper_after_two_second_grace") {
  std::stop_source cancellation; WatchdogFixture backend(cancellation);
  Deadline deadline{std::chrono::steady_clock::time_point{} + std::chrono::seconds(60)};
  DeadlineWatchdog watchdog(deadline, cancellation, backend); CHECK(backend.Await(1));
  CHECK(!cancellation.stop_requested()); backend.Advance(); CHECK(backend.Await(2));
  CHECK(cancellation.stop_requested());
  { std::lock_guard lock(backend.mutex_); CHECK(backend.cancelled_before_grace); CHECK(backend.exits == 0);
    CHECK(backend.deadlines[0] == deadline.value); CHECK(backend.deadlines[1] == deadline.value + std::chrono::seconds(2)); }
  backend.Advance(); CHECK(backend.Await(2, true)); watchdog.Disarm(); CHECK(backend.exits == 1);
}
TEST("debug.watchdog_disarm_before_deadline_never_cancels_or_exits") {
  std::stop_source cancellation; WatchdogFixture backend(cancellation);
  DeadlineWatchdog watchdog({std::chrono::steady_clock::time_point{}}, cancellation, backend);
  CHECK(backend.Await(1)); watchdog.Disarm(); watchdog.Disarm();
  CHECK(!cancellation.stop_requested()); CHECK(backend.exits == 0);
}
TEST("debug.watchdog_disarm_during_grace_does_not_exit_helper") {
  std::stop_source cancellation; WatchdogFixture backend(cancellation);
  DeadlineWatchdog watchdog({std::chrono::steady_clock::time_point{}}, cancellation, backend);
  CHECK(backend.Await(1)); backend.Advance(); CHECK(backend.Await(2)); watchdog.Disarm();
  CHECK(cancellation.stop_requested()); CHECK(backend.exits == 0);
}
TEST("debug.attach_breakpoint_window_closes_on_first_unrelated_exception") {
  Fixture fixture; REQUIRE_OK(fixture.session.Attach(fixture.owned));
  fixture.backend.events.push_back(Event(DebugEventKind::ProcessCreated)); auto created = fixture.Stop();
  REQUIRE_OK(fixture.session.Continue(created, Continuation::Handled));
  fixture.backend.events.push_back(Exception(false)); auto application = fixture.Stop();
  REQUIRE_OK(fixture.session.Continue(application, Continuation::NotHandled));
  fixture.backend.events.push_back(Exception(true)); auto later = fixture.Stop();
  CHECK(later.continuation() == Continuation::NotHandled);
}
TEST("debug.deadline_crossed_between_clock_reads_never_yields_negative_wait") {
  Fixture fixture; REQUIRE_OK(fixture.session.Attach(fixture.owned)); fixture.backend.now_step = std::chrono::milliseconds(50);
  REQUIRE_ERROR(fixture.session.WaitStopped({std::chrono::steady_clock::time_point{} + std::chrono::milliseconds(25)}), ErrorCode::TimedOut);
  CHECK(fixture.backend.last_slice >= std::chrono::milliseconds(0));
}
TEST("debug.held_capability_binds_exact_session_and_checks_without_continuing") {
  Fixture fixture, foreign; REQUIRE_OK(fixture.session.Attach(fixture.owned)); REQUIRE_OK(foreign.session.Attach(foreign.owned));
  fixture.backend.events.push_back(Event()); auto stop = fixture.Stop(); const auto before = fixture.backend.calls.size();
  CHECK(fixture.session.Holds(stop)); CHECK(!foreign.session.Holds(stop)); CHECK(fixture.backend.calls.size() == before);
  bool worker_held{true}; std::thread worker([&] { worker_held = fixture.session.Holds(stop); }); worker.join(); CHECK(!worker_held);
  auto moved = std::move(stop); CHECK(!fixture.session.Holds(stop)); CHECK(fixture.session.Holds(moved));
  REQUIRE_OK(fixture.session.Continue(moved, Continuation::Handled)); CHECK(!fixture.session.Holds(moved));
}
TEST("debug.failed_abort_retains_session_capability_and_refuses_false_resume_or_detach") {
  Fixture fixture; REQUIRE_OK(fixture.session.Attach(fixture.owned)); fixture.backend.events.push_back(Event()); auto stop = fixture.Stop();
  REQUIRE_OK(fixture.session.SetKillOnExit(true)); fixture.backend.fail_abort = true;
  REQUIRE_ERROR(fixture.session.AbortOwnedStartup(), ErrorCode::DebugFailure);
  CHECK(stop.active()); CHECK(fixture.session.Holds(stop)); CHECK(fixture.backend.kill);
  const auto before = fixture.backend.calls.size();
  REQUIRE_ERROR(fixture.session.SetKillOnExit(false), ErrorCode::DebugFailure);
  REQUIRE_ERROR(fixture.session.Continue(stop, Continuation::Handled), ErrorCode::DebugFailure);
  REQUIRE_ERROR(fixture.session.Detach(), ErrorCode::DebugFailure);
  CHECK(fixture.backend.calls.size() == before); CHECK(fixture.backend.continued.empty());
}

TEST("debug.safety_accessor_distinguishes_failed_attach_and_verified_exit") {
  static_assert(noexcept(std::declval<const DebugSession&>().Safety()));
  {
    Fixture fixture;CHECK(fixture.session.Safety()==DebugSafety::NotAttached);
    REQUIRE_OK(fixture.session.Attach(fixture.owned));CHECK(fixture.session.Safety()==DebugSafety::AttachedSafe);
    REQUIRE_OK(fixture.session.AbortOwnedStartup());CHECK(fixture.session.Safety()==DebugSafety::Exited);
  }
  {
    Fixture fixture;fixture.backend.fail_policy=true;fixture.backend.fail_abort=true;
    REQUIRE_ERROR(fixture.session.Attach(fixture.owned),ErrorCode::DebugFailure);
    CHECK(fixture.session.Safety()==DebugSafety::AttachedUnverified);
  }
  {
    Fixture fixture;fixture.backend.denied=true;
    REQUIRE_ERROR(fixture.session.Attach(fixture.owned),ErrorCode::DebugFailure);
    CHECK(fixture.session.Safety()==DebugSafety::NotAttached);
  }
}
TEST("debug.watchdog_external_cancel_interrupts_deadline_wait") {
  std::stop_source cancellation, external;WatchdogFixture backend(cancellation);
  const auto before=std::chrono::steady_clock::now();
  DeadlineWatchdog watchdog({before+std::chrono::seconds(60)},cancellation,backend,external.get_token());
  CHECK(backend.Await(1));external.request_stop();CHECK(backend.Await(2));
  CHECK(cancellation.stop_requested());
  {std::lock_guard lock(backend.mutex_);CHECK(backend.deadlines[1]<=std::chrono::steady_clock::now()+std::chrono::seconds(2));}
  backend.Advance();CHECK(backend.Await(2,true));watchdog.Disarm();
}

TEST("debug.watchdog_grace_is_measured_from_request_not_worker_wakeup") {
  std::stop_source cancellation,external;WatchdogFixture backend(cancellation);
  backend.stop_delay=std::chrono::milliseconds(30);
  DeadlineWatchdog watchdog({std::chrono::steady_clock::now()+std::chrono::seconds(60)},cancellation,backend,external.get_token());
  CHECK(backend.Await(1));external.request_stop();const auto requested=std::chrono::steady_clock::now();
  CHECK(backend.Await(2));
  {std::lock_guard lock(backend.mutex_);CHECK(backend.deadlines[1]<=requested+std::chrono::seconds(2));}
  watchdog.Disarm();
}
