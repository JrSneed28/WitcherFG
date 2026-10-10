#include "support.hpp"
#include "fixtures/observation.hpp"

#include <type_traits>
#include <thread>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

using namespace wfg;
using namespace observation_fixture;

static_assert(!std::is_default_constructible_v<DiagnosticLayout>);

TEST("observation.unknown_layout_is_not_fg_rejection") {
  RuntimeInventory inventory;
  inventory.dependencies.push_back({"sl.dlss_g.dll"});
  REQUIRE_ERROR(SelectDiagnosticLayout(inventory), ErrorCode::UnsupportedObservation);
  Fixture fixture;
  auto report = fixture.Run();
  CHECK(report.latest.sdk_status.availability == Availability::UnsupportedLayout);
  CHECK(!report.sdk_rejection);
  CHECK(report.activity == ActivityAssessment::Unknown);
  CHECK(report.diagnostic_code == "WFG-W-OBSERVATION_UNAVAILABLE");
}
TEST("observation.off_or_status_zero_does_not_prove_generated_output") {
  Fixture fixture;
  fixture.provider.sequence = {OnSample()};
  auto report = fixture.Run();
  CHECK(report.activity == ActivityAssessment::NotObservedInWindow);
  CHECK(report.diagnostic_code == "WFG-W-FG_ACTIVITY_NOT_OBSERVED");
  CHECK(std::find(report.limitations.begin(), report.limitations.end(), "Visible output requires an independent visual check.") != report.limitations.end());
  CHECK(report.samples == 5);
}
TEST("observation.expected_off") {
  Fixture fixture;
  auto sample = OnSample(); sample.requested_mode = Field(RequestedFgMode::Off);
  fixture.provider.sequence = {sample};
  const auto report = fixture.Run();
  CHECK(report.activity == ActivityAssessment::OffExpected);
  CHECK(!report.diagnostic_code);
}
TEST("observation.recognized_exposed_sdk_rejection") {
  Fixture fixture;
  auto sample = OnSample(); sample.sdk_status = Field(4U); sample.sdk_code = Field(91U);
  sample.decoded_rejection = Field(SdkRejection{91U, "synthetic exposed rejection"});
  fixture.provider.sequence = {sample};
  const auto report = fixture.Run();
  CHECK(report.activity == ActivityAssessment::RejectionObserved);
  CHECK(report.sdk_rejection && report.sdk_rejection->code == 91);
  CHECK(report.diagnostic_code == "WFG-E-FG_REJECTED");
}
TEST("observation.unknown_status_flags_are_unknown") {
  Fixture fixture;
  auto sample = OnSample(); sample.sdk_status = Field(0x80U);
  fixture.provider.sequence = {sample};
  const auto report = fixture.Run();
  CHECK(!report.sdk_rejection);
  CHECK(report.activity == ActivityAssessment::Unknown);
}
TEST("observation.decoded_rejection_requires_readable_matching_code") {
  for (const bool mismatched : {false, true}) {
    Fixture fixture;
    auto sample = OnSample(); sample.decoded_rejection = Field(SdkRejection{91U, "synthetic exposed rejection"});
    sample.sdk_code = mismatched ? Field(92U) : Observed<std::uint32_t>{};
    fixture.provider.sequence = {sample};
    const auto report = fixture.Run();
    CHECK(!report.sdk_rejection);
    CHECK(report.activity == ActivityAssessment::Unknown);
  }
}
TEST("observation.progress_is_sampled_activity_not_visible_verification") {
  Fixture fixture;
  fixture.provider.sequence = {OnSample(1), OnSample(2)};
  const auto report = fixture.Run();
  CHECK(report.activity == ActivityAssessment::ActivityObserved);
  CHECK(!report.sdk_rejection);
  CHECK(report.latest.actual_presentations.value == 2U);
}
TEST("observation.frame_progress_without_decoded_generation_is_unknown") {
  Fixture fixture;
  auto first = OnSample(1); first.generation_active = {};
  auto second = OnSample(2); second.generation_active = {};
  fixture.provider.sequence = {first, second};
  CHECK(fixture.Run().activity == ActivityAssessment::Unknown);
}
TEST("observation.frame_id_reset_is_not_activity") {
  Fixture fixture;
  fixture.provider.sequence = {OnSample(9), OnSample(1)};
  CHECK(fixture.Run().activity == ActivityAssessment::NotObservedInWindow);
}
TEST("observation.unreadable_latest_does_not_reuse_prior_values") {
  Fixture fixture;
  auto stale = OnSample();
  stale.sdk_status = {0U, Availability::ReadFailed, "synthetic_sdk_fixture", "stale"};
  fixture.provider.sequence = {OnSample(), stale};
  const auto report = fixture.Run();
  CHECK(report.latest.sdk_status.availability == Availability::ReadFailed);
  CHECK(!report.latest.sdk_status.value);
  CHECK(report.activity == ActivityAssessment::Unknown);
}
TEST("observation.swap_chain_unavailable_does_not_mean_rejected") {
  Fixture fixture; fixture.provider.sequence = {OnSample()};
  const auto report = fixture.Run();
  CHECK(report.latest.swap_chain_intercepted.availability == Availability::NotObserved);
  CHECK(!report.sdk_rejection);
}
TEST("observation.process_exit_or_pid_reuse_invalidates_sample") {
  for (const unsigned failure : {1U, 2U, 3U}) {
    Fixture fixture; fixture.provider.sequence = {OnSample()}; fixture.provider.fail_check = failure;
    const auto report = fixture.Run();
    CHECK(report.process_lost);
    CHECK(report.activity == ActivityAssessment::Unknown);
    CHECK(report.latest.sdk_status.availability == Availability::ReadFailed);
    CHECK(!report.latest.sdk_status.value);
    CHECK(report.samples <= 1);
  }
}
TEST("observation.pre_cancelled_does_not_collect") {
  Fixture fixture; std::stop_source stop; stop.request_stop();
  const auto report = fixture.Run(stop.get_token());
  CHECK(report.cancelled);
  CHECK(report.samples == 0);
  CHECK(fixture.provider.checks == 0);
}
TEST("observation.cancellation_during_wait_stops_before_next_sample") {
  Fixture fixture; std::stop_source stop; fixture.provider.cancel_during_wait = &stop;
  const auto report = fixture.Run(stop.get_token());
  CHECK(report.cancelled);
  CHECK(report.samples == 1);
  CHECK(report.duration == std::chrono::milliseconds{0});
}
TEST("observation.interval_duration_are_defensively_bounded") {
  Fixture fixture; fixture.config.sample_interval_ms = 1; fixture.config.observation_seconds = 0;
  const auto report = fixture.Run();
  CHECK(report.samples == 5);
  CHECK(report.duration == std::chrono::milliseconds{5000});
  for (std::size_t i = 1; i < fixture.provider.sample_times.size(); ++i)
    CHECK(fixture.provider.sample_times[i] - fixture.provider.sample_times[i - 1] >= std::chrono::milliseconds{1000});
  Fixture maximum; maximum.config.observation_seconds = 999; maximum.config.sample_interval_ms = 99999;
  const auto bounded = maximum.Run();
  CHECK(bounded.samples == 12);
  CHECK(bounded.duration == std::chrono::milliseconds{120000});
}
TEST("observation.slow_capture_does_not_schedule_catchup_burst") {
  Fixture fixture; fixture.provider.capture_time = std::chrono::milliseconds{1500};
  const auto report = fixture.Run();
  CHECK(report.samples == 2);
  CHECK(report.duration == std::chrono::milliseconds{5000});
  CHECK(fixture.provider.sample_times[1] == std::chrono::milliseconds{2500});
}
TEST("observation.acquisition_crossing_deadline_discards_late_sample") {
  Fixture fixture; fixture.provider.sequence = {OnSample()};
  fixture.provider.capture_time = std::chrono::milliseconds{6000};
  const auto report = fixture.Run();
  CHECK(report.samples == 0);
  CHECK(report.latest.sdk_status.availability == Availability::NotObserved);
  CHECK(report.duration == std::chrono::milliseconds{6000});
  CHECK(report.acquisition_overran);
  CHECK(fixture.provider.waits == 0);
}
TEST("observation.acquisition_cancelled_does_not_check_or_wait_again") {
  Fixture fixture; std::stop_source stop; fixture.provider.cancel_during_capture = &stop;
  const auto report = fixture.Run(stop.get_token());
  CHECK(report.cancelled);
  CHECK(report.samples == 0);
  CHECK(fixture.provider.checks == 1);
  CHECK(fixture.provider.waits == 0);
}

namespace {
void CloseNative(void* handle) noexcept { CloseHandle(handle); }
ObservedProcess PinSelf(std::uint64_t creation_delta = 0) {
  HANDLE handle{};
  if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &handle,
                       PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ | SYNCHRONIZE, FALSE, 0))
    throw std::runtime_error("native_self_pin_failed");
  FILETIME created{}, exited{}, kernel{}, user{};
  if (!GetProcessTimes(handle, &created, &exited, &kernel, &user)) {
    CloseHandle(handle);
    throw std::runtime_error("native_self_creation_failed");
  }
  const auto creation = (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
  return VerifiedIdentityTestFactory::Observed(handle, CloseNative, GetProcessId(handle), creation + creation_delta);
}
}
TEST("observation.native_pinned_self_has_actual_host_facts_without_sdk_claims") {
  auto process = PinSelf();
  auto config = DefaultConfiguration(); config.observation_seconds = 5;
  const auto report = ObserveRuntime(process, config, {});
  CHECK(!report.process_lost);
  CHECK(!report.cancelled);
  CHECK(report.samples >= 1 && report.samples <= 5);
  CHECK(report.duration >= std::chrono::milliseconds{5000});
  CHECK(report.duration < std::chrono::milliseconds{7000});
  CHECK(report.environment.os.availability == Availability::Observed);
  CHECK(report.environment.os.value && !report.environment.os.value->empty());
  CHECK(report.environment.selected_adapter.availability == Availability::NotObserved);
  CHECK(report.latest.sdk_status.availability != Availability::Observed);
  CHECK(!report.latest.sdk_status.value);
  CHECK(!report.sdk_rejection);
  CHECK(report.activity == ActivityAssessment::Unknown);
}
TEST("observation.native_wrong_creation_time_cannot_follow_pid") {
  auto process = PinSelf(1);
  const auto report = ObserveRuntime(process, DefaultConfiguration(), {});
  CHECK(report.process_lost);
  CHECK(report.samples == 0);
  CHECK(report.latest.sdk_status.availability == Availability::ReadFailed);
  CHECK(report.activity == ActivityAssessment::Unknown);
}
TEST("observation.native_wait_is_cancelled_promptly") {
  auto process = PinSelf();
  auto config = DefaultConfiguration(); config.observation_seconds = 120; config.sample_interval_ms = 10000;
  std::stop_source stop;
  std::jthread cancellation([&] { std::this_thread::sleep_for(std::chrono::milliseconds{250}); stop.request_stop(); });
  const auto report = ObserveRuntime(process, config, stop.get_token());
  CHECK(report.cancelled);
  CHECK(report.duration < std::chrono::milliseconds{2000});
  CHECK(report.samples <= 1);
  CHECK(!report.sdk_rejection);
}
TEST("observation.unsourced_values_are_unavailable_with_reasons") {
  Fixture fixture;
  auto sample = OnSample(); sample.requested_mode.source.clear();
  fixture.provider.sequence = {sample};
  const auto report = fixture.Run();
  CHECK(report.activity == ActivityAssessment::Unknown);
  CHECK(!report.latest.requested_mode.value);
  CHECK(report.latest.requested_mode.availability == Availability::ReadFailed);
  CHECK(!report.latest.requested_mode.source.empty());
  CHECK(!report.latest.requested_mode.reason.empty());
  CHECK(!report.latest.swap_chain_intercepted.source.empty());
  CHECK(!report.latest.swap_chain_intercepted.reason.empty());
}
