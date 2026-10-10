#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace wfg {

using Address = std::uint64_t;
using Rva = std::uint32_t;
using Bytes = std::vector<std::byte>;
using Hash256 = std::array<std::byte, 32>;
using Unit = std::monostate;

enum class Stage { Inspect, Compatibility, Discover, Debug, Transaction, Diagnostic, Configuration, Support };
enum class ErrorCode {
  MalformedImage, UnsupportedBuild, UnexpectedImports, UntrustedInterposer, UnreadableMemory,
  ModuleNotReady, NoMatchingProcess, IdentityChanged, AmbiguousProcess, AlreadyRunning,
  StartupTooLate, AccessDenied, LaunchFailed, TimedOut, DebugFailure, MutationFailure,
  DiagnosticFailure, ConfigurationInvalid, DllConflict, UnsupportedObservation, SupportBundleFailure
};

struct Error {
  ErrorCode code{};
  Stage stage{Stage::Inspect};
  std::string operation;
  std::optional<std::uint32_t> native_code;
  std::optional<std::size_t> affected_slot;
};

template <class T> using Result = std::variant<T, Error>;
template <class T> [[nodiscard]] inline bool IsOk(const Result<T>& value) { return std::holds_alternative<T>(value); }
template <class T> [[nodiscard]] inline const T& Value(const Result<T>& value) { return std::get<T>(value); }
template <class T> [[nodiscard]] inline const Error& Failure(const Result<T>& value) { return std::get<Error>(value); }

struct Deadline { std::chrono::steady_clock::time_point value; };
struct MemoryRegion { Address base{}; std::size_t size{}; bool committed{}; bool readable{}; bool writable{}; bool executable{}; std::uint32_t protection{}; };

class IMemory {
public:
  virtual ~IMemory() = default;
  virtual Result<Bytes> Read(Address address, std::size_t size) const = 0;
  virtual Result<MemoryRegion> Query(Address address) const = 0;
};
class IWritableMemory : public IMemory {
public:
  virtual Result<Unit> Write(Address address, std::span<const std::byte> bytes) = 0;
  virtual Result<std::uint32_t> Protect(Address address, std::size_t size, std::uint32_t protection) = 0;
};

}  // namespace wfg
