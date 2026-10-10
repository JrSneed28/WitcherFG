#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace wfg {

using Address = std::uint64_t;
using Rva = std::uint32_t;
using Bytes = std::vector<std::byte>;
using Hash256 = std::array<std::byte, 32>;
using Unit = std::monostate;

enum class Stage {
  configuration, package_discovery, activation, process_identity, debug_attach, dependencies,
  build_validation, pre_factory_guard, import_transaction, detach, runtime_observation,
  Configuration = configuration, Discover = package_discovery, Launch = activation,
  Debug = debug_attach, Inspect = build_validation, Compatibility = build_validation,
  Transaction = import_transaction, Detach = detach, Diagnostic = runtime_observation,
  Support = configuration
};
enum class ErrorCode {
  MalformedImage, UnsupportedBuild, UnexpectedImports, UntrustedInterposer, UnreadableMemory,
  ModuleNotReady, NoMatchingProcess, IdentityChanged, AmbiguousProcess, AlreadyRunning,
  StartupTooLate, AccessDenied, LaunchFailed, TimedOut, DebugFailure, MutationFailure,
  DiagnosticFailure, ConfigurationInvalid, DllConflict, UnsupportedObservation, SupportBundleFailure
};

class OperationIdentifier {
public:
  OperationIdentifier() : OperationIdentifier("invalid_operation") {}
  OperationIdentifier(const char* value) : OperationIdentifier(value == nullptr ? std::string_view{} : std::string_view(value)) {}
  OperationIdentifier(const std::string& value) : OperationIdentifier(std::string_view(value)) {}
  OperationIdentifier(std::string_view value) {
    if (!Valid(value)) value = "invalid_operation";
    size_ = value.size();
    for (std::size_t index = 0; index < size_; ++index) bytes_[index] = value[index];
  }
  std::string_view view() const noexcept { return {bytes_.data(), size_}; }
private:
  static bool Letter(char value) { return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z'); }
  static bool Valid(std::string_view value) {
    if (value.empty() || value.size() > 64 || !Letter(value.front())) return false;
    for (const char character : value) {
      if (!Letter(character) && !(character >= '0' && character <= '9') && character != '_') return false;
    }
    return true;
  }
  std::array<char, 65> bytes_{};
  std::size_t size_{};
};

struct Error {
  ErrorCode code{};
  Stage stage{Stage::Inspect};
  OperationIdentifier operation;
  std::optional<std::uint32_t> native_code;
  std::optional<std::size_t> affected_slot;
};

template <class T> using Result = std::variant<T, Error>;
template <class T> [[nodiscard]] inline bool IsOk(const Result<T>& value) { return std::holds_alternative<T>(value); }
template <class T> [[nodiscard]] inline const T& Value(const Result<T>& value) { return std::get<T>(value); }
template <class T> [[nodiscard]] inline const Error& Failure(const Result<T>& value) { return std::get<Error>(value); }

struct Deadline { std::chrono::steady_clock::time_point value; };
struct MemoryRegion { Address base{}; std::size_t size{}; bool committed{}; bool readable{}; bool writable{}; bool executable{}; std::uint32_t protection{}; };

class VerifiedIdentityFactory;
class VerifiedIdentityTestFactory;
using HandleCloser = void (*)(void*) noexcept;

class ProcessHandle {
public:
  ~ProcessHandle() { Reset(); }
  ProcessHandle(const ProcessHandle&) = delete;
  ProcessHandle& operator=(const ProcessHandle&) = delete;
  ProcessHandle(ProcessHandle&& other) noexcept
      : handle_(std::exchange(other.handle_, nullptr)), closer_(std::exchange(other.closer_, nullptr)) {}
  ProcessHandle& operator=(ProcessHandle&& other) noexcept {
    if (this != &other) {
      Reset();
      handle_ = std::exchange(other.handle_, nullptr);
      closer_ = std::exchange(other.closer_, nullptr);
    }
    return *this;
  }
  void* get() const noexcept { return handle_; }
private:
  ProcessHandle(void* handle, HandleCloser closer) : handle_(handle), closer_(closer) {
    if (handle == nullptr || closer == nullptr) throw std::invalid_argument("invalid_handle_owner");
  }
  void Reset() noexcept {
    if (handle_ != nullptr) closer_(std::exchange(handle_, nullptr));
  }
  void* handle_{};
  HandleCloser closer_{};
  friend class OwnedProcess;
  friend class ObservedProcess;
};

class OwnedProcess {
public:
  ~OwnedProcess() = default;
  OwnedProcess(const OwnedProcess&) = delete;
  OwnedProcess& operator=(const OwnedProcess&) = delete;
  OwnedProcess(OwnedProcess&& other) noexcept
      : handle_(std::move(other.handle_)), pid_(std::exchange(other.pid_, 0)), creation_time_(std::exchange(other.creation_time_, 0)), user_identity_(std::move(other.user_identity_)), session_identity_(std::move(other.session_identity_)), package_identity_(std::move(other.package_identity_)) {}
  OwnedProcess& operator=(OwnedProcess&& other) noexcept {
    if (this != &other) {
      handle_ = std::move(other.handle_);
      pid_ = std::exchange(other.pid_, 0);
      creation_time_ = std::exchange(other.creation_time_, 0);
      user_identity_ = std::move(other.user_identity_);
      session_identity_ = std::move(other.session_identity_);
      package_identity_ = std::move(other.package_identity_);
    }
    return *this;
  }
  void* native_handle() const { return handle_.get(); }
  std::uint32_t pid() const { return pid_; }
  std::uint64_t creation_time() const { return creation_time_; }
  std::string_view user_identity() const { return user_identity_; }
  std::string_view session_identity() const { return session_identity_; }
  std::string_view package_identity() const { return package_identity_; }
private:
  OwnedProcess(void* handle, std::uint32_t pid, std::uint64_t creation_time, std::string user_identity, std::string session_identity, std::string package_identity, HandleCloser closer)
      : handle_(handle, closer), pid_(pid), creation_time_(creation_time), user_identity_(std::move(user_identity)), session_identity_(std::move(session_identity)), package_identity_(std::move(package_identity)) {}
  ProcessHandle handle_;
  std::uint32_t pid_{};
  std::uint64_t creation_time_{};
  std::string user_identity_;
  std::string session_identity_;
  std::string package_identity_;
  friend class VerifiedIdentityFactory;
  friend class VerifiedIdentityTestFactory;
};

class ObservedProcess {
public:
  ~ObservedProcess() = default;
  ObservedProcess(const ObservedProcess&) = delete;
  ObservedProcess& operator=(const ObservedProcess&) = delete;
  ObservedProcess(ObservedProcess&& other) noexcept
      : handle_(std::move(other.handle_)), pid_(std::exchange(other.pid_, 0)), creation_time_(std::exchange(other.creation_time_, 0)) {}
  ObservedProcess& operator=(ObservedProcess&& other) noexcept {
    if (this != &other) {
      handle_ = std::move(other.handle_);
      pid_ = std::exchange(other.pid_, 0);
      creation_time_ = std::exchange(other.creation_time_, 0);
    }
    return *this;
  }
  void* native_handle() const { return handle_.get(); }
  std::uint32_t pid() const { return pid_; }
  std::uint64_t creation_time() const { return creation_time_; }
private:
  ObservedProcess(void* handle, std::uint32_t pid, std::uint64_t creation_time, HandleCloser closer)
      : handle_(handle, closer), pid_(pid), creation_time_(creation_time) {}
  ProcessHandle handle_;
  std::uint32_t pid_{};
  std::uint64_t creation_time_{};
  friend class VerifiedIdentityFactory;
  friend class VerifiedIdentityTestFactory;
};

enum class Availability { Observed, NotObserved, UnsupportedLayout, ReadFailed };

template <class T> struct Observed {
  std::optional<T> value;
  Availability availability{Availability::NotObserved};
  std::string source;
  std::string reason;
  Observed() = default;
  Observed(std::optional<T> observed_value, Availability state, std::string source_id, std::string safe_reason)
      : value(std::move(observed_value)), availability(state), source(source_id.substr(0, 64)), reason(safe_reason.substr(0, 256)) {}
};

struct EnvironmentSnapshot {
  Observed<std::string> os;
  Observed<std::string> gpu;
  Observed<std::string> driver;
  Observed<std::string> selected_adapter;
  Observed<bool> hags_configured;
  Observed<bool> hags_active;
};

enum class Status { Applied, RoutingAlreadyPresent, AlreadyRunning, UnsupportedBuild, UnexpectedImports, UntrustedInterposer, StartupTooLate, AccessDenied, RolledBack, StartupAborted, LaunchFailed, TimedOut, ConfigurationInvalid, DllConflict };
enum class TransactionOutcome { NotStarted, Committed, RolledBack, Failed };
enum class DetachOutcome { NotStarted, Detached, Failed };
struct SanitizedDiagnosticRecord { ErrorCode code{}; Stage stage{Stage::Launch}; std::string operation; std::chrono::system_clock::time_point time{}; };
struct RunResult { Status status{Status::LaunchFailed}; TransactionOutcome transaction{TransactionOutcome::NotStarted}; DetachOutcome detach{DetachOutcome::NotStarted}; SanitizedDiagnosticRecord diagnostic; };

class IMemory {
public:
  virtual ~IMemory() = default;
  virtual Result<Bytes> Read(Address address, std::size_t size) const = 0;
  virtual Result<MemoryRegion> Query(Address address) const = 0;
};
class IWritableMemory : public IMemory {
public:
  virtual Result<Unit> ReadInto(Address address, std::span<std::byte> bytes) const = 0;
  virtual Result<Unit> Write(Address address, std::span<const std::byte> bytes) = 0;
  virtual Result<std::uint32_t> Protect(Address address, std::size_t size, std::uint32_t protection) = 0;
};

}  // namespace wfg
