#include "wfg/diagnostics.hpp"
#include "local_storage_windows.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <fstream>
#include <limits>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace wfg {
namespace {
constexpr std::size_t MaxSummaryBytes = 16 * 1024;
constexpr std::size_t MaxEventBytes = 4096;

Error DiagnosticError(std::string_view operation = "diagnostic_io", std::optional<std::uint32_t> native_code = std::nullopt) {
  return {ErrorCode::DiagnosticFailure, Stage::runtime_observation, operation, native_code, std::nullopt};
}

std::string_view StageName(Stage stage) {
  switch (stage) {
    case Stage::configuration: return "configuration";
    case Stage::package_discovery: return "package_discovery";
    case Stage::activation: return "activation";
    case Stage::process_identity: return "process_identity";
    case Stage::debug_attach: return "debug_attach";
    case Stage::dependencies: return "dependencies";
    case Stage::build_validation: return "build_validation";
    case Stage::pre_factory_guard: return "pre_factory_guard";
    case Stage::import_transaction: return "import_transaction";
    case Stage::detach: return "detach";
    case Stage::runtime_observation: return "runtime_observation";
  }
  return "unknown";
}

std::string_view OperationName(std::string_view operation) {
  if (operation.empty() || operation.size() > 64 || operation == "invalid_operation") return "redacted";
  for (const auto ch : operation) {
    const bool valid = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                       (ch >= '0' && ch <= '9') || ch == '_';
    if (!valid) return "redacted";
  }
  return operation;
}

std::string UtcNow() {
  const auto now = std::chrono::system_clock::now();
  const auto time = std::chrono::system_clock::to_time_t(now);
  std::tm utc{};
#ifdef _WIN32
  gmtime_s(&utc, &time);
#else
  gmtime_r(&time, &utc);
#endif
  std::array<char, 32> text{};
  const auto count = std::strftime(text.data(), text.size(), "%Y-%m-%dT%H:%M:%SZ", &utc);
  return std::string(text.data(), count);
}

std::string_view CodeFor(ErrorCode code, Stage stage, std::string_view operation) {
  switch (code) {
    case ErrorCode::ConfigurationInvalid: return "WFG-E-CONFIG_INVALID";
    case ErrorCode::MalformedImage:
    case ErrorCode::UnsupportedBuild:
    case ErrorCode::UnexpectedImports: return "WFG-E-BUILD_UNSUPPORTED";
    case ErrorCode::DllConflict: return "WFG-E-DLL_CONFLICT";
    case ErrorCode::UntrustedInterposer: return "WFG-E-INTERPOSER_UNTRUSTED";
    case ErrorCode::AccessDenied: return stage == Stage::debug_attach ? "WFG-E-ATTACH_DENIED" : "WFG-E-LAUNCH_FAILED";
    case ErrorCode::StartupTooLate: return "WFG-E-STARTUP_TOO_LATE";
    case ErrorCode::MutationFailure:
      if (stage != Stage::import_transaction) return "WFG-E-LAUNCH_FAILED";
      if (operation == "IatVerify" || operation == "VerifyIat" || operation == "IAT_VERIFY") return "WFG-E-IAT_VERIFY";
      if (operation == "ProtectionRestore" || operation == "RestoreProtection" || operation == "PROTECTION_RESTORE") return "WFG-E-PROTECTION_RESTORE";
      if (operation == "RollbackAbort" || operation == "AbortAfterRollbackFailure" || operation == "ROLLBACK_ABORT") return "WFG-E-ROLLBACK_ABORT";
      if (operation == "IatWrite" || operation == "WriteIat" || operation == "IAT_WRITE") return "WFG-E-IAT_WRITE";
      return "WFG-E-LAUNCH_FAILED";
    case ErrorCode::UnsupportedObservation: return "WFG-W-OBSERVATION_UNAVAILABLE";
    case ErrorCode::DiagnosticFailure: return "WFG-E-DIAGNOSTIC_IO";
    default: return "WFG-E-LAUNCH_FAILED";
  }
}

std::string_view ActionFor(std::string_view code) {
  if (code == "WFG-E-CONFIG_INVALID") return "Correct or remove FrameGenFix.toml and retry.";
  if (code == "WFG-E-BUILD_UNSUPPORTED") return "Use a supported game build; no imports were changed.";
  if (code == "WFG-E-DLL_CONFLICT") return "Remove conflicting local modifications and retry.";
  if (code == "WFG-E-INTERPOSER_UNTRUSTED") return "Restore the verified signed interposer and retry.";
  if (code == "WFG-E-ATTACH_DENIED") return "Close the game and retry from the current user session.";
  if (code == "WFG-E-STARTUP_TOO_LATE") return "Close the game and launch this tool before the game.";
  if (code == "WFG-E-IAT_VERIFY") return "The import update could not be verified; inspect the recorded startup result.";
  if (code == "WFG-E-PROTECTION_RESTORE") return "Memory protections could not be restored; inspect the recorded startup result.";
  if (code == "WFG-E-ROLLBACK_ABORT") return "Rollback or abort verification failed; inspect the recorded startup result.";
  if (code == "WFG-W-OBSERVATION_UNAVAILABLE") return "Some optional runtime observations were unavailable; this does not indicate rejection.";
  if (code == "WFG-E-DIAGNOSTIC_IO") return "Diagnostics could not be saved; use the console result.";
  if (code == "WFG-E-IAT_WRITE") return "The import update failed; inspect the recorded startup result.";
  return "Retry once; if the problem continues, collect a local support bundle.";
}

bool ValidUtf8(std::string_view value) {
  for (std::size_t index = 0; index < value.size();) {
    const auto lead = static_cast<unsigned char>(value[index]);
    if (lead < 0x80) { ++index; continue; }
    const std::size_t width = lead >= 0xc2 && lead <= 0xdf ? 2 :
                              lead >= 0xe0 && lead <= 0xef ? 3 :
                              lead >= 0xf0 && lead <= 0xf4 ? 4 : 0;
    if (!width || width > value.size() - index) return false;
    for (std::size_t offset = 1; offset < width; ++offset) {
      const auto byte = static_cast<unsigned char>(value[index + offset]);
      if (byte < 0x80 || byte > 0xbf) return false;
    }
    const auto second = static_cast<unsigned char>(value[index + 1]);
    if ((lead == 0xe0 && second < 0xa0) || (lead == 0xed && second > 0x9f) ||
        (lead == 0xf0 && second < 0x90) || (lead == 0xf4 && second > 0x8f)) return false;
    index += width;
  }
  return true;
}

std::string_view SafeObservationText(std::string_view value) noexcept {
  if (value.size() > 96 || !ValidUtf8(value)) return "redacted";
  if (value.find_first_of("/\\:%@$~") != std::string_view::npos) return "redacted";
  for (const unsigned char byte : value) {
    if (byte < 0x20 || byte == 0x7f) return "redacted";
  }
  return value;
}

class JsonWriter {
public:
  explicit JsonWriter(std::size_t limit) : limit_(limit) { output_.reserve(std::min<std::size_t>(limit, 4096)); }
  bool Raw(std::string_view value) {
    if (value.size() > limit_ - std::min(limit_, output_.size())) return false;
    output_.append(value);
    return true;
  }
  bool String(std::string_view value) {
    if (!ValidUtf8(value)) value = "redacted";
    if (!Raw("\"")) return false;
    for (const unsigned char ch : value) {
      if (ch == '"') { if (!Raw("\\\"")) return false; }
      else if (ch == '\\') { if (!Raw("\\\\")) return false; }
      else if (ch == '\b') { if (!Raw("\\b")) return false; }
      else if (ch == '\f') { if (!Raw("\\f")) return false; }
      else if (ch == '\n') { if (!Raw("\\n")) return false; }
      else if (ch == '\r') { if (!Raw("\\r")) return false; }
      else if (ch == '\t') { if (!Raw("\\t")) return false; }
      else if (ch < 0x20) {
        constexpr char Hex[] = "0123456789abcdef";
        const std::array<char, 6> escaped{'\\', 'u', '0', '0', Hex[(ch >> 4) & 0xf], Hex[ch & 0xf]};
        if (!Raw(std::string_view(escaped.data(), escaped.size()))) return false;
      } else {
        const char byte = static_cast<char>(ch);
        if (!Raw(std::string_view(&byte, 1))) return false;
      }
    }
    return Raw("\"");
  }
  bool Number(std::uint64_t value) {
    std::array<char, 32> text{};
    const auto result = std::to_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && Raw(std::string_view(text.data(), static_cast<std::size_t>(result.ptr - text.data())));
  }
  bool Boolean(bool value) { return Raw(value ? "true" : "false"); }
  bool Null() { return Raw("null"); }
  [[nodiscard]] std::string Take() && { return std::move(output_); }
private:
  std::size_t limit_;
  std::string output_;
};

std::string SafeObservationValue(std::string_view value) {
  return std::string(SafeObservationText(value));
}

std::string SafeTag(std::string_view value) {
  if (value.empty() || value.size() > 64) return "unknown";
  for (const auto ch : value) {
    const bool valid = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                       (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.';
    if (!valid) return "redacted";
  }
  return std::string(value);
}

std::string_view AvailabilityName(Availability availability) {
  switch (availability) {
    case Availability::Observed: return "observed";
    case Availability::NotObserved: return "not_observed";
    case Availability::UnsupportedLayout: return "unsupported_layout";
    case Availability::ReadFailed: return "read_failed";
  }
  return "unknown";
}

std::string_view LogLevelName(LogLevel level) {
  switch (level) {
    case LogLevel::Error: return "error";
    case LogLevel::Info: return "info";
    case LogLevel::Debug: return "debug";
  }
  return "error";
}

std::string_view OriginName(ConfigOrigin origin) {
  switch (origin) {
    case ConfigOrigin::Default: return "default";
    case ConfigOrigin::File: return "file";
    case ConfigOrigin::CommandLine: return "command_line";
  }
  return "unknown";
}

std::string_view OutcomeName(RunOutcome outcome) {
  switch (outcome) {
    case RunOutcome::NotStarted: return "not_started";
    case RunOutcome::Applied: return "applied";
    case RunOutcome::RoutingAlreadyPresent: return "routing_already_present";
    case RunOutcome::Refused: return "refused";
    case RunOutcome::RolledBack: return "rolled_back";
    case RunOutcome::StartupAborted: return "startup_aborted";
    case RunOutcome::Failed: return "failed";
  }
  return "unknown";
}

std::string_view HookName(HookState state) {
  switch (state) {
    case HookState::Unknown: return "unknown";
    case HookState::Applied: return "applied";
    case HookState::RoutingAlreadyPresent: return "routing_already_present";
    case HookState::Refused: return "refused";
    case HookState::RolledBack: return "rolled_back";
    case HookState::StartupAborted: return "startup_aborted";
  }
  return "unknown";
}

std::string_view FgName(FrameGenerationState state, bool has_decoded_rejection) {
  switch (state) {
    case FrameGenerationState::Unknown: return "unknown";
    case FrameGenerationState::ActivityNotObserved: return "activity_not_observed";
    case FrameGenerationState::ActivityObserved: return "activity_observed";
    case FrameGenerationState::Rejected: return has_decoded_rejection ? "rejected" : "unknown";
  }
  return "unknown";
}

std::string_view EventLevelName(EventLevel level) {
  switch (level) {
    case EventLevel::Failure: return "error";
    case EventLevel::Information: return "info";
    case EventLevel::DebugDetail: return "debug";
    case EventLevel::CriticalWarning: return "critical_warning";
  }
  return "error";
}

std::string_view EventCodeName(EventCode code, bool decoded_rejection) {
  switch (code) {
    case EventCode::Failure: return "WFG-E-LAUNCH_FAILED";
    case EventCode::Initialization: return "INITIALIZATION";
    case EventCode::StageCompleted: return "STAGE_COMPLETED";
    case EventCode::StateChanged: return "STATE_CHANGED";
    case EventCode::DependencyObserved: return "DEPENDENCY_OBSERVED";
    case EventCode::ObservationUnavailable: return "WFG-W-OBSERVATION_UNAVAILABLE";
    case EventCode::FgActivityNotObserved: return "WFG-W-FG_ACTIVITY_NOT_OBSERVED";
    case EventCode::FgRejected: return decoded_rejection ? "WFG-E-FG_REJECTED" : "WFG-W-OBSERVATION_UNAVAILABLE";
    case EventCode::MutationStep: return "MUTATION_STEP";
  }
  return "UNKNOWN";
}

std::string_view FieldKeyName(DiagnosticFieldKey key) {
  switch (key) {
    case DiagnosticFieldKey::PackageVersion: return "package_version";
    case DiagnosticFieldKey::ExecutableVersion: return "executable_version";
    case DiagnosticFieldKey::InterposerVersion: return "interposer_version";
    case DiagnosticFieldKey::InterposerSignature: return "interposer_signature";
    case DiagnosticFieldKey::ImportCount: return "import_count";
    case DiagnosticFieldKey::FactoryState: return "factory_state";
    case DiagnosticFieldKey::AdapterName: return "adapter_name";
    case DiagnosticFieldKey::FgActivity: return "fg_activity";
    case DiagnosticFieldKey::DecodedRejectionCode: return "decoded_rejection_code";
    case DiagnosticFieldKey::ElapsedMilliseconds: return "elapsed_ms";
  }
  return "unknown";
}

std::string_view FieldSourceName(DiagnosticFieldSource source) {
  switch (source) {
    case DiagnosticFieldSource::Launcher: return "launcher";
    case DiagnosticFieldSource::PackageManifest: return "package_manifest";
    case DiagnosticFieldSource::VersionResource: return "version_resource";
    case DiagnosticFieldSource::Authenticode: return "authenticode";
    case DiagnosticFieldSource::PeInspection: return "pe_inspection";
    case DiagnosticFieldSource::FactoryGuard: return "factory_guard";
    case DiagnosticFieldSource::DxgiObservation: return "dxgi_observation";
    case DiagnosticFieldSource::RuntimeObservation: return "runtime_observation";
  }
  return "unknown";
}

template <class T, class WriteValue>
bool WriteObserved(JsonWriter& writer, const Observed<T>& observed, WriteValue write_value) {
  if (!writer.Raw("{\"availability\":" ) || !writer.String(AvailabilityName(observed.availability)) ||
      !writer.Raw(",\"source\":" ) || !writer.String(SafeTag(observed.source)) ||
      !writer.Raw(",\"reason\":" ) || !writer.String(SafeTag(observed.reason)) || !writer.Raw(",\"value\":")) return false;
  if (observed.availability == Availability::Observed && observed.value) {
    if (!write_value(*observed.value)) return false;
  } else if (!writer.Null()) return false;
  return writer.Raw("}");
}

bool WriteStringValue(JsonWriter& writer, const std::string& value) {
  return writer.String(SafeObservationValue(value));
}

bool WriteBuild(JsonWriter& writer, const BuildIdentity& build) {
  return writer.Raw("{\"version\":" ) && writer.String(SafeObservationValue(build.version)) &&
         writer.Raw(",\"channel\":" ) && writer.String(SafeTag(build.channel)) &&
         writer.Raw(",\"build_type\":" ) && writer.String(SafeTag(build.build_type)) &&
         writer.Raw(",\"commit\":" ) && writer.String(build.commit.empty() ? std::string{} : SafeTag(build.commit)) &&
         writer.Raw(",\"dirty\":" ) && writer.Boolean(build.dirty) &&
         writer.Raw(",\"archive\":" ) && writer.Boolean(build.archive) &&
         writer.Raw(",\"diagnostic_schema\":" ) && writer.Number(build.diagnostic_schema) &&
         writer.Raw(",\"input_digest\":" ) && writer.String(SafeTag(build.input_digest)) && writer.Raw("}");
}

bool ValidUtc(std::string_view time_utc) {
  bool valid_utc = time_utc.size() == 20;
  if (valid_utc) {
    for (std::size_t index = 0; index < time_utc.size(); ++index) {
      const auto character = time_utc[index];
      if (index == 4 || index == 7) valid_utc = valid_utc && character == '-';
      else if (index == 10) valid_utc = valid_utc && character == 'T';
      else if (index == 13 || index == 16) valid_utc = valid_utc && character == ':';
      else if (index == 19) valid_utc = valid_utc && character == 'Z';
      else valid_utc = valid_utc && character >= '0' && character <= '9';
    }
  }
  return valid_utc;
}

bool WriteFailure(JsonWriter& writer, const FailureInfo& failure, std::uint64_t elapsed_ms) {
  return writer.Raw("{\"code\":" ) && writer.String(SafeTag(failure.code)) &&
         writer.Raw(",\"stage\":" ) && writer.String(StageName(failure.stage)) &&
         writer.Raw(",\"operation\":" ) && writer.String(OperationName(failure.operation)) &&
         writer.Raw(",\"time_utc\":" ) && writer.String(ValidUtc(failure.time_utc) ? std::string_view(failure.time_utc) : "unknown") &&
         writer.Raw(",\"elapsed_ms\":" ) && writer.Number(failure.elapsed_ms ? failure.elapsed_ms : elapsed_ms) &&
         writer.Raw(",\"raw_code\":" ) && (failure.raw_code ? writer.Number(*failure.raw_code) : writer.Null()) &&
         writer.Raw(",\"vendor_code\":" ) && (failure.vendor_code ? writer.Number(*failure.vendor_code) : writer.Null()) &&
         writer.Raw(",\"action\":" ) && writer.String(ActionFor(failure.code)) && writer.Raw("}");
}

bool IsVisible(const EffectiveConfig& config, EventLevel level) {
  if (level == EventLevel::Failure || level == EventLevel::CriticalWarning) return true;
  if (level == EventLevel::Information) return config.log_level != LogLevel::Error;
  return config.log_level == LogLevel::Debug;
}

bool HasDecodedRejection(const DiagnosticEvent& event) {
  for (const auto& field : event.fields) {
    if (field.key == DiagnosticFieldKey::DecodedRejectionCode && field.source == DiagnosticFieldSource::RuntimeObservation) {
      const auto* code = std::get_if<std::uint64_t>(&field.value);
      if (code && *code <= std::numeric_limits<std::uint32_t>::max()) return true;
    }
  }
  return false;
}

bool WriteEventJson(JsonWriter& writer, const DiagnosticEvent& event) {
  if (event.fields.size() > 8) return false;
  auto failure = event.code == EventCode::Failure && event.failure ? std::optional<FailureInfo>(ExplainFailure(*event.failure)) : std::nullopt;
  const std::string_view time_utc(event.time_utc.data(), event.time_utc.size());
  if (failure) {
    failure->time_utc.assign(time_utc);
    failure->elapsed_ms = event.elapsed_ms;
    failure->vendor_code = event.vendor_code;
  }
  if (!writer.Raw("{\"level\":" ) || !writer.String(EventLevelName(event.level)) ||
      !writer.Raw(",\"code\":" ) || !writer.String(failure ? std::string_view(failure->code) : EventCodeName(event.code, HasDecodedRejection(event))) ||
      !writer.Raw(",\"stage\":" ) || !writer.String(StageName(failure ? failure->stage : event.stage)) ||
      !writer.Raw(",\"operation\":" ) || !writer.String(OperationName(failure ? std::string_view(failure->operation) : event.operation)) ||
      !writer.Raw(",\"time_utc\":" ) || !writer.String(ValidUtc(time_utc) ? time_utc : "unknown") ||
      !writer.Raw(",\"elapsed_ms\":" ) || !writer.Number(event.elapsed_ms) ||
      !writer.Raw(",\"failure\":" ) || !(failure ? WriteFailure(writer, *failure, event.elapsed_ms) : writer.Null()) ||
      !writer.Raw(",\"fields\":[")) return false;
  for (std::size_t index = 0; index < event.fields.size(); ++index) {
    if (index && !writer.Raw(",")) return false;
    const auto& field = event.fields[index];
    if (!writer.Raw("{\"key\":" ) || !writer.String(FieldKeyName(field.key)) ||
        !writer.Raw(",\"source\":" ) || !writer.String(FieldSourceName(field.source)) || !writer.Raw(",\"value\":")) return false;
    if (const auto* boolean = std::get_if<bool>(&field.value)) {
      if (!writer.Boolean(*boolean)) return false;
    } else if (const auto* integer = std::get_if<std::uint64_t>(&field.value)) {
      if (!writer.Number(*integer)) return false;
    } else if (!writer.String(SafeObservationValue(std::get<std::string_view>(field.value)))) return false;
    if (!writer.Raw("}")) return false;
  }
  return writer.Raw("]}");
}

std::uint32_t NativeError(const std::error_code& error) {
  if (error.value() < 0 || static_cast<std::uint64_t>(error.value()) > std::numeric_limits<std::uint32_t>::max()) return 0;
  return static_cast<std::uint32_t>(error.value());
}

template <std::size_t N>
void CopyFixed(std::array<char, N>& destination, std::uint8_t& length, std::string_view source, std::size_t maximum) noexcept {
  const auto count = std::min({source.size(), maximum, N});
  if (count) std::memcpy(destination.data(), source.data(), count);
  length = static_cast<std::uint8_t>(count);
}

}

const BuildIdentity& EmbeddedBuildIdentity() noexcept { return kEmbeddedBuildIdentity; }

FailureInfo ExplainFailure(const Error& error) {
  const auto operation = error.operation.view();
  const auto code = CodeFor(error.code, error.stage, operation);
  return {std::string(code), error.stage, std::string(OperationName(operation)), UtcNow(), 0,
          error.native_code, std::nullopt, std::string(ActionFor(code))};
}

Result<std::string> SerializeSummary(const DiagnosticRecord& record) {
  JsonWriter writer(MaxSummaryBytes);
  const auto& config = record.effective_config;
  if (!writer.Raw("{\"schema_version\":" ) || !writer.Number(config.schema_version) ||
      !writer.Raw(",\"build\":" ) || !WriteBuild(writer, record.build) ||
      !writer.Raw(",\"configuration\":{\"log_level\":" ) || !writer.String(LogLevelName(config.log_level)) ||
      !writer.Raw(",\"file_logging\":" ) || !writer.Boolean(config.file_logging) ||
      !writer.Raw(",\"max_file_mib\":" ) || !writer.Number(config.max_file_mib) ||
      !writer.Raw(",\"retained_files\":" ) || !writer.Number(config.retained_files) ||
      !writer.Raw(",\"runtime_observation\":" ) || !writer.Boolean(config.runtime_observation) ||
      !writer.Raw(",\"sample_interval_ms\":" ) || !writer.Number(config.sample_interval_ms) ||
      !writer.Raw(",\"observation_seconds\":" ) || !writer.Number(config.observation_seconds) ||
      !writer.Raw(",\"origins\":{")) return DiagnosticError("summary_prefix");

  constexpr std::array<std::string_view, 8> OriginKeys{
      "schema_version", "logging.level", "logging.file_logging", "logging.max_file_mib", "logging.retained_files",
      "diagnostics.runtime_observation", "diagnostics.sample_interval_ms", "diagnostics.observation_seconds"};
  bool first_origin = true;
  for (const auto key : OriginKeys) {
    const auto found = config.origins.find(key);
    if (found == config.origins.end()) continue;
    if (!first_origin && !writer.Raw(",")) return DiagnosticError("summary_origin_separator");
    first_origin = false;
    if (!writer.String(key) || !writer.Raw(":") || !writer.String(OriginName(found->second))) return DiagnosticError("summary_origin_value");
  }
  if (!writer.Raw("},\"warnings\":[")) return DiagnosticError("summary_warning_prefix");
  const auto warning_count = std::min<std::size_t>(config.warnings.size(), 8);
  for (std::size_t index = 0; index < warning_count; ++index) {
    if ((index && !writer.Raw(",")) || !writer.String(SafeTag(config.warnings[index]))) return DiagnosticError("summary_warning_value");
  }
  if (!writer.Raw("]},\"game\":{\"package\":" ) ||
      !WriteObserved(writer, record.game_package, [&](const auto& value) { return WriteStringValue(writer, value); }) ||
      !writer.Raw(",\"version\":" ) || !WriteObserved(writer, record.game_version, [&](const auto& value) { return WriteStringValue(writer, value); }) ||
      !writer.Raw(",\"executable_timestamp\":" ) || !WriteObserved(writer, record.executable_timestamp, [&](auto value) { return writer.Number(value); }) ||
      !writer.Raw("}")) return DiagnosticError("summary_game");
  if (!writer.Raw(",\"environment\":{\"os\":" ) || !WriteObserved(writer, record.environment.os, [&](const auto& value) { return WriteStringValue(writer, value); }) ||
      !writer.Raw(",\"gpu\":" ) || !WriteObserved(writer, record.environment.gpu, [&](const auto& value) { return WriteStringValue(writer, value); }) ||
      !writer.Raw(",\"driver\":" ) || !WriteObserved(writer, record.environment.driver, [&](const auto& value) { return WriteStringValue(writer, value); }) ||
      !writer.Raw(",\"selected_adapter\":" ) || !WriteObserved(writer, record.environment.selected_adapter, [&](const auto& value) { return WriteStringValue(writer, value); }) ||
      !writer.Raw(",\"hags_configured\":" ) || !WriteObserved(writer, record.environment.hags_configured, [&](bool value) { return writer.Boolean(value); }) ||
      !writer.Raw(",\"hags_active\":" ) || !WriteObserved(writer, record.environment.hags_active, [&](bool value) { return writer.Boolean(value); }) ||
      !writer.Raw("}")) return DiagnosticError("summary_environment");
  if (!writer.Raw(",\"dependencies\":{\"interposer_version\":" ) || !WriteObserved(writer, record.interposer_version, [&](const auto& value) { return WriteStringValue(writer, value); }) ||
      !writer.Raw(",\"interposer_sha256\":" ) || !WriteObserved(writer, record.interposer_sha256, [&](const auto& value) { return writer.String(SafeTag(value)); }) ||
      !writer.Raw(",\"interposer_signature_valid\":" ) || !WriteObserved(writer, record.interposer_signature_valid, [&](bool value) { return writer.Boolean(value); }) ||
      !writer.Raw(",\"loaded_plugin\":" ) || !WriteObserved(writer, record.loaded_plugin, [&](bool value) { return writer.Boolean(value); }) ||
      !writer.Raw("}")) return DiagnosticError("summary_dependencies");
  if (!writer.Raw(",\"hook_state\":" ) || !writer.String(HookName(record.hook_state))) return DiagnosticError("summary_hook_state");
  if (!writer.Raw(",\"frame_generation\":" ) || !writer.String(FgName(record.frame_generation, record.decoded_rejection_code.has_value()))) return DiagnosticError("summary_frame_generation");
  if (!writer.Raw(",\"decoded_rejection_code\":" ) ||
      !(record.frame_generation == FrameGenerationState::Rejected && record.decoded_rejection_code ? writer.Number(*record.decoded_rejection_code) : writer.Null())) return DiagnosticError("summary_decoded_rejection");
  if (!writer.Raw(",\"warning_codes\":[")) return DiagnosticError("summary_warning_codes");

  bool first_warning = true;
  if (record.frame_generation == FrameGenerationState::Unknown ||
      (record.frame_generation == FrameGenerationState::Rejected && !record.decoded_rejection_code)) {
    if (!writer.String("WFG-W-OBSERVATION_UNAVAILABLE")) return DiagnosticError("summary_unavailable_warning");
    first_warning = false;
  } else if (record.frame_generation == FrameGenerationState::ActivityNotObserved) {
    if (!writer.String("WFG-W-FG_ACTIVITY_NOT_OBSERVED")) return DiagnosticError("summary_activity_warning");
    first_warning = false;
  } else if (record.frame_generation == FrameGenerationState::Rejected && record.decoded_rejection_code) {
    if (!writer.String("WFG-E-FG_REJECTED")) return DiagnosticError("summary_rejected_warning");
    first_warning = false;
  }
  if (record.failure && record.failure->code == "WFG-W-OBSERVATION_UNAVAILABLE") {
    if (!first_warning && !writer.Raw(",")) return DiagnosticError("summary_warning_separator");
    if (!writer.String(record.failure->code)) return DiagnosticError("summary_failure_warning");
  }
  if (!writer.Raw("],\"outcome\":" ) || !writer.String(OutcomeName(record.outcome)) ||
      !writer.Raw(",\"elapsed_ms\":" ) || !writer.Number(record.elapsed_ms) ||
      !writer.Raw(",\"failure\":" ) || !(record.failure ? WriteFailure(writer, *record.failure, record.elapsed_ms) : writer.Null()) || !writer.Raw("}")) {
    return DiagnosticError("summary_tail");
  }
  return std::move(writer).Take();
}

struct FileLogStorage::State {
  std::vector<local_storage::NativeHandle> pins;
  std::optional<std::wstring> volume;
  local_storage::DirectoryLease lease;
  bool Ensure(const std::filesystem::path& path) {
    const auto current = local_storage::LocalVolumeRoot(path);
    if (!current || (volume && *current != *volume)) { SetLastError(ERROR_NOT_SUPPORTED); return false; }
    if (pins.empty()) {
      volume = current;
      // Only the product root and diagnostics leaf may be created.
      const auto tail = path.filename() == L"diagnostics" && path.parent_path().filename() == L"WitcherFG" ? 2U : 1U;
      if (!local_storage::PinDirectories(path, *volume, tail, pins, true)) { pins.clear(); return false; }
    }
    if (!local_storage::DirectoriesValid(pins)) return false;
    if(!lease.Acquire(pins.back().get()))return false;
    return true;
  }
  HANDLE directory() const { return pins.back().get(); }
};
FileLogStorage::FileLogStorage(std::filesystem::path directory) : state_(std::make_unique<State>()), directory_(std::move(directory)) {}
FileLogStorage::~FileLogStorage() = default;
const std::filesystem::path& FileLogStorage::directory() const noexcept { return directory_; }

Result<Unit> FileLogStorage::AppendEvent(std::string_view line, std::uint32_t max_file_mib, std::uint32_t retained_files) {
  const auto limit = static_cast<std::uint64_t>(std::clamp(max_file_mib,1U,8U))*1024U*1024U;
  const auto retained = std::clamp(retained_files,1U,10U);
  if (line.size()+1 > limit) return DiagnosticError("event_too_large");
  if (!state_->Ensure(directory_)) return DiagnosticError("storage_local_or_writer_refused",GetLastError());
  auto writer=local_storage::RelativeOpen(state_->directory(),L"events.jsonl",FILE_APPEND_DATA|FILE_READ_ATTRIBUTES|DELETE,FILE_SHARE_READ,3);
  if(!writer||!local_storage::RegularSingleLink(writer.get()))return DiagnosticError("storage_file_refused",GetLastError());
  const auto name=[](unsigned i){return i ? L"events."+std::to_wstring(i)+L".jsonl" : std::wstring(L"events.jsonl");};
  std::array<local_storage::NativeHandle,10> files;
  std::array<BY_HANDLE_FILE_INFORMATION,10> info{};
  if (!local_storage::RegularSingleLink(writer.get(), &info[0])) return DiagnosticError("storage_file_info",GetLastError());
  for (unsigned i=1;i<10;++i) {
    files[i]=local_storage::RelativeOpen(state_->directory(),name(i),FILE_READ_ATTRIBUTES|DELETE,FILE_SHARE_READ,1);
    if (!files[i] && GetLastError()!=ERROR_FILE_NOT_FOUND) return DiagnosticError("storage_open",GetLastError());
    if (files[i] && !local_storage::RegularSingleLink(files[i].get(),&info[i])) return DiagnosticError("storage_file_refused",GetLastError());
  }
  const auto size=[&](unsigned i){return (static_cast<std::uint64_t>(info[i].nFileSizeHigh)<<32U)|info[i].nFileSizeLow;};
  for (unsigned i=1;i<10;++i) if(files[i]&&(i>=retained||size(i)>limit)) {
    if(!local_storage::DeleteFileHandle(files[i].get()))return DiagnosticError("storage_prune",GetLastError()); files[i]={};
  }
  if (size(0)+line.size()+1 > limit) {
    if(size(0)>limit||retained==1) {
      if(!local_storage::DeleteFileHandle(writer.get()))return DiagnosticError("storage_rotate",GetLastError());
    } else {
      for(unsigned i=retained-1;i>0;--i) {
        if(files[i]) {if(!local_storage::DeleteFileHandle(files[i].get()))return DiagnosticError("storage_rotate",GetLastError());files[i]={};}
        HANDLE from = i==1 ? writer.get() : files[i-1].get();
        if(from!=INVALID_HANDLE_VALUE&&!local_storage::RenameWithin(from,state_->directory(),name(i),false)) return DiagnosticError("storage_rotate",GetLastError());
        if(i>1)files[i-1]={};
      }
    }
    writer={};
    writer=local_storage::RelativeOpen(state_->directory(),L"events.jsonl",FILE_APPEND_DATA|FILE_READ_ATTRIBUTES|DELETE,FILE_SHARE_READ,2);
    if(!writer)return DiagnosticError("storage_open",GetLastError());
  }
  if(!local_storage::WriteBytes(writer.get(),line)||!local_storage::WriteBytes(writer.get(),"\n"))return DiagnosticError("storage_write",GetLastError());
  return Unit{};
}
Result<Unit> FileLogStorage::WriteSummaryAtomically(std::string_view json) {
  if(json.size()>MaxSummaryBytes)return DiagnosticError("summary_too_large");
  if(!state_->Ensure(directory_))return DiagnosticError("storage_local_or_writer_refused",GetLastError());
  // Hold any existing destination against replacement/deletion while validating it.
  auto destination=local_storage::RelativeOpen(state_->directory(),L"last-run.json",FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_DELETE,1);
  if(!destination&&GetLastError()!=ERROR_FILE_NOT_FOUND)return DiagnosticError("summary_destination",GetLastError());
  if(destination&&!local_storage::RegularSingleLink(destination.get()))return DiagnosticError("storage_file_refused",GetLastError());
  auto temporary=local_storage::RelativeOpen(state_->directory(),L"last-run.json.tmp",GENERIC_WRITE|DELETE|FILE_READ_ATTRIBUTES,0,2);
  if(!temporary)return DiagnosticError("summary_open",GetLastError());
  const auto fail=[&](){const auto error=GetLastError();local_storage::DeleteFileHandle(temporary.get());return DiagnosticError("summary_write",error);};
  if(!local_storage::RegularSingleLink(temporary.get())||!local_storage::WriteBytes(temporary.get(),json)||!FlushFileBuffers(temporary.get()))return fail();
  // Native rename replaces a directory entry, never follows the destination.
  // An open destination prevents replacement on Windows even with delete sharing.
  destination = {};
  if(!state_->Ensure(directory_)||!local_storage::RenameWithin(temporary.get(),state_->directory(),L"last-run.json",true))return fail();
  return Unit{};
}

void FileLogStorage::WriteConsoleLine(std::string_view text) {
  static_cast<void>(std::fwrite(text.data(), 1, text.size(), stderr));
  if (text.empty() || text.back() != '\n') static_cast<void>(std::fwrite("\n", 1, 1, stderr));
}

Result<std::filesystem::path> LocalAppDataDiagnosticsDirectory() {
  const auto root=local_storage::LocalAppDataCandidate();
  if(!root)return DiagnosticError("local_app_data_unavailable");
  return *root / L"WitcherFG" / L"diagnostics";
}

RunLogger::RunLogger(const EffectiveConfig& config, ILogStorage& storage) noexcept : config_(config), storage_(storage) {}

Result<Unit> RunLogger::Emit(const DiagnosticEvent& event) {
  if (mutation_active_) return StoreDeferred(event);
  if (!IsVisible(config_, event.level)) return Unit{};
  return WriteEvent(event);
}

Result<Unit> RunLogger::DeferMutationEvent(const DiagnosticEvent& event) noexcept {
  mutation_active_ = true;
  return StoreDeferred(event);
}

Result<Unit> RunLogger::StoreDeferred(const DiagnosticEvent& event) noexcept {
  if (!IsVisible(config_, event.level)) return Unit{};
  if (deferred_count_ >= deferred_.size() || event.fields.size() > MaxDeferredFields) return DiagnosticError();
  auto& stored = deferred_[deferred_count_++];
  stored.level = event.level;
  stored.code = event.code;
  stored.stage = event.stage;
  CopyFixed(stored.operation.bytes, stored.operation.length, OperationName(event.operation), MaxOperationBytes);
  stored.time_utc = event.time_utc;
  stored.elapsed_ms = event.elapsed_ms;
  stored.failure = event.failure;
  stored.vendor_code = event.vendor_code;
  stored.field_count = static_cast<std::uint8_t>(event.fields.size());
  for (std::size_t index = 0; index < event.fields.size(); ++index) {
    const auto& field = event.fields[index];
    auto& target = stored.fields[index];
    target.key = field.key;
    target.source = field.source;
    if (const auto* boolean = std::get_if<bool>(&field.value)) {
      target.is_text = false;
      target.is_integer = false;
      target.boolean_value = *boolean;
      target.integer_value = 0;
    } else if (const auto* integer = std::get_if<std::uint64_t>(&field.value)) {
      target.is_text = false;
      target.is_integer = true;
      target.boolean_value = false;
      target.integer_value = *integer;
    } else {
      target.is_text = true;
      target.is_integer = false;
      target.boolean_value = false;
      target.integer_value = 0;
      CopyFixed(target.text.bytes, target.text.length, SafeObservationText(std::get<std::string_view>(field.value)), MaxFieldBytes);
    }
  }
  return Unit{};
}

Result<Unit> RunLogger::WriteEvent(const DiagnosticEvent& event) {
  JsonWriter writer(MaxEventBytes);
  if (!WriteEventJson(writer, event)) return DiagnosticError("event_too_large");
  auto line = std::move(writer).Take();
  if (!config_.file_logging) {
    storage_.WriteConsoleLine(line);
    return Unit{};
  }
  auto result = storage_.AppendEvent(line, std::clamp(config_.max_file_mib, 1U, 8U), std::clamp(config_.retained_files, 1U, 10U));
  if (!IsOk(result)) storage_.WriteConsoleLine(line);
  return result;
}

Result<Unit> RunLogger::FlushDeferred() {
  Error first_error{};
  bool failed = false;
  for (std::size_t index = 0; index < deferred_count_; ++index) {
    auto& stored = deferred_[index];
    std::array<DiagnosticField, MaxDeferredFields> fields{};
    std::array<std::string_view, MaxDeferredFields> text_values{};
    for (std::size_t field_index = 0; field_index < stored.field_count; ++field_index) {
      const auto& saved = stored.fields[field_index];
      fields[field_index].key = saved.key;
      fields[field_index].source = saved.source;
      if (saved.is_text) {
        text_values[field_index] = std::string_view(saved.text.bytes.data(), saved.text.length);
        fields[field_index].value = text_values[field_index];
      } else if (saved.is_integer) {
        fields[field_index].value = saved.integer_value;
      } else {
        fields[field_index].value = saved.boolean_value;
      }
    }
    DiagnosticEvent event{stored.level, stored.code, stored.stage,
                          std::string_view(stored.operation.bytes.data(), stored.operation.length),
                          std::span<const DiagnosticField>(fields.data(), stored.field_count)};
    event.time_utc = stored.time_utc;
    event.elapsed_ms = stored.elapsed_ms;
    event.failure = stored.failure;
    event.vendor_code = stored.vendor_code;
    if (!IsVisible(config_, event.level)) continue;
    auto result = WriteEvent(event);
    if (!IsOk(result) && !failed) {
      first_error = Failure(result);
      failed = true;
    }
  }
  deferred_count_ = 0;
  mutation_active_ = false;
  if (failed) return first_error;
  return Unit{};
}

Result<Unit> RunLogger::Finish(const DiagnosticRecord& record) {
  if (record.safety == SafetyDisposition::Unverified) return DiagnosticError();
  const auto flushed = FlushDeferred();
  const auto summary = SerializeSummary(record);
  if (!IsOk(summary)) {
    if (!IsOk(flushed)) return Failure(flushed);
    return Failure(summary);
  }
  const auto& json = Value(summary);
  Result<Unit> summary_result = Unit{};
  if (config_.file_logging) {
    summary_result = storage_.WriteSummaryAtomically(json);
    if (!IsOk(summary_result)) storage_.WriteConsoleLine(json);
  } else {
    storage_.WriteConsoleLine(json);
  }
  if (!IsOk(flushed)) return Failure(flushed);
  return summary_result;
}

}
