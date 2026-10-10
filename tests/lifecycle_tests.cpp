#include "fixtures/lifecycle.hpp"
#include "support.hpp"
using namespace lifecycle;
void Ready(const Observation &o) {
  CHECK(o.ready);
  CHECK(o.helper_exited);
  CHECK(o.sentinel_alive);
  CHECK(o.total_ms > 0 && o.total_ms <= 10000);
}
void Originals(const Observation &o) {
  Ready(o);
  CHECK(o.target_alive);
  CHECK(o.ran_originals);
  CHECK(o.protections_restored);
  CHECK(!o.mixed);
}
void Replacements(const Observation &o) {
  Ready(o);
  CHECK(o.target_alive);
  CHECK(o.ran_replacements);
  CHECK(o.protections_restored);
  CHECK(!o.mixed);
}
TEST("lifecycle.inspection_false_exit_resumes_originals") {
  for (auto c : {Case::InspectExit, Case::InspectThreadExit}) {
    auto o = RunFixtureCase(c);
    Originals(o);
    CHECK(o.held);
    CHECK(!o.armed);
    if (c == Case::InspectThreadExit)
      CHECK(o.thread_exit_resumed);
  }
}
TEST("lifecycle.death_after_each_real_write_cannot_resume_mixed") {
  unsigned n = 0;
  for (auto c : {Case::Die1, Case::Die2, Case::Die3, Case::Die4}) {
    auto o = RunFixtureCase(c);
    Ready(o);
    CHECK(o.held);
    CHECK(o.armed);
    CHECK(o.writes == ++n);
    CHECK(o.write_values_verified);
    CHECK(o.controller_write_checks == n);
    CHECK(o.target_exited);
    CHECK(!o.mixed);
    CHECK(!o.ran_originals && !o.ran_replacements);
  }
}
TEST("lifecycle.verified_commit_survives_helper_exit") {
  auto o = RunFixtureCase(Case::Commit);
  Replacements(o);
  CHECK(o.writes == 4);
  CHECK(o.detached);
}
TEST("lifecycle.verified_rollback_resumes_originals") {
  auto o = RunFixtureCase(Case::Rollback);
  Originals(o);
  CHECK(o.writes >= 2);
  CHECK(o.armed);
  CHECK(o.detached);
}
TEST("lifecycle.failed_rollback_contains_owned_target") {
  auto o = RunFixtureCase(Case::Abort);
  Ready(o);
  CHECK(o.armed);
  CHECK(o.target_exited);
  CHECK(o.unsafe_quiet);
  CHECK(!o.mixed);
}
TEST("lifecycle.native_termination_denial_keeps_true_until_helper_death") {
  auto o = RunFixtureCase(Case::TerminationDenied);
  Ready(o);
  CHECK(o.armed);
  CHECK(o.abort_denied);
  CHECK(o.alive_before_death);
  CHECK(o.target_exited);
  CHECK(o.unsafe_quiet);
}
TEST("lifecycle.initial_false_policy_failure_exits_before_diagnostics") {
  auto o = RunFixtureCase(Case::InitialPolicyFailure);
  Ready(o);
  CHECK(o.abort_denied);
  CHECK(o.alive_before_death);
  CHECK(o.target_exited);
  CHECK(o.unsafe_quiet);
  CHECK(o.writes == 0);
}
TEST("lifecycle.detach_retry_and_deadline_preserve_commit") {
  for (auto c : {Case::DetachRetry, Case::DetachDeadline}) {
    auto o = RunFixtureCase(c);
    Replacements(o);
    CHECK(o.detaches >= 2);
    CHECK(o.exception_handled);
    CHECK(o.continues > 0);
    CHECK(o.writes == 4);
  }
}
TEST("lifecycle.unrelated_exception_reaches_target_handler") {
  auto o = RunFixtureCase(Case::Exception);
  Originals(o);
  CHECK(o.exception_handled);
  CHECK(o.continues > 0);
}
TEST("lifecycle.native_watchdog_obeys_inspection_and_mutation_policy") {
  auto safe = RunFixtureCase(Case::WatchdogInspect);
  Originals(safe);
  CHECK(safe.elapsed_ms <= 3000);
  CHECK(safe.helper_code == 1460 || safe.helper_code == 124);
  auto unsafe = RunFixtureCase(Case::WatchdogMutation);
  Ready(unsafe);
  CHECK(unsafe.elapsed_ms <= 3000);
  CHECK(unsafe.armed);
  CHECK(unsafe.target_exited);
  CHECK(!unsafe.mixed);
  CHECK(unsafe.helper_code == 1460 || unsafe.helper_code == 124);
}
TEST("lifecycle.blocked_acquisition_diagnostic_and_final_stdout_are_bounded") {
  for (auto c : {Case::BlockAcquisition, Case::BlockDiagnostic, Case::BlockConsole}) {
    auto o = RunFixtureCase(c);
    Replacements(o);
    CHECK(o.detached);
    CHECK(o.blocked);
    CHECK(o.cancellation_ms >= 1800 && o.cancellation_ms <= 2500);
    CHECK(o.helper_code == 1460 || o.helper_code == 124);
  }
}
TEST("lifecycle.readonly_cancellation_has_no_mutation_or_attach") {
  auto o = RunFixtureCase(Case::ReadOnlyCancel);
  Originals(o);
  CHECK(o.blocked);
  CHECK(o.readonly_acquired);
  CHECK(o.attach_calls == 0 && o.writes == 0 && o.abort_calls == 0);
  CHECK(o.cancellation_ms >= 1800 && o.cancellation_ms <= 2500);
}
TEST("lifecycle.support_collection_preserves_sentinel") {
  auto o = RunFixtureCase(Case::Support);
  Originals(o);
  CHECK(o.attach_calls == 0 && o.writes == 0 && o.abort_calls == 0);
}
TEST("lifecycle.storage_refusal_precedes_source_and_output_io") {
  auto o = RunFixtureCase(Case::StorageRefusal);
  Originals(o);
  CHECK(o.storage_refused_before_io);
  CHECK(o.attach_calls == 0 && o.writes == 0 && o.abort_calls == 0);
}
TEST("lifecycle.observed_pin_preserves_identity_and_restricts_rights") {
  auto o = RunFixtureCase(Case::ObservedIdentity);
  Originals(o);
  CHECK(o.identity_verified);
  CHECK(o.readonly_denied);
  CHECK(o.attach_calls == 0 && o.writes == 0 && o.abort_calls == 0);
}

TEST("lifecycle.blocked_observation_has_independent_deadline") {
  auto o = RunFixtureCase(Case::BlockAcquisitionDeadline);
  Replacements(o);
  CHECK(o.detached);
  CHECK(o.blocked);
  CHECK(o.readonly_acquired);
  CHECK(o.blocked_ms >= 2800 && o.blocked_ms <= 3500);
  CHECK(o.cancellation_ms == 0);
  CHECK(o.helper_code == 1460 || o.helper_code == 124);
}
TEST("lifecycle.observed_handle_rejects_exited_pinned_target") {
  auto o = RunFixtureCase(Case::ObservedExited);
  Ready(o);
  CHECK(o.target_exited);
  CHECK(o.ran_originals);
  CHECK(o.protections_restored);
  CHECK(o.observed_exit_rejected);
  CHECK(o.attach_calls == 0 && o.writes == 0 && o.abort_calls == 0);
}

TEST("lifecycle.setup_failure_and_denied_cleanup_are_contained") {
  auto o = RunCleanupRegression();
  CHECK(o.setup_failed);
  CHECK(o.constructor_exit);
  CHECK(o.denial_reported);
  CHECK(o.denied_alive);
  CHECK(o.recovered_exit);
  CHECK(o.sentinel_survived);
}
