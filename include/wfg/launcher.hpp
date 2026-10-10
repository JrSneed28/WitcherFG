#pragma once
#include "wfg/diagnostics.hpp"
#include "wfg/runtime_observation.hpp"
#include "wfg/transaction.hpp"

namespace wfg {
struct PackageFacts { std::string name; std::string version; std::uint16_t architecture{}; };
class IRunGuard {
public:
  virtual ~IRunGuard() = default;
  virtual void Disarm() = 0;
};
// The native implementation owns the package/observer and derives every writable
// capability from the single returned OwnedProcess. Read-only modes never request it.
class ILaunchEnvironment {
public:
  virtual ~ILaunchEnvironment() = default;
  virtual std::span<const std::string_view> Arguments() const = 0;
  virtual std::chrono::steady_clock::time_point Now() const = 0;
  virtual Result<EffectiveConfig> Configuration(const CommandLine&) = 0;
  virtual ILogStorage& Diagnostics() = 0;
  virtual std::unique_ptr<IRunGuard> Guard(Deadline, std::stop_source&, std::stop_token external) = 0;
  virtual Result<PackageFacts> Discover() = 0;
  virtual EnvironmentSnapshot Environment() = 0;
  virtual Result<Unit> InstallObserver(Deadline) = 0;
  virtual Result<Unit> Activate() = 0;
  virtual Result<OwnedProcess> PinFresh() = 0;
  virtual Result<std::unique_ptr<IWritableMemory>> Memory(const OwnedProcess&) = 0;
  virtual IDebugBackend& Debugger() = 0;
  virtual Result<RuntimeInventory> Inventory(const OwnedProcess&, const IMemory&) = 0;
  virtual const CompatibilityRecord& Compatibility() const = 0;
  virtual Result<ObservationReport> Observe(const OwnedProcess*, const EffectiveConfig&, std::stop_token) = 0;
  // Must close the producer sink before collector opens persisted diagnostics.
  virtual Result<Unit> Collect(const EffectiveConfig&) = 0;
  [[noreturn]] virtual void ExitHelper() noexcept = 0;
};
RunResult RunLauncher(ILaunchEnvironment&, std::stop_token);
std::wstring FormatStatus(const RunResult&);
Result<Unit> WriteDiagnostic(const DiagnosticRecord&);
int ExitCode(Status) noexcept;
int RunNativeLauncher(std::span<const std::string_view>, std::stop_token);
}
