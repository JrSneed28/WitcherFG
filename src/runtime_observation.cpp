#include "wfg/runtime_observation.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <condition_variable>
#include <mutex>

namespace wfg {
namespace {
template<class T> Observed<T> Missing(Availability availability, std::string_view reason, std::string_view source = "read_only_runtime") {
  return {std::nullopt, availability, std::string(source), std::string(reason)};
}
EnvironmentSnapshot UncollectedEnvironment() {
  EnvironmentSnapshot environment;
  constexpr auto reason = "Host environment collection was not completed.";
  environment.os = Missing<std::string>(Availability::NotObserved, reason, "host_environment");
  environment.gpu = Missing<std::string>(Availability::NotObserved, reason, "host_environment");
  environment.driver = Missing<std::string>(Availability::NotObserved, reason, "host_environment");
  environment.selected_adapter = Missing<std::string>(Availability::NotObserved, "Target adapter was not independently observed.", "read_only_runtime");
  environment.hags_configured = Missing<bool>(Availability::NotObserved, reason, "host_environment");
  environment.hags_active = Missing<bool>(Availability::NotObserved, reason, "host_environment");
  return environment;
}
RuntimeSample Unavailable(Availability availability, std::string_view reason) {
  RuntimeSample sample;
  sample.requested_mode = Missing<RequestedFgMode>(availability, reason);
  sample.requested_count = Missing<std::uint32_t>(availability, reason);
  sample.sdk_status = Missing<std::uint32_t>(availability, reason);
  sample.sdk_code = Missing<std::uint32_t>(availability, reason);
  sample.frame_id = Missing<std::uint64_t>(availability, reason);
  sample.actual_presentations = Missing<std::uint32_t>(availability, reason);
  sample.generation_active = Missing<bool>(availability, reason);
  sample.factory_created = Missing<bool>(availability, reason);
  sample.factory_proxy = Missing<bool>(availability, reason);
  sample.swap_chain_intercepted = Missing<bool>(availability, reason);
  sample.decoded_rejection = Missing<SdkRejection>(availability, reason);
  return sample;
}
template<class T> bool Available(const Observed<T>& field) {
  return field.availability == Availability::Observed && field.value.has_value();
}
template<class T> void DiscardUnavailableValue(Observed<T>& field) {
  if (Available(field) && field.source.empty()) {
    field.availability = Availability::ReadFailed;
    field.reason = "Available observation lacked its source.";
  }
  if (!Available(field)) {
    field.value.reset();
    if (field.availability == Availability::Observed) {
      field.availability = Availability::ReadFailed;
      field.reason = "Observed field was missing its value.";
    }
  }
  if (field.source.empty()) field.source = "read_only_runtime";
  if (!Available(field) && field.reason.empty()) field.reason = "Field not exposed by a validated observation.";
  field.source.resize(std::min(field.source.size(), std::size_t{64}));
  field.reason.resize(std::min(field.reason.size(), std::size_t{256}));
}
void Normalize(RuntimeSample& sample) {
  DiscardUnavailableValue(sample.requested_mode);
  DiscardUnavailableValue(sample.requested_count);
  DiscardUnavailableValue(sample.sdk_status);
  DiscardUnavailableValue(sample.sdk_code);
  DiscardUnavailableValue(sample.frame_id);
  DiscardUnavailableValue(sample.actual_presentations);
  DiscardUnavailableValue(sample.generation_active);
  DiscardUnavailableValue(sample.factory_created);
  DiscardUnavailableValue(sample.factory_proxy);
  DiscardUnavailableValue(sample.swap_chain_intercepted);
  DiscardUnavailableValue(sample.decoded_rejection);
}
void NormalizeMetadata(observation_detail::Snapshot& snapshot) {
  if (!Available(snapshot.inventory) && snapshot.import_routing.reason.empty())
    snapshot.import_routing.reason = "Import routing was not checked because pinned inventory was unavailable.";
  if (snapshot.inventory.reason.empty() && !Available(snapshot.inventory))
    snapshot.inventory.reason = "Pinned inventory collection was not completed.";
  DiscardUnavailableValue(snapshot.inventory);
  DiscardUnavailableValue(snapshot.import_routing);
  DiscardUnavailableValue(snapshot.environment.os);
  DiscardUnavailableValue(snapshot.environment.gpu);
  DiscardUnavailableValue(snapshot.environment.driver);
  DiscardUnavailableValue(snapshot.environment.selected_adapter);
  DiscardUnavailableValue(snapshot.environment.hags_configured);
  DiscardUnavailableValue(snapshot.environment.hags_active);
}
bool UsableActivityState(const RuntimeSample& sample) {
  return Available(sample.requested_mode) && *sample.requested_mode.value == RequestedFgMode::On &&
         Available(sample.sdk_status) && *sample.sdk_status.value == 0 &&
         Available(sample.generation_active) && Available(sample.frame_id) && Available(sample.actual_presentations) &&
         !Available(sample.decoded_rejection);
}

class NativeProvider final : public observation_detail::IReadOnlyObservationProvider {
public:
  std::chrono::steady_clock::time_point Now() const override { return std::chrono::steady_clock::now(); }
  void WaitUntil(std::chrono::steady_clock::time_point deadline, std::stop_token cancellation) override {
    std::mutex mutex;
    std::unique_lock lock(mutex);
    std::condition_variable_any wake;
    wake.wait_until(lock, cancellation, deadline, [] { return false; });
  }
  Result<Unit> CheckPinned(const ObservedProcess& process) override {
    const HANDLE handle = process.native_handle();
    FILETIME created{}, exited{}, kernel{}, user{};
    if (handle == nullptr || GetProcessId(handle) != process.pid() ||
        WaitForSingleObject(handle, 0) != WAIT_TIMEOUT || !GetProcessTimes(handle, &created, &exited, &kernel, &user)) {
      return Error{ErrorCode::IdentityChanged, Stage::runtime_observation, "observation_process_not_current"};
    }
    const auto creation = (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    if (creation != process.creation_time()) return Error{ErrorCode::IdentityChanged, Stage::runtime_observation, "observation_creation_changed"};
    return Unit{};
  }
  observation_detail::Snapshot Capture(const ObservedProcess& process, Deadline deadline, std::stop_token cancellation) override {
    const auto interrupted = [&] { return cancellation.stop_requested() || Now() >= deadline.value; };
    if (interrupted()) return {};
    if (captured_) return snapshot_;
    captured_ = true;
    snapshot_.environment = ObserveEnvironment(&process);
    if (interrupted()) return {};
    const auto memory = OpenReadOnlyMemory(process);
    if (interrupted()) return {};
    if (!IsOk(memory)) {
      snapshot_.runtime = Unavailable(Availability::ReadFailed, "Read-only process access unavailable.");
      snapshot_.inventory = Missing<RuntimeInventory>(Availability::ReadFailed, "Read-only process access unavailable.");
      snapshot_.import_routing = Missing<Routing>(Availability::NotObserved, "Import routing skipped because read-only process access was unavailable.", "validated_imports");
      return snapshot_;
    }
    const auto inventory = InventoryObservedProcess(process, *Value(memory));
    if (interrupted()) return {};
    if (!IsOk(inventory)) {
      snapshot_.runtime = Unavailable(Availability::ReadFailed, "Pinned process inventory unavailable.");
      snapshot_.inventory = Missing<RuntimeInventory>(Availability::ReadFailed, "Pinned process inventory unavailable.");
      snapshot_.import_routing = Missing<Routing>(Availability::NotObserved, "Import routing skipped because pinned inventory was unavailable.", "validated_imports");
      return snapshot_;
    }
    snapshot_.inventory = {Value(inventory), Availability::Observed, "pinned_process_inventory", "Initial read-only dependency snapshot."};
    const auto routing = ValidateBuild(*Value(memory), Value(inventory), SupportedBuild());
    if (interrupted()) return {};
    if (IsOk(routing)) snapshot_.import_routing = {Value(routing).routing, Availability::Observed, "validated_imports", "Initial approved build import snapshot."};
    else snapshot_.import_routing = Missing<Routing>(Availability::UnsupportedLayout, "Approved build/import guards did not validate.");
    const auto layout = SelectDiagnosticLayout(Value(inventory));
    snapshot_.runtime = IsOk(layout) ? ReadRuntimeSample(*Value(memory), Value(inventory), Value(layout))
                                    : Unavailable(Availability::UnsupportedLayout, "No independently validated SDK diagnostic layout.");
    return snapshot_;
  }
private:
  bool captured_{};
  observation_detail::Snapshot snapshot_;
};
}

Result<DiagnosticLayout> SelectDiagnosticLayout(const RuntimeInventory&) {
  // Deliberately empty: module names and startup compatibility cannot establish
  // plugin-internal field offsets or the vendor meaning of SDK state.
  return Error{ErrorCode::UnsupportedObservation, Stage::runtime_observation, "diagnostic_layout_unavailable"};
}
RuntimeSample ReadRuntimeSample(const IMemory&, const RuntimeInventory&, const DiagnosticLayout&) {
  // No record can currently be constructed. Never read speculative offsets.
  return Unavailable(Availability::UnsupportedLayout, "No independently validated SDK diagnostic layout.");
}
ObservationReport ObserveRuntime(const ObservedProcess& process, const EffectiveConfig& config, std::stop_token cancellation) {
  NativeProvider provider;
  return observation_detail::ObserveRuntimeWithProvider(process, config, cancellation, provider);
}
namespace observation_detail {
ObservationReport ObserveRuntimeWithProvider(const ObservedProcess& process, const EffectiveConfig& config,
                                             std::stop_token cancellation, IReadOnlyObservationProvider& provider) {
  ObservationReport report;
  report.latest = Unavailable(Availability::NotObserved, "No runtime sample captured.");
  report.inventory = Missing<RuntimeInventory>(Availability::NotObserved, "Pinned inventory collection was not completed.", "pinned_process_inventory");
  report.import_routing = Missing<Routing>(Availability::NotObserved, "Import routing was not checked.", "validated_imports");
  report.environment = UncollectedEnvironment();
  report.limitations = {"Visible output requires an independent visual check.",
                        "Loaded modules and zero SDK status do not prove SDK initialization or interpolation.",
                        "SDK actual presentations are raw values, not generated/rendered ratios.",
                        "Inventory, environment and import routing are initial snapshots; modules can change.",
                        "Synchronous Windows inventory calls cannot be preempted; an independent coordinator watchdog supplies a hard process deadline."};
  const auto started = provider.Now();
  const auto interval = std::chrono::milliseconds{std::clamp(config.sample_interval_ms, 1000U, 10000U)};
  const auto deadline = started + std::chrono::seconds{std::clamp(config.observation_seconds, 5U, 120U)};
  std::optional<std::uint64_t> previous_frame;
  bool activity_observed{};
  while (!cancellation.stop_requested() && provider.Now() < deadline) {
    if (!IsOk(provider.CheckPinned(process))) {
      report.process_lost = true;
      break;
    }
    if (cancellation.stop_requested() || provider.Now() >= deadline) break;
    auto snapshot = provider.Capture(process, Deadline{deadline}, cancellation);
    if (cancellation.stop_requested()) break;
    if (provider.Now() >= deadline) {
      report.acquisition_overran = true;
      break;
    }
    if (!IsOk(provider.CheckPinned(process))) {
      report.process_lost = true;
      break;
    }
    if (cancellation.stop_requested()) break;
    if (provider.Now() >= deadline) {
      report.acquisition_overran = true;
      break;
    }
    Normalize(snapshot.runtime);
    NormalizeMetadata(snapshot);
    report.latest = std::move(snapshot.runtime);
    report.inventory = std::move(snapshot.inventory);
    report.environment = std::move(snapshot.environment);
    report.import_routing = std::move(snapshot.import_routing);
    ++report.samples;
    const auto& sample = report.latest;
    if (Available(sample.decoded_rejection) && Available(sample.sdk_status) && Available(sample.sdk_code) &&
        sample.decoded_rejection.value->code == *sample.sdk_code.value && !sample.decoded_rejection.value->reason.empty()) {
      report.sdk_rejection = *sample.decoded_rejection.value;
    }
    if (UsableActivityState(sample)) {
      if (previous_frame && *sample.frame_id.value > *previous_frame && *sample.generation_active.value && *sample.actual_presentations.value > 0) activity_observed = true;
      previous_frame = *sample.frame_id.value;
    } else previous_frame.reset();
    const auto next = std::min(provider.Now() + interval, deadline);
    if (provider.Now() < next) {
      provider.WaitUntil(next, cancellation);
      // A terminal wait must revalidate the pinned object too. Cancellation
      // exits before any additional process work or acquisition.
      if (cancellation.stop_requested()) break;
      if (!IsOk(provider.CheckPinned(process))) {
        report.process_lost = true;
        break;
      }
    }
  }
  report.cancelled = cancellation.stop_requested();
  report.duration = std::chrono::duration_cast<std::chrono::milliseconds>(provider.Now() - started);
  if (report.process_lost) {
    report.latest = Unavailable(Availability::ReadFailed, "Pinned process exited or identity changed; prior values discarded.");
    report.sdk_rejection.reset();
    report.inventory = Missing<RuntimeInventory>(Availability::ReadFailed, "Pinned process exited or identity changed.");
    report.import_routing = Missing<Routing>(Availability::ReadFailed, "Pinned process exited or identity changed.");
    report.environment.selected_adapter = Missing<std::string>(Availability::ReadFailed, "Pinned process exited or identity changed; target adapter discarded.");
  } else if (report.sdk_rejection) report.activity = ActivityAssessment::RejectionObserved;
  else if (Available(report.latest.requested_mode) && *report.latest.requested_mode.value == RequestedFgMode::Off) report.activity = ActivityAssessment::OffExpected;
  else if (activity_observed) report.activity = ActivityAssessment::ActivityObserved;
  else if (!report.cancelled && UsableActivityState(report.latest)) report.activity = ActivityAssessment::NotObservedInWindow;
  if (report.activity == ActivityAssessment::RejectionObserved) report.diagnostic_code = "WFG-E-FG_REJECTED";
  else if (report.activity == ActivityAssessment::NotObservedInWindow) report.diagnostic_code = "WFG-W-FG_ACTIVITY_NOT_OBSERVED";
  else if (report.activity == ActivityAssessment::Unknown) report.diagnostic_code = "WFG-W-OBSERVATION_UNAVAILABLE";
  return report;
}
}
}
