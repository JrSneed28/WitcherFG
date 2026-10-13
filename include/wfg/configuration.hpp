#pragma once

#include "wfg/core.hpp"

#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wfg {

enum class LogLevel { Error, Info, Debug };
enum class Mode { Launch, Help, Version, Diagnose, CollectSupport };
enum class ConfigOrigin { Default, File, CommandLine };

struct CommandLine {
  Mode mode{Mode::Launch};
  std::optional<LogLevel> log_level;
  std::optional<std::string> game_path;
};

struct EffectiveConfig {
  std::uint32_t schema_version{1};
  LogLevel log_level{LogLevel::Error};
  bool file_logging{true};
  std::uint32_t max_file_mib{2};
  std::uint32_t retained_files{5};
  bool runtime_observation{false};
  std::uint32_t sample_interval_ms{1000};
  std::uint32_t observation_seconds{30};
  std::map<std::string, ConfigOrigin, std::less<>> origins;
  std::vector<std::string> warnings;
};

[[nodiscard]] EffectiveConfig DefaultConfiguration();
[[nodiscard]] Result<CommandLine> ParseOptions(std::span<const std::string_view> arguments);
[[nodiscard]] Result<EffectiveConfig> LoadConfiguration(const std::filesystem::path& exe_dir, const CommandLine& command_line);

}
