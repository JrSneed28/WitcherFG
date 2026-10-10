#pragma once

#include "wfg/configuration.hpp"
#include "wfg/windows_process.hpp"

#include <stop_token>

namespace wfg {

enum class RequestedFgMode { Off, On };
enum class ActivityAssessment { Unknown, OffExpected, RejectionObserved, ActivityObserved, NotObservedInWindow };
struct SdkRejection { std::uint32_t code{}; std::string reason; };

// A separate diagnostic capability. No compiled SDK record is currently approved.
// Only selection can construct it; callers cannot supply offsets or semantics.
class DiagnosticLayout final {
public:
  DiagnosticLayout(const DiagnosticLayout&) = default;
private:
  DiagnosticLayout() = default;
  struct ModuleIdentity {
    std::uint16_t machine{};
    std::uint32_t timestamp{};
    std::uint32_t size_of_image{};
    Hash256 sha256{};
  };
  enum class FieldType { Uint32, Uint64, Boolean };
  struct Field { Rva offset{}; FieldType type{}; };
  ModuleIdentity executable_;
  ModuleIdentity plugin_;
  std::vector<CodeFingerprint> executable_guards_;
  std::vector<CodeFingerprint> plugin_guards_;
  std::vector<Field> fields_;
  std::vector<SdkRejection> decoded_rejections_;
  friend Result<DiagnosticLayout> SelectDiagnosticLayout(const RuntimeInventory&);
};

struct RuntimeSample {
  Observed<RequestedFgMode> requested_mode;
  Observed<std::uint32_t> requested_count;
  Observed<std::uint32_t> sdk_status;
  Observed<std::uint32_t> sdk_code;
  Observed<std::uint64_t> frame_id;
  Observed<std::uint32_t> actual_presentations;
  // Decoded state, independent of a raw status of zero or module presence.
  Observed<bool> generation_active;
  Observed<bool> factory_created;
  Observed<bool> factory_proxy;
  Observed<bool> swap_chain_intercepted;
  Observed<SdkRejection> decoded_rejection;
};

struct ObservationReport {
  RuntimeSample latest;
  Observed<RuntimeInventory> inventory;
  EnvironmentSnapshot environment;
  Observed<Routing> import_routing;
  std::uint32_t samples{};
  std::chrono::milliseconds duration{};
  bool cancelled{};
  bool process_lost{};
  bool acquisition_overran{};
  std::vector<std::string> limitations;
  std::optional<SdkRejection> sdk_rejection;
  ActivityAssessment activity{ActivityAssessment::Unknown};
  // Stable logging code for the coordinator; emission uses the existing RunLogger.
  std::optional<std::string> diagnostic_code;
};

[[nodiscard]] Result<DiagnosticLayout> SelectDiagnosticLayout(const RuntimeInventory& inventory);
[[nodiscard]] RuntimeSample ReadRuntimeSample(const IMemory& memory, const RuntimeInventory& inventory, const DiagnosticLayout& layout);
// Explicit invocation is authorized by the coordinator (--diagnose or opted-in
// post-detach observation). The configuration's default remains Off.
[[nodiscard]] ObservationReport ObserveRuntime(const ObservedProcess& process, const EffectiveConfig& config, std::stop_token cancellation);

namespace observation_detail {
struct Snapshot {
  RuntimeSample runtime;
  Observed<RuntimeInventory> inventory;
  EnvironmentSnapshot environment;
  Observed<Routing> import_routing;
};
// Internal read-only acquisition seam, not a layout/SDK validation bypass.
// Implementations must use the pinned object, a monotonic clock, fresh available
// fields and independently decoded semantics. WaitUntil must honor cancellation.
// Synthetic implementations belong in test support, never the production catalog.
class IReadOnlyObservationProvider {
public:
  virtual ~IReadOnlyObservationProvider() = default;
  virtual std::chrono::steady_clock::time_point Now() const = 0;
  virtual void WaitUntil(std::chrono::steady_clock::time_point deadline, std::stop_token cancellation) = 0;
  virtual Result<Unit> CheckPinned(const ObservedProcess& process) = 0;
  virtual Snapshot Capture(const ObservedProcess& process, Deadline deadline, std::stop_token cancellation) = 0;
};
[[nodiscard]] ObservationReport ObserveRuntimeWithProvider(const ObservedProcess& process, const EffectiveConfig& config,
                                                         std::stop_token cancellation, IReadOnlyObservationProvider& provider);
}
}
