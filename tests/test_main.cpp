#include "support.hpp"
#include <iostream>
#include <string_view>

#ifdef WFG_CONFIG_SNAPSHOT
#include "wfg/configuration.hpp"
#include <chrono>
#include <filesystem>

namespace {
class SnapshotDirectory {
public:
  SnapshotDirectory() {
    const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    for (unsigned attempt = 0; attempt < 10; ++attempt) {
      path = std::filesystem::temp_directory_path() / ("wfg-snapshot-" + suffix + "-" + std::to_string(attempt));
      if (std::filesystem::create_directory(path)) return;
    }
    throw std::runtime_error("snapshot_directory_unavailable");
  }
  ~SnapshotDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }
  std::filesystem::path path;
};

std::string_view OriginName(wfg::ConfigOrigin origin) {
  switch (origin) {
    case wfg::ConfigOrigin::Default: return "default";
    case wfg::ConfigOrigin::File: return "file";
    case wfg::ConfigOrigin::CommandLine: return "command_line";
  }
  return "unknown";
}

int ConfigSnapshot(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "WFG-E-CONFIG_INVALID: snapshot_path_required\n";
    return 2;
  }
  try {
    std::vector<std::string_view> arguments;
    for (int index = 3; index < argc; ++index) arguments.emplace_back(argv[index]);
    const auto options = wfg::ParseOptions(arguments);
    if (!wfg::IsOk(options) || wfg::Value(options).mode != wfg::Mode::Launch) {
      std::cerr << "WFG-E-CONFIG_INVALID: snapshot_options\n";
      return 2;
    }
    SnapshotDirectory temporary;
    const auto selected = std::filesystem::absolute(argv[2]);
    if (std::filesystem::exists(selected)) std::filesystem::copy_file(selected, temporary.path / "FrameGenFix.toml");
    const auto loaded = wfg::LoadConfiguration(temporary.path, wfg::Value(options));
    if (!wfg::IsOk(loaded)) {
      std::cerr << "WFG-E-CONFIG_INVALID: " << wfg::Failure(loaded).operation.view() << '\n';
      return 2;
    }
    const auto& config = wfg::Value(loaded);
    const auto level = config.log_level == wfg::LogLevel::Error ? "error" : config.log_level == wfg::LogLevel::Info ? "info" : "debug";
    std::cout << std::boolalpha << "{\"schema_version\":" << config.schema_version
              << ",\"logging\":{\"level\":\"" << level << "\",\"file_logging\":" << config.file_logging
              << ",\"max_file_mib\":" << config.max_file_mib << ",\"retained_files\":" << config.retained_files
              << "},\"diagnostics\":{\"runtime_observation\":" << config.runtime_observation
              << ",\"sample_interval_ms\":" << config.sample_interval_ms << ",\"observation_seconds\":" << config.observation_seconds
              << "},\"origins\":{";
    bool first = true;
    for (const auto& [key, origin] : config.origins) {
      if (!first) std::cout << ',';
      first = false;
      std::cout << '\"' << key << "\":\"" << OriginName(origin) << '\"';
    }
    std::cout << "}}\n";
    return 0;
  } catch (const std::exception&) {
    std::cerr << "WFG-E-CONFIG_INVALID: snapshot_io\n";
    return 2;
  }
}
}
#endif

int main(int argc, char** argv) {
#ifdef WFG_CONFIG_SNAPSHOT
  if (argc >= 2 && std::string_view(argv[1]) == "--config-snapshot") return ConfigSnapshot(argc, argv);
#endif
  const std::string_view filter = argc == 3 && std::string_view(argv[1]) == "--filter" ? argv[2] : "";
  int failures = 0;
  int executed = 0;
  for (const auto& test : test::Cases()) if (filter.empty() || std::string_view(test.name).starts_with(filter)) {
    ++executed;
    try { test.run(); } catch (const std::exception& error) { ++failures; std::cerr << test.name << ": " << error.what() << '\n'; }
  }
  if (!filter.empty() && executed == 0) {
    std::cerr << "No tests matched the selected filter\n";
    return 2;
  }
  std::cout << executed - failures << "/" << executed << " tests passed\n";
  return failures == 0 ? 0 : 1;
}
