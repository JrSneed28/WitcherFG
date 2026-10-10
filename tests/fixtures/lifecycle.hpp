#pragma once
#include <array>
#include <cstdint>
namespace lifecycle {
enum class Case {
  InspectExit,
  InspectThreadExit,
  Die1,
  Die2,
  Die3,
  Die4,
  Commit,
  Rollback,
  Abort,
  TerminationDenied,
  InitialPolicyFailure,
  DetachRetry,
  DetachDeadline,
  Exception,
  WatchdogInspect,
  WatchdogMutation,
  BlockAcquisition,
  BlockDiagnostic,
  BlockConsole,
  ReadOnlyCancel,
  Support,
  StorageRefusal,
  ObservedIdentity,
  BlockAcquisitionDeadline,
  ObservedExited
};
struct Observation {
  bool ready{}, held{}, armed{}, helper_exited{}, target_exited{}, target_alive{}, sentinel_alive{};
  bool ran_originals{}, ran_replacements{}, protections_restored{}, mixed{}, exception_handled{};
  bool abort_denied{}, alive_before_death{}, unsafe_quiet{}, detached{}, identity_verified{},
      readonly_denied{};
  bool blocked{}, storage_refused_before_io{}, write_values_verified{}, thread_exit_resumed{},
      observed_exit_rejected{}, readonly_acquired{};
  unsigned writes{}, detaches{}, continues{}, attach_calls{}, abort_calls{}, helper_code{};
  std::uint64_t cancellation_ms{}, blocked_ms{}, elapsed_ms{};
};
Observation RunFixtureCase(Case);
} // namespace lifecycle
