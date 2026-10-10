#pragma once

#include "wfg/runtime_observation.hpp"
#include "verified_identity.hpp"

namespace observation_fixture {
template<class T> wfg::Observed<T> Field(T value) {
  return {std::move(value), wfg::Availability::Observed, "synthetic_sdk_fixture", "synthetic only; no production layout"};
}
inline wfg::RuntimeSample OnSample(std::uint64_t frame = 1) {
  wfg::RuntimeSample sample;
  sample.requested_mode = Field(wfg::RequestedFgMode::On);
  sample.requested_count = Field(1U);
  sample.sdk_status = Field(0U);
  sample.sdk_code = Field(0U);
  sample.frame_id = Field(frame);
  sample.actual_presentations = Field(2U);
  sample.generation_active = Field(true);
  return sample;
}
inline void Ignore(void*) noexcept {}

class Provider final : public wfg::observation_detail::IReadOnlyObservationProvider {
public:
  std::chrono::steady_clock::time_point Now() const override { return std::chrono::steady_clock::time_point{} + elapsed; }
  void WaitUntil(std::chrono::steady_clock::time_point deadline, std::stop_token cancellation) override {
    ++waits;
    if (cancel_during_wait) cancel_during_wait->request_stop();
    if (!cancellation.stop_requested()) elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(deadline.time_since_epoch());
  }
  wfg::Result<wfg::Unit> CheckPinned(const wfg::ObservedProcess&) override {
    ++checks;
    if (checks >= fail_check) return wfg::Error{wfg::ErrorCode::IdentityChanged, wfg::Stage::runtime_observation, "pinned_identity_changed"};
    return wfg::Unit{};
  }
  wfg::observation_detail::Snapshot Capture(const wfg::ObservedProcess&, wfg::Deadline, std::stop_token) override {
    sample_times.push_back(elapsed);
    elapsed += capture_time;
    if (cancel_during_capture) cancel_during_capture->request_stop();
    wfg::observation_detail::Snapshot result;
    if (!sequence.empty()) result.runtime = sequence[std::min(sample_times.size() - 1, sequence.size() - 1)];
    else result.runtime.sdk_status = {std::nullopt, wfg::Availability::UnsupportedLayout, "synthetic_unknown_layout", "no approved SDK layout"};
    return result;
  }
  std::chrono::milliseconds elapsed{};
  std::chrono::milliseconds capture_time{};
  std::vector<std::chrono::milliseconds> sample_times;
  std::vector<wfg::RuntimeSample> sequence;
  unsigned checks{};
  unsigned waits{};
  unsigned fail_check{~0U};
  std::stop_source* cancel_during_wait{};
  std::stop_source* cancel_during_capture{};
};
struct Fixture {
  int handle{};
  wfg::ObservedProcess observed = wfg::VerifiedIdentityTestFactory::Observed(&handle, Ignore);
  Provider provider;
  wfg::EffectiveConfig config = wfg::DefaultConfiguration();
  Fixture() { config.observation_seconds = 5; config.runtime_observation = true; }
  wfg::ObservationReport Run(std::stop_token cancellation = {}) {
    return wfg::observation_detail::ObserveRuntimeWithProvider(observed, config, cancellation, provider);
  }
};
}
