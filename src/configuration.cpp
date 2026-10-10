#include "wfg/configuration.hpp"
#include "local_storage_windows.hpp"

#include "../third_party/tomlplusplus/toml.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <string>

namespace wfg {
namespace {
constexpr std::size_t MaxConfigBytes = 64 * 1024;
constexpr std::size_t MaxWarnings = 8;

Error InvalidConfig(std::string_view operation) {
  return {ErrorCode::ConfigurationInvalid, Stage::configuration, operation, std::nullopt, std::nullopt};
}

bool AllowedKeys(const toml::table& table, std::span<const std::string_view> allowed) {
  for (const auto& [key, value] : table) {
    static_cast<void>(value);
    if (std::find(allowed.begin(), allowed.end(), key.str()) == allowed.end()) return false;
  }
  return true;
}

template <class T>
bool ReadInteger(const toml::table& table, std::string_view key, T minimum, T maximum, T& output) {
  const auto* node = table.get(key);
  if (!node) return true;
  if (!node->is_integer()) return false;
  const auto value = node->value<std::int64_t>();
  if (!value || *value < minimum || *value > maximum) return false;
  output = static_cast<T>(*value);
  return true;
}

bool ReadBoolean(const toml::table& table, std::string_view key, bool& output) {
  const auto* node = table.get(key);
  if (!node) return true;
  if (!node->is_boolean()) return false;
  const auto value = node->value<bool>();
  if (!value) return false;
  output = *value;
  return true;
}

bool ReadLogLevel(const toml::table& table, std::string_view key, LogLevel& output) {
  const auto* node = table.get(key);
  if (!node) return true;
  if (!node->is_string()) return false;
  const auto value = node->value<std::string>();
  if (!value) return false;
  if (*value == "error") output = LogLevel::Error;
  else if (*value == "info") output = LogLevel::Info;
  else if (*value == "debug") output = LogLevel::Debug;
  else return false;
  return true;
}

void SetOrigin(EffectiveConfig& config, std::string_view key, ConfigOrigin origin) {
  config.origins.insert_or_assign(std::string(key), origin);
}

bool ApplyFile(const toml::table& document, EffectiveConfig& config) {
  constexpr std::array<std::string_view, 3> RootKeys{"schema_version", "logging", "diagnostics"};
  constexpr std::array<std::string_view, 4> LoggingKeys{"level", "file_logging", "max_file_mib", "retained_files"};
  constexpr std::array<std::string_view, 3> DiagnosticsKeys{"runtime_observation", "sample_interval_ms", "observation_seconds"};
  if (!AllowedKeys(document, RootKeys) || !document.get("schema_version")) return false;
  if (!ReadInteger(document, "schema_version", 1U, 1U, config.schema_version)) return false;

  if (const auto* logging_node = document.get("logging")) {
    const auto* logging = logging_node->as_table();
    if (!logging || !AllowedKeys(*logging, LoggingKeys) ||
        !ReadLogLevel(*logging, "level", config.log_level) ||
        !ReadBoolean(*logging, "file_logging", config.file_logging) ||
        !ReadInteger(*logging, "max_file_mib", 1U, 8U, config.max_file_mib) ||
        !ReadInteger(*logging, "retained_files", 1U, 10U, config.retained_files)) return false;
    for (const auto key : LoggingKeys) if (logging->get(key)) SetOrigin(config, std::string("logging.") + std::string(key), ConfigOrigin::File);
  }

  if (const auto* diagnostics_node = document.get("diagnostics")) {
    const auto* diagnostics = diagnostics_node->as_table();
    if (!diagnostics || !AllowedKeys(*diagnostics, DiagnosticsKeys) ||
        !ReadBoolean(*diagnostics, "runtime_observation", config.runtime_observation) ||
        !ReadInteger(*diagnostics, "sample_interval_ms", 1000U, 10000U, config.sample_interval_ms) ||
        !ReadInteger(*diagnostics, "observation_seconds", 5U, 120U, config.observation_seconds)) return false;
    for (const auto key : DiagnosticsKeys) if (diagnostics->get(key)) SetOrigin(config, std::string("diagnostics.") + std::string(key), ConfigOrigin::File);
  }

  SetOrigin(config, "schema_version", ConfigOrigin::File);
  return true;
}

}  // namespace

EffectiveConfig DefaultConfiguration() {
  EffectiveConfig config;
  config.origins = {{"schema_version", ConfigOrigin::Default}, {"logging.level", ConfigOrigin::Default},
                    {"logging.file_logging", ConfigOrigin::Default}, {"logging.max_file_mib", ConfigOrigin::Default},
                    {"logging.retained_files", ConfigOrigin::Default}, {"diagnostics.runtime_observation", ConfigOrigin::Default},
                    {"diagnostics.sample_interval_ms", ConfigOrigin::Default}, {"diagnostics.observation_seconds", ConfigOrigin::Default}};
  config.warnings.reserve(MaxWarnings);
  return config;
}

Result<CommandLine> ParseOptions(std::span<const std::string_view> arguments) {
  CommandLine command_line;
  bool mode_seen = false;
  bool log_level_seen = false;
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    const auto argument = arguments[index];
    if (argument == "--help" || argument == "--version" || argument == "--diagnose" || argument == "--collect-support") {
      if (mode_seen) return InvalidConfig("conflicting_modes");
      mode_seen = true;
      if (argument == "--help") command_line.mode = Mode::Help;
      else if (argument == "--version") command_line.mode = Mode::Version;
      else if (argument == "--diagnose") command_line.mode = Mode::Diagnose;
      else command_line.mode = Mode::CollectSupport;
    } else if (argument == "--log-level") {
      if (log_level_seen || index + 1 >= arguments.size()) return InvalidConfig("invalid_log_level_option");
      log_level_seen = true;
      const auto value = arguments[++index];
      if (value == "error") command_line.log_level = LogLevel::Error;
      else if (value == "info") command_line.log_level = LogLevel::Info;
      else if (value == "debug") command_line.log_level = LogLevel::Debug;
      else return InvalidConfig("invalid_log_level");
    } else {
      return InvalidConfig("unknown_argument");
    }
  }
  return command_line;
}

Result<EffectiveConfig> LoadConfiguration(const std::filesystem::path& exe_dir, const CommandLine& command_line) {
  auto config = DefaultConfiguration();
  const auto volume = local_storage::LocalVolumeRoot(exe_dir);
  if (!volume) return InvalidConfig("config_local_storage_required");
  std::vector<local_storage::NativeHandle> pins;
  if (!local_storage::PinDirectories(exe_dir, *volume, 0, pins)) return InvalidConfig("config_directory_unavailable");
  auto input = local_storage::RelativeOpen(pins.back().get(), L"FrameGenFix.toml", GENERIC_READ, FILE_SHARE_READ, 1);
  if (!input && GetLastError() != ERROR_FILE_NOT_FOUND) return InvalidConfig("config_file_unavailable");
  if (input) {
    BY_HANDLE_FILE_INFORMATION before{}, after{};
    if (!local_storage::RegularSingleLink(input.get(), &before)) return InvalidConfig("config_file_not_regular");
    const auto size = (static_cast<std::uint64_t>(before.nFileSizeHigh) << 32U) | before.nFileSizeLow;
    if (size > MaxConfigBytes) return InvalidConfig("config_file_too_large_or_unavailable");
    std::string contents(static_cast<std::size_t>(size), '\0');
    DWORD read{};
    if (!ReadFile(input.get(), contents.data(), static_cast<DWORD>(size), &read, nullptr) || read != size ||
        !local_storage::RegularSingleLink(input.get(), &after) || !local_storage::Same(before, after) ||
        local_storage::LocalVolumeRoot(exe_dir) != volume || !local_storage::DirectoriesValid(pins))
      return InvalidConfig("config_file_read_failed");
    try {
      const auto document = toml::parse(contents);
      if (!ApplyFile(document, config)) return InvalidConfig("config_schema_invalid");
    } catch (const toml::parse_error&) {
      return InvalidConfig("config_syntax_invalid");
    }
  }
  if (command_line.log_level) {
    config.log_level = *command_line.log_level;
    SetOrigin(config, "logging.level", ConfigOrigin::CommandLine);
  }
  return config;
}

}  // namespace wfg
