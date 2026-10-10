#pragma once

#include "wfg/build_info.hpp"
#include "wfg/configuration.hpp"
#include "wfg/core.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>

namespace wfg {

enum class EventLevel { Failure, Information, DebugDetail, CriticalWarning };
enum class EventCode { Failure, Initialization, StageCompleted, StateChanged, DependencyObserved, ObservationUnavailable, FgActivityNotObserved, FgRejected, MutationStep };
enum class DiagnosticFieldKey { PackageVersion, ExecutableVersion, InterposerVersion, InterposerSignature, ImportCount, FactoryState, AdapterName, FgActivity, DecodedRejectionCode, ElapsedMilliseconds };
enum class DiagnosticFieldSource { Launcher, PackageManifest, VersionResource, Authenticode, PeInspection, FactoryGuard, DxgiObservation, RuntimeObservation };
enum class SafetyDisposition { Unverified, VerifiedNoMutation, VerifiedCommit, VerifiedRollback, VerifiedAbort };
enum class RunOutcome { NotStarted, Applied, RoutingAlreadyPresent, Refused, RolledBack, StartupAborted, Failed };
enum class HookState { Unknown, Applied, RoutingAlreadyPresent, Refused, RolledBack, StartupAborted };
enum class FrameGenerationState { Unknown, ActivityNotObserved, ActivityObserved, Rejected };

using DiagnosticFieldValue = std::variant<bool, std::uint64_t, std::string_view>;

struct DiagnosticField {
  DiagnosticFieldKey key{};
  DiagnosticFieldSource source{};
  DiagnosticFieldValue value{false};
};

struct DiagnosticEvent {
  EventLevel level{EventLevel::Information};
  EventCode code{EventCode::Initialization};
  Stage stage{Stage::activation};
  std::string_view operation;
  std::span<const DiagnosticField> fields;
  std::array<char, 20> time_utc{};
  std::uint64_t elapsed_ms{};
  std::optional<Error> failure;
  std::optional<std::uint32_t> vendor_code;
};

struct FailureInfo {
  std::string code;
  Stage stage{Stage::activation};
  std::string operation;
  std::string time_utc;
  std::uint64_t elapsed_ms{};
  std::optional<std::uint32_t> raw_code;
  std::optional<std::uint32_t> vendor_code;
  std::string action;
};

struct DiagnosticRecord {
  BuildIdentity build{kEmbeddedBuildIdentity};
  EffectiveConfig effective_config{DefaultConfiguration()};
  Observed<std::string> game_package;
  Observed<std::string> game_version;
  Observed<std::uint32_t> executable_timestamp;
  Observed<std::string> interposer_version;
  Observed<std::string> interposer_sha256;
  Observed<bool> interposer_signature_valid;
  Observed<bool> loaded_plugin;
  EnvironmentSnapshot environment;
  HookState hook_state{HookState::Unknown};
  FrameGenerationState frame_generation{FrameGenerationState::Unknown};
  std::optional<std::uint32_t> decoded_rejection_code;
  RunOutcome outcome{RunOutcome::NotStarted};
  SafetyDisposition safety{SafetyDisposition::Unverified};
  std::uint64_t elapsed_ms{};
  std::optional<FailureInfo> failure;
};

class ILogStorage {
public:
  virtual ~ILogStorage() = default;
  virtual Result<Unit> AppendEvent(std::string_view json_line, std::uint32_t max_file_mib, std::uint32_t retained_files) = 0;
  virtual Result<Unit> WriteSummaryAtomically(std::string_view json) = 0;
  virtual void WriteConsoleLine(std::string_view text) = 0;
};

class FileLogStorage final : public ILogStorage {
public:
  explicit FileLogStorage(std::filesystem::path directory);
  Result<Unit> AppendEvent(std::string_view json_line, std::uint32_t max_file_mib, std::uint32_t retained_files) override;
  Result<Unit> WriteSummaryAtomically(std::string_view json) override;
  void WriteConsoleLine(std::string_view text) override;
  [[nodiscard]] const std::filesystem::path& directory() const noexcept;
private:
  std::filesystem::path directory_;
};

[[nodiscard]] Result<std::filesystem::path> LocalAppDataDiagnosticsDirectory();
[[nodiscard]] const BuildIdentity& EmbeddedBuildIdentity() noexcept;
[[nodiscard]] FailureInfo ExplainFailure(const Error& error);
[[nodiscard]] Result<std::string> SerializeSummary(const DiagnosticRecord& record);

class RunLogger {
public:
  RunLogger(const EffectiveConfig& config, ILogStorage& storage) noexcept;
  [[nodiscard]] Result<Unit> Emit(const DiagnosticEvent& event);
  [[nodiscard]] Result<Unit> DeferMutationEvent(const DiagnosticEvent& event) noexcept;
  [[nodiscard]] Result<Unit> Finish(const DiagnosticRecord& record);
private:
  static constexpr std::size_t MaxDeferredEvents = 32;
  static constexpr std::size_t MaxDeferredFields = 8;
  static constexpr std::size_t MaxOperationBytes = 64;
  static constexpr std::size_t MaxFieldBytes = 96;

  struct FixedText {
    std::array<char, MaxFieldBytes> bytes{};
    std::uint8_t length{};
  };
  struct DeferredField {
    DiagnosticFieldKey key{};
    DiagnosticFieldSource source{};
    bool is_text{};
    bool is_integer{};
    bool boolean_value{};
    std::uint64_t integer_value{};
    FixedText text;
  };
  struct DeferredEvent {
    EventLevel level{};
    EventCode code{};
    Stage stage{};
    FixedText operation;
    std::array<DeferredField, MaxDeferredFields> fields{};
    std::uint8_t field_count{};
    std::array<char, 20> time_utc{};
    std::uint64_t elapsed_ms{};
    std::optional<Error> failure;
    std::optional<std::uint32_t> vendor_code;
  };

  [[nodiscard]] Result<Unit> StoreDeferred(const DiagnosticEvent& event) noexcept;
  [[nodiscard]] Result<Unit> WriteEvent(const DiagnosticEvent& event);
  [[nodiscard]] Result<Unit> FlushDeferred();

  const EffectiveConfig& config_;
  ILogStorage& storage_;
  std::array<DeferredEvent, MaxDeferredEvents> deferred_{};
  std::size_t deferred_count_{};
  bool mutation_active_{};
};

}
