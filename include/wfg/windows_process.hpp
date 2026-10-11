#pragma once

#include "wfg/compatibility.hpp"

#include <memory>
#include <filesystem>

namespace wfg {

struct LaunchIdentity {
  std::uint64_t earliest_creation_time{};
  std::uint32_t session{};
  std::wstring user;
  std::wstring package;
  std::wstring executable;
  bool require_fresh{true};
};

struct Candidate {
  std::uint32_t pid{};
  std::uint64_t creation_time{};
  std::uint32_t session{};
  std::wstring user;
  std::wstring package;
  std::wstring executable;
  std::uint32_t pinned_pid{};
  std::uint64_t pinned_creation_time{};
  std::shared_ptr<void> pin;
};

Result<Candidate> SelectCandidate(std::span<const Candidate> candidates, const LaunchIdentity& launch);

class IProcessBackend {
public:
  virtual ~IProcessBackend() = default;
  virtual std::chrono::steady_clock::time_point Now() const = 0;
  virtual void Sleep(std::chrono::milliseconds duration) = 0;
  virtual Result<Unit> Install() = 0;
  virtual Result<Unit> CheckRegistration() = 0;
  virtual Result<Unit> Activate() = 0;
  virtual Result<std::vector<Candidate>> Enumerate() = 0;
  virtual bool ActivationRequested() const = 0;
};

class PackageIdentity {
public:
  std::string name() const;
  std::string version() const;
  std::string family() const;
  std::string application_id() const;
  std::uint16_t architecture() const;
private:
  struct State;
  explicit PackageIdentity(std::shared_ptr<State> state) : state_(std::move(state)) {}
  std::shared_ptr<State> state_;
  friend class NativeProcessBackend;
  friend class VerifiedIdentityFactory;
  friend Result<PackageIdentity> DiscoverPackage();
  friend Result<Unit> ActivateRegistered(const PackageIdentity&);
  friend Result<ObservedProcess> OpenDiagnosticTarget(const PackageIdentity&);
  friend Result<std::unique_ptr<class ProcessObserver>> StartObserver(const PackageIdentity&, Deadline);
};

class ProcessObserver {
public:
  Result<Unit> Activate();
  Result<Candidate> WaitForCandidate();
  Result<OwnedProcess> WaitForFreshProcess();
private:
  ProcessObserver(LaunchIdentity launch, IProcessBackend& backend, Deadline deadline)
      : launch_(std::move(launch)), backend_(&backend), deadline_(deadline) {}
  LaunchIdentity launch_;
  IProcessBackend* backend_;
  Deadline deadline_;
  std::shared_ptr<IProcessBackend> owner_;
  bool activated_{};
  bool consumed_{};
  friend Result<std::unique_ptr<ProcessObserver>> InstallProcessObserver(LaunchIdentity, IProcessBackend&, Deadline);
  friend Result<std::unique_ptr<ProcessObserver>> StartObserver(const PackageIdentity&, Deadline);
};

Result<std::unique_ptr<ProcessObserver>> InstallProcessObserver(LaunchIdentity launch, IProcessBackend& backend, Deadline deadline);
Result<PackageIdentity> DiscoverPackage();
Result<std::unique_ptr<ProcessObserver>> StartObserver(const PackageIdentity& package, Deadline deadline);
Result<Unit> ActivateRegistered(const PackageIdentity& package);
Result<RuntimeInventory> InventoryProcess(const OwnedProcess& process, const IMemory& memory);
Result<std::unique_ptr<IWritableMemory>> OpenMemory(const OwnedProcess& process);
Result<ObservedProcess> DuplicateObservedProcess(const OwnedProcess& process);
Result<ObservedProcess> OpenDiagnosticTarget(const PackageIdentity& package);
Result<std::unique_ptr<IMemory>> OpenReadOnlyMemory(const ObservedProcess& process);
Result<RuntimeInventory> InventoryObservedProcess(const ObservedProcess& process, const IMemory& memory);
EnvironmentSnapshot ObserveEnvironment(const ObservedProcess* process);

namespace process_detail {
// Read-only package-document resolution; does not confer process or write authority.
struct RegisteredPackageMetadata {
  std::wstring name;
  std::wstring publisher;
  std::wstring version;
  std::wstring resource_id;
  std::wstring architecture;
};
Result<std::wstring> ResolvePackageExecutable(const std::wstring& root, const RegisteredPackageMetadata& registered);
#ifdef WFG_LIFECYCLE_TESTING
// Disposable-child test core only. No package eligibility bypass in production.
Result<std::unique_ptr<IWritableMemory>> OpenLifecycleMemory(const OwnedProcess&);
Result<std::unique_ptr<IMemory>> OpenLifecycleReadOnlyMemory(const ObservedProcess&);
#endif
struct OsBuild { std::uint32_t major{}; std::uint32_t minor{}; std::uint32_t build{}; };
struct AdapterFacts {
  std::string model;
  std::uint32_t vendor{};
  std::uint32_t device{};
  Result<std::string> driver_version;
};
class IEnvironmentBackend {
public:
  virtual ~IEnvironmentBackend() = default;
  virtual Result<OsBuild> QueryOsBuild() = 0;
  virtual Result<std::optional<AdapterFacts>> QueryAdapter(std::uint32_t index) = 0;
  virtual Result<std::optional<std::uint32_t>> QueryHagsMode() = 0;
};
EnvironmentSnapshot ObserveHostEnvironment(IEnvironmentBackend& backend);
class IActivationOperations {
public:
  virtual ~IActivationOperations() = default;
  virtual std::chrono::steady_clock::time_point Now() const = 0;
  virtual Result<Unit> Prepare() = 0;
  virtual Result<Unit> Launch() = 0;
};
Result<Unit> ActivateBeforeDeadline(IActivationOperations& operations, Deadline deadline);
class ILocalDirectory {
public:
  virtual ~ILocalDirectory() = default;
  virtual bool AtEnd() const = 0;
  virtual std::filesystem::path Current() const = 0;
  virtual void Increment(std::error_code& error) = 0;
};
Result<std::vector<std::filesystem::path>> EnumerateLocalFiles(ILocalDirectory& directory);
struct FileEvidence {
  std::uint64_t length{};
  Hash256 sha256{};
  std::string version;
  bool trusted_signature{};
  std::optional<DependencySigner> signer;
};
Result<FileEvidence> InspectFile(const std::wstring& path, bool require_nvidia);
// Read-only evidence policy; it grants no package/process/mutation capability.
bool QualifiesBundledDebugHelper(const FileEvidence& evidence, std::span<const std::byte> header,
  std::string_view package, std::string_view version, std::wstring_view relative_path);
Result<bool> InspectRegisteredBundledDebugHelper();
Result<Unit> ReadIntoHandle(void* handle, Address address, std::span<std::byte> bytes);
Result<Unit> VerifyBackingImage(void* handle, Address base, const std::wstring& path, const IMemory& memory);
Observed<DependencyRole> IdentifyDependencyRole(const IMemory& memory, const PeImage& image, const FileEvidence& evidence, std::string_view basename);
Result<Unit> ValidateStartupInventory(const RuntimeInventory& inventory);
}

}
