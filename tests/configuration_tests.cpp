#include "support.hpp"
#include "wfg/configuration.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace {
class ConfigFixture {
public:
  ConfigFixture() {
    path = std::filesystem::temp_directory_path() / "wfg-configuration-tests";
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path / "exe");
    std::filesystem::create_directories(path / "hostile");
  }
  ~ConfigFixture() { std::filesystem::remove_all(path); }
  void Write(std::string_view content) const {
    std::ofstream file(path / "exe" / "FrameGenFix.toml", std::ios::binary);
    file.write(content.data(), static_cast<std::streamsize>(content.size()));
  }
  std::filesystem::path path;
};

constexpr std::string_view ValidConfig = R"(schema_version = 1
[logging]
level = "debug"
file_logging = false
max_file_mib = 8
retained_files = 10
[diagnostics]
runtime_observation = true
sample_interval_ms = 10000
observation_seconds = 120
)";
}

TEST("configuration.defaults_are_exact") {
  const auto config = wfg::DefaultConfiguration();
  CHECK(config.schema_version == 1);
  CHECK(config.log_level == wfg::LogLevel::Error);
  CHECK(config.file_logging);
  CHECK(config.max_file_mib == 2);
  CHECK(config.retained_files == 5);
  CHECK(!config.runtime_observation);
  CHECK(config.sample_interval_ms == 1000);
  CHECK(config.observation_seconds == 30);
}

TEST("configuration.missing_file_uses_executable_relative_defaults") {
  ConfigFixture fixture;
  std::ofstream hostile(fixture.path / "hostile" / "FrameGenFix.toml");
  hostile << "schema_version=1\n[logging]\nlevel='unsafe'\n";
  hostile.close();
  const auto original = std::filesystem::current_path();
  std::filesystem::current_path(fixture.path / "hostile");
  const auto result = wfg::LoadConfiguration(fixture.path / "exe", {});
  std::filesystem::current_path(original);
  CHECK(wfg::IsOk(result));
  if (wfg::IsOk(result)) CHECK(wfg::Value(result).max_file_mib == 2);
}

TEST("configuration.valid_file_is_loaded_from_executable_directory") {
  ConfigFixture fixture;
  fixture.Write(ValidConfig);
  std::ofstream hostile(fixture.path / "hostile" / "FrameGenFix.toml");
  hostile << "schema_version=1\n[logging]\nlevel='error'\n";
  hostile.close();
  const auto original = std::filesystem::current_path();
  std::filesystem::current_path(fixture.path / "hostile");
  const auto result = wfg::LoadConfiguration(fixture.path / "exe", {});
  std::filesystem::current_path(original);
  CHECK(wfg::IsOk(result));
  if (wfg::IsOk(result)) CHECK(wfg::Value(result).log_level == wfg::LogLevel::Debug);
}

TEST("configuration.valid_file_applies_all_allowlisted_values") {
  ConfigFixture fixture;
  fixture.Write(ValidConfig);
  const auto result = wfg::LoadConfiguration(fixture.path / "exe", {});
  CHECK(wfg::IsOk(result));
  if (!wfg::IsOk(result)) return;
  const auto& config = wfg::Value(result);
  CHECK(config.log_level == wfg::LogLevel::Debug);
  CHECK(!config.file_logging);
  CHECK(config.max_file_mib == 8);
  CHECK(config.retained_files == 10);
  CHECK(config.runtime_observation);
  CHECK(config.sample_interval_ms == 10000);
  CHECK(config.observation_seconds == 120);
  CHECK(config.origins.size() == 8);
  CHECK(config.origins.at("logging.level") == wfg::ConfigOrigin::File);
}

TEST("configuration.cli_log_level_overrides_file_only") {
  ConfigFixture fixture;
  fixture.Write(ValidConfig);
  const wfg::CommandLine command{wfg::Mode::Launch, wfg::LogLevel::Info};
  const auto result = wfg::LoadConfiguration(fixture.path / "exe", command);
  CHECK(wfg::IsOk(result));
  if (wfg::IsOk(result)) {
    CHECK(wfg::Value(result).log_level == wfg::LogLevel::Info);
    CHECK(wfg::Value(result).origins.at("logging.level") == wfg::ConfigOrigin::CommandLine);
  }
}

TEST("configuration.rejects_invalid_files_before_activation") {
  const std::vector<std::string> invalid_files{
      "", "[logging]\nlevel='error'\n", "schema_version=2\n", "schema_version=1\n[logging]\nlevel='verbose'\n",
      "schema_version=1\n[logging]\nlevel='error'\nlevel='debug'\n",
      "schema_version=1\n[logging]\nunsafe=true\n", "schema_version=1\n[logging]\nmax_file_mib=0\n",
      "schema_version=1\n[logging]\nmax_file_mib=9\n", "schema_version=1\n[logging]\nretained_files=0\n",
      "schema_version=1\n[logging]\nretained_files=11\n", "schema_version=1\n[logging]\nlevel=1\n",
      "schema_version=1\n[logging]\nmax_file_mib=2.0\n",
      "schema_version=1\n[logging]\nretained_files='5'\n",
      "schema_version=1\n[diagnostics]\nsample_interval_ms='1000'\n",
      "schema_version=1\n[diagnostics]\nobservation_seconds=30.0\n",
      "schema_version=1\n[diagnostics]\nsample_interval_ms=10001\n",
      "schema_version=1\n[diagnostics]\nsample_interval_ms=999\n",
      "schema_version=1\n[diagnostics]\nobservation_seconds=4\n",
      "schema_version=1\n[diagnostics]\nobservation_seconds=121\n",
      "schema_version=1\n[logging]\nfile_logging=1\n",
      "schema_version='1'\n", "schema_version=1\n[diagnostics]\nruntime_observation='true'\n",
      "schema_version = [\n",
      "schema_version=1\n[launcher]\nmodule='evil.dll'\n",
      "schema_version=1\n[launcher]\naddress=123\n",
      "schema_version=1\n[launcher]\nbypass_guard=true\n",
      "schema_version=1\n" + std::string(65537, ' ')};
  ConfigFixture fixture;
  for (const auto& content : invalid_files) {
    fixture.Write(content);
    const auto result = wfg::LoadConfiguration(fixture.path / "exe", {});
    if (wfg::IsOk(result)) throw std::runtime_error("unexpected config acceptance: " + content);
    CHECK(wfg::Failure(result).code == wfg::ErrorCode::ConfigurationInvalid);
  }
}

TEST("configuration.accepts_all_numeric_range_endpoints") {
  ConfigFixture fixture;
  fixture.Write("schema_version=1\n[logging]\nmax_file_mib=1\nretained_files=1\n[diagnostics]\nsample_interval_ms=1000\nobservation_seconds=5\n");
  auto low = wfg::LoadConfiguration(fixture.path / "exe", {});
  CHECK(wfg::IsOk(low));
  fixture.Write("schema_version=1\n[logging]\nmax_file_mib=8\nretained_files=10\n[diagnostics]\nsample_interval_ms=10000\nobservation_seconds=120\n");
  auto high = wfg::LoadConfiguration(fixture.path / "exe", {});
  CHECK(wfg::IsOk(high));
}

TEST("configuration.parses_only_supported_modes_and_log_level") {
  CHECK(wfg::Value(wfg::ParseOptions(std::span<const std::string_view>{})).mode == wfg::Mode::Launch);
  const std::vector<std::string_view> help{"--help"};
  const std::vector<std::string_view> version{"--version"};
  const std::vector<std::string_view> diagnose{"--diagnose"};
  const std::vector<std::string_view> support{"--collect-support"};
  CHECK(wfg::Value(wfg::ParseOptions(help)).mode == wfg::Mode::Help);
  CHECK(wfg::Value(wfg::ParseOptions(version)).mode == wfg::Mode::Version);
  CHECK(wfg::Value(wfg::ParseOptions(diagnose)).mode == wfg::Mode::Diagnose);
  CHECK(wfg::Value(wfg::ParseOptions(support)).mode == wfg::Mode::CollectSupport);
  const std::vector<std::string_view> verbosity{"--log-level", "debug"};
  const auto parsed = wfg::ParseOptions(verbosity);
  CHECK(wfg::IsOk(parsed));
  if (wfg::IsOk(parsed)) CHECK(wfg::Value(parsed).log_level == wfg::LogLevel::Debug);
  const std::vector<std::string_view> conflicting{"--help", "--version"};
  REQUIRE_ERROR(wfg::ParseOptions(conflicting), wfg::ErrorCode::ConfigurationInvalid);
  const std::vector<std::string_view> unknown{"--dll", "evil.dll"};
  REQUIRE_ERROR(wfg::ParseOptions(unknown), wfg::ErrorCode::ConfigurationInvalid);
  const std::vector<std::string_view> snapshot{"--config-snapshot", "selected.toml"};
  REQUIRE_ERROR(wfg::ParseOptions(snapshot), wfg::ErrorCode::ConfigurationInvalid);
}
