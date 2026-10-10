#include "support.hpp"
#include "wfg/diagnostics.hpp"

#include <array>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
std::atomic<bool> track_allocations{};
std::atomic<std::size_t> tracked_allocation_count{};

wfg::Error StorageError() {
  return {wfg::ErrorCode::DiagnosticFailure, wfg::Stage::Diagnostic, {}, std::nullopt, std::nullopt};
}

class FakeStorage final : public wfg::ILogStorage {
public:
  wfg::Result<wfg::Unit> AppendEvent(std::string_view json_line, std::uint32_t max_file_mib, std::uint32_t retained_files) override {
    ++event_writes;
    observed_max_file_mib = max_file_mib;
    observed_retained_files = retained_files;
    if (fail_events) return StorageError();
    events.emplace_back(json_line);
    return wfg::Unit{};
  }
  wfg::Result<wfg::Unit> WriteSummaryAtomically(std::string_view json) override {
    ++summary_writes;
    if (fail_summary) return StorageError();
    summary.assign(json);
    return wfg::Unit{};
  }
  void WriteConsoleLine(std::string_view text) override { console.emplace_back(text); }
  bool fail_events{};
  bool fail_summary{};
  std::size_t event_writes{};
  std::size_t summary_writes{};
  std::uint32_t observed_max_file_mib{};
  std::uint32_t observed_retained_files{};
  std::vector<std::string> events;
  std::vector<std::string> console;
  std::string summary;
};

wfg::DiagnosticEvent Event(wfg::EventLevel level, wfg::EventCode code) {
  wfg::DiagnosticEvent event;
  event.level = level;
  event.code = code;
  event.stage = wfg::Stage::Transaction;
  event.operation = "VerifyImports";
  return event;
}

wfg::DiagnosticRecord Record() {
  wfg::DiagnosticRecord record;
  record.safety = wfg::SafetyDisposition::VerifiedNoMutation;
  return record;
}

class StorageFixture {
public:
  StorageFixture() {
    path = std::filesystem::temp_directory_path() / ("wfg-storage-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(path / "diagnostics");
  }
  ~StorageFixture() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }
  static void Write(const std::filesystem::path& file, std::string_view text) {
    std::ofstream output(file, std::ios::binary);
    output << text;
    CHECK(output.good());
  }
  static std::string Read(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary);
    CHECK(input.good());
    return {std::istreambuf_iterator<char>(input), {}};
  }
  std::filesystem::path path;
};
}

void* operator new(std::size_t size) {
  if (track_allocations.load(std::memory_order_relaxed)) tracked_allocation_count.fetch_add(1, std::memory_order_relaxed);
  if (void* memory = std::malloc(size)) return memory;
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) {
  if (track_allocations.load(std::memory_order_relaxed)) tracked_allocation_count.fetch_add(1, std::memory_order_relaxed);
  if (void* memory = std::malloc(size)) return memory;
  throw std::bad_alloc();
}
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

TEST("diagnostics.error_default_preserves_failure_explanation") {
  auto config = wfg::DefaultConfiguration();
  FakeStorage storage;
  wfg::RunLogger logger(config, storage);
  const auto failure = wfg::ExplainFailure({wfg::ErrorCode::StartupTooLate, wfg::Stage::Debug, "CheckFactory", 5U, std::nullopt});
  CHECK(failure.code == "WFG-E-STARTUP_TOO_LATE");
  CHECK(failure.operation == "CheckFactory");
  CHECK(failure.stage == wfg::Stage::Debug);
  CHECK(failure.raw_code == 5U);
  CHECK(failure.action.size() > 0);
  CHECK(logger.Emit(Event(wfg::EventLevel::Information, wfg::EventCode::StageCompleted)).index() == 0);
  CHECK(storage.event_writes == 0);
}

TEST("diagnostics.level_filters_apply_to_events") {
  for (const auto level : {wfg::LogLevel::Error, wfg::LogLevel::Info, wfg::LogLevel::Debug}) {
    auto config = wfg::DefaultConfiguration();
    config.log_level = level;
    FakeStorage storage;
    wfg::RunLogger logger(config, storage);
    CHECK(wfg::IsOk(logger.Emit(Event(wfg::EventLevel::Failure, wfg::EventCode::Failure))));
    CHECK(wfg::IsOk(logger.Emit(Event(wfg::EventLevel::Information, wfg::EventCode::StageCompleted))));
    CHECK(wfg::IsOk(logger.Emit(Event(wfg::EventLevel::DebugDetail, wfg::EventCode::DependencyObserved))));
    const auto expected = level == wfg::LogLevel::Error ? 1U : level == wfg::LogLevel::Info ? 2U : 3U;
    CHECK(storage.events.size() == expected);
  }
}

TEST("diagnostics.critical_warnings_are_visible_at_error") {
  auto config = wfg::DefaultConfiguration();
  FakeStorage storage;
  wfg::RunLogger logger(config, storage);
  auto warning = Event(wfg::EventLevel::CriticalWarning, wfg::EventCode::ObservationUnavailable);
  CHECK(wfg::IsOk(logger.Emit(warning)));
  CHECK(storage.events.size() == 1);
  CHECK(storage.events.front().find("OBSERVATION_UNAVAILABLE") != std::string::npos);
}

TEST("diagnostics.summary_escapes_values_and_redacts_paths") {
  auto record = Record();
  record.environment.gpu = wfg::Observed<std::string>(std::string("NVIDIA \"Ada\""), wfg::Availability::Observed, "dxgi_adapter", "query_ok");
  record.failure = wfg::ExplainFailure({wfg::ErrorCode::StartupTooLate, wfg::Stage::Debug, "/home/player/private/CheckFactory", std::nullopt, std::nullopt});
  const auto summary = wfg::SerializeSummary(record);
  CHECK(wfg::IsOk(summary));
  if (!wfg::IsOk(summary)) return;
  CHECK(wfg::Value(summary).find("NVIDIA \\\"Ada\\\"") != std::string::npos);
  CHECK(wfg::Value(summary).find("/home/player/private") == std::string::npos);
  CHECK(wfg::Value(summary).find("\"operation\":\"redacted\"") != std::string::npos);
  CHECK(wfg::Value(summary).find("\"dxgi_adapter\"") != std::string::npos);
}

TEST("diagnostics.failure_summary_keeps_utc_elapsed_and_only_known_action") {
  auto record = Record();
  record.failure = wfg::ExplainFailure({wfg::ErrorCode::StartupTooLate, wfg::Stage::debug_attach, "CheckFactory", 5U, std::nullopt});
  record.failure->time_utc = "2026-10-10T12:34:56Z";
  record.failure->action = "private free text that must never be persisted";
  record.elapsed_ms = 1234;
  const auto summary = wfg::SerializeSummary(record);
  CHECK(wfg::IsOk(summary));
  if (!wfg::IsOk(summary)) return;
  CHECK(wfg::Value(summary).find("\"time_utc\":\"2026-10-10T12:34:56Z\"") != std::string::npos);
  CHECK(wfg::Value(summary).find("\"elapsed_ms\":1234") != std::string::npos);
  CHECK(wfg::Value(summary).find("private free text") == std::string::npos);
}

TEST("diagnostics.spec_error_codes_are_stable") {
  struct FailureCase { wfg::ErrorCode code; std::string_view operation; std::string_view expected; };
  const std::array cases{
    FailureCase{wfg::ErrorCode::ConfigurationInvalid, "LoadConfig", "WFG-E-CONFIG_INVALID"},
    FailureCase{wfg::ErrorCode::UnsupportedBuild, "ValidateBuild", "WFG-E-BUILD_UNSUPPORTED"},
    FailureCase{wfg::ErrorCode::DllConflict, "InspectLocalDlls", "WFG-E-DLL_CONFLICT"},
    FailureCase{wfg::ErrorCode::UntrustedInterposer, "VerifyInterposer", "WFG-E-INTERPOSER_UNTRUSTED"},
    FailureCase{wfg::ErrorCode::AccessDenied, "Attach", "WFG-E-ATTACH_DENIED"},
    FailureCase{wfg::ErrorCode::StartupTooLate, "CheckFactory", "WFG-E-STARTUP_TOO_LATE"},
    FailureCase{wfg::ErrorCode::MutationFailure, "IAT_WRITE", "WFG-E-IAT_WRITE"},
    FailureCase{wfg::ErrorCode::MutationFailure, "IAT_VERIFY", "WFG-E-IAT_VERIFY"},
    FailureCase{wfg::ErrorCode::MutationFailure, "PROTECTION_RESTORE", "WFG-E-PROTECTION_RESTORE"},
    FailureCase{wfg::ErrorCode::MutationFailure, "ROLLBACK_ABORT", "WFG-E-ROLLBACK_ABORT"}};
  for (const auto& value : cases) {
    const auto failure = wfg::ExplainFailure({value.code, wfg::Stage::import_transaction, value.operation, 5U, std::nullopt});
    CHECK(failure.code == value.expected);
    CHECK(failure.raw_code == 5U);
  }
}

TEST("diagnostics.unavailable_observations_stay_unknown_and_plugin_is_not_fg") {
  auto record = Record();
  record.loaded_plugin = wfg::Observed<bool>(true, wfg::Availability::Observed, "module_list", "observed");
  const auto summary = wfg::SerializeSummary(record);
  CHECK(wfg::IsOk(summary));
  if (!wfg::IsOk(summary)) return;
  CHECK(wfg::Value(summary).find("\"hags_active\":{\"availability\":\"not_observed\"") != std::string::npos);
  CHECK(wfg::Value(summary).find("\"loaded_plugin\":{\"availability\":\"observed\",\"source\":\"module_list\",\"reason\":\"observed\",\"value\":true") != std::string::npos);
  CHECK(wfg::Value(summary).find("\"frame_generation\":\"unknown\"") != std::string::npos);
}

TEST("diagnostics.fg_rejection_requires_a_decoded_rejection_code") {
  auto record = Record();
  record.frame_generation = wfg::FrameGenerationState::Rejected;
  auto without_code = wfg::SerializeSummary(record);
  CHECK(wfg::IsOk(without_code));
  if (!wfg::IsOk(without_code)) return;
  CHECK(wfg::Value(without_code).find("FG_REJECTED") == std::string::npos);
  record.decoded_rejection_code = 0x1234U;
  auto with_code = wfg::SerializeSummary(record);
  CHECK(wfg::IsOk(with_code));
  if (wfg::IsOk(with_code)) CHECK(wfg::Value(with_code).find("WFG-W-FG_REJECTED") != std::string::npos);
}

TEST("diagnostics.embedded_identity_matches_generated_build") {
  const auto& embedded = wfg::EmbeddedBuildIdentity();
  CHECK(embedded.version == wfg::kEmbeddedBuildIdentity.version);
  CHECK(embedded.channel == wfg::kEmbeddedBuildIdentity.channel);
  CHECK(embedded.build_type == wfg::kEmbeddedBuildIdentity.build_type);
  CHECK(embedded.commit == wfg::kEmbeddedBuildIdentity.commit);
  CHECK(embedded.dirty == wfg::kEmbeddedBuildIdentity.dirty);
  CHECK(embedded.archive == wfg::kEmbeddedBuildIdentity.archive);
  CHECK(embedded.diagnostic_schema == wfg::kEmbeddedBuildIdentity.diagnostic_schema);
  CHECK(embedded.input_digest == wfg::kEmbeddedBuildIdentity.input_digest);
  auto record = Record();
  record.build = embedded;
  const auto summary = wfg::SerializeSummary(record);
  CHECK(wfg::IsOk(summary));
  if (wfg::IsOk(summary)) std::cout << "WFG_BUILD_IDENTITY:" << wfg::Value(summary) << '\n';
}

TEST("diagnostics.mutation_deferral_has_no_storage_or_allocation") {
  auto config = wfg::DefaultConfiguration();
  config.log_level = wfg::LogLevel::Debug;
  FakeStorage storage;
  wfg::RunLogger logger(config, storage);
  const auto event = Event(wfg::EventLevel::DebugDetail, wfg::EventCode::MutationStep);
  tracked_allocation_count.store(0, std::memory_order_relaxed);
  track_allocations.store(true, std::memory_order_relaxed);
  const auto result = logger.DeferMutationEvent(event);
  track_allocations.store(false, std::memory_order_relaxed);
  CHECK(wfg::IsOk(result));
  CHECK(tracked_allocation_count.load(std::memory_order_relaxed) == 0);
  CHECK(storage.event_writes == 0);
  CHECK(storage.summary_writes == 0);
}

TEST("diagnostics.deferred_events_flush_only_after_verified_safety") {
  auto config = wfg::DefaultConfiguration();
  config.log_level = wfg::LogLevel::Debug;
  FakeStorage storage;
  wfg::RunLogger logger(config, storage);
  CHECK(wfg::IsOk(logger.DeferMutationEvent(Event(wfg::EventLevel::DebugDetail, wfg::EventCode::MutationStep))));
  auto record = Record();
  record.safety = wfg::SafetyDisposition::Unverified;
  CHECK(!wfg::IsOk(logger.Finish(record)));
  CHECK(storage.event_writes == 0);
  CHECK(storage.summary_writes == 0);
  record.safety = wfg::SafetyDisposition::VerifiedRollback;
  CHECK(wfg::IsOk(logger.Finish(record)));
  CHECK(storage.event_writes == 1);
  CHECK(storage.summary_writes == 1);
}

TEST("diagnostics.storage_failure_falls_back_without_replacing_run_failure") {
  auto config = wfg::DefaultConfiguration();
  FakeStorage storage;
  storage.fail_events = true;
  storage.fail_summary = true;
  wfg::RunLogger logger(config, storage);
  const auto emit = logger.Emit(Event(wfg::EventLevel::Failure, wfg::EventCode::Failure));
  CHECK(!wfg::IsOk(emit));
  auto record = Record();
  record.safety = wfg::SafetyDisposition::VerifiedRollback;
  record.outcome = wfg::RunOutcome::RolledBack;
  record.failure = wfg::ExplainFailure({wfg::ErrorCode::StartupTooLate, wfg::Stage::Debug, "CheckFactory", std::nullopt, std::nullopt});
  const auto finish = logger.Finish(record);
  CHECK(!wfg::IsOk(finish));
  CHECK(record.outcome == wfg::RunOutcome::RolledBack);
  CHECK(record.failure->code == "WFG-E-STARTUP_TOO_LATE");
  CHECK(storage.console.size() >= 2);
}

TEST("diagnostics.file_storage_rotates_to_configured_limits") {
  const auto directory = std::filesystem::temp_directory_path() / "wfg-diagnostics-rotation-tests";
  std::filesystem::remove_all(directory);
  wfg::FileLogStorage storage(directory);
  const std::string line(700 * 1024, 'x');
  CHECK(wfg::IsOk(storage.AppendEvent(line, 1, 2)));
  CHECK(wfg::IsOk(storage.AppendEvent(line, 1, 2)));
  CHECK(wfg::IsOk(storage.AppendEvent(line, 1, 2)));
  std::size_t event_files = 0;
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    if (entry.path().filename().string().starts_with("events")) {
      ++event_files;
      CHECK(std::filesystem::file_size(entry.path()) <= 1024 * 1024);
    }
  }
  CHECK(event_files <= 2);
  const std::string summary = "{\"status\":\"ok\"}";
  CHECK(wfg::IsOk(storage.WriteSummaryAtomically(summary)));
  std::ifstream summary_file(directory / "last-run.json", std::ios::binary);
  CHECK(std::string(std::istreambuf_iterator<char>(summary_file), {}) == summary);
  summary_file.close();
  std::filesystem::remove_all(directory);
}

TEST("diagnostics.summary_replacement_publishes_complete_new_content") {
  StorageFixture fixture;
  wfg::FileLogStorage storage(fixture.path / "diagnostics");
  CHECK(wfg::IsOk(storage.WriteSummaryAtomically("{\"run\":1}")));
  CHECK(wfg::IsOk(storage.WriteSummaryAtomically("{\"run\":2,\"complete\":true}")));
  CHECK(StorageFixture::Read(fixture.path / "diagnostics" / "last-run.json") == "{\"run\":2,\"complete\":true}");
  CHECK(!std::filesystem::exists(fixture.path / "diagnostics" / "last-run.json.tmp"));
}

TEST("diagnostics.rotation_prunes_old_files_when_retention_is_reduced") {
  StorageFixture fixture;
  for (unsigned index = 1; index < 10; ++index) {
    StorageFixture::Write(fixture.path / "diagnostics" / ("events." + std::to_string(index) + ".jsonl"), "old");
  }
  wfg::FileLogStorage storage(fixture.path / "diagnostics");
  CHECK(wfg::IsOk(storage.AppendEvent("new", 1, 1)));
  CHECK(std::distance(std::filesystem::directory_iterator(fixture.path / "diagnostics"), std::filesystem::directory_iterator{}) == 1);
  CHECK(StorageFixture::Read(fixture.path / "diagnostics" / "events.jsonl") == "new\n");
}

TEST("diagnostics.rotation_prunes_files_exceeding_a_reduced_size_limit") {
  StorageFixture fixture;
  const std::string previous(2 * 1024 * 1024, 'x');
  StorageFixture::Write(fixture.path / "diagnostics" / "events.jsonl", previous);
  StorageFixture::Write(fixture.path / "diagnostics" / "events.1.jsonl", previous);
  wfg::FileLogStorage storage(fixture.path / "diagnostics");
  CHECK(wfg::IsOk(storage.AppendEvent("new", 1, 2)));
  for (const auto& entry : std::filesystem::directory_iterator(fixture.path / "diagnostics")) {
    CHECK(std::filesystem::file_size(entry.path()) <= 1024 * 1024);
  }
}

TEST("diagnostics.storage_refuses_linked_event_and_temporary_files") {
  StorageFixture fixture;
  const auto outside = fixture.path / "outside.txt";
  StorageFixture::Write(outside, "private sentinel");
  wfg::FileLogStorage storage(fixture.path / "diagnostics");
  std::filesystem::create_hard_link(outside, fixture.path / "diagnostics" / "events.jsonl");
  CHECK(!wfg::IsOk(storage.AppendEvent("{\"event\":true}", 1, 2)));
  CHECK(StorageFixture::Read(outside) == "private sentinel");
  std::filesystem::create_hard_link(outside, fixture.path / "diagnostics" / "last-run.json.tmp");
  CHECK(!wfg::IsOk(storage.WriteSummaryAtomically("{\"run\":2}")));
  CHECK(StorageFixture::Read(outside) == "private sentinel");
}

TEST("diagnostics.storage_refuses_symlink_directory_escape") {
  StorageFixture fixture;
  std::filesystem::create_directory(fixture.path / "outside");
  std::error_code error;
  std::filesystem::create_directory_symlink(fixture.path / "outside", fixture.path / "linked", error);
  if (error) {
    std::cout << "SKIP symlink creation: " << error.message() << '\n';
    return;
  }
  wfg::FileLogStorage storage(fixture.path / "linked" / "diagnostics");
  CHECK(!wfg::IsOk(storage.AppendEvent("{\"event\":true}", 1, 2)));
  CHECK(!wfg::IsOk(storage.WriteSummaryAtomically("{\"run\":2}")));
  CHECK(std::filesystem::is_empty(fixture.path / "outside"));
}

TEST("diagnostics.storage_refuses_symlink_file_escape") {
  StorageFixture fixture;
  const auto outside = fixture.path / "outside.txt";
  StorageFixture::Write(outside, "private sentinel");
  std::error_code error;
  std::filesystem::create_symlink(outside, fixture.path / "diagnostics" / "events.jsonl", error);
  if (error) {
    std::cout << "SKIP file symlink creation: " << error.message() << '\n';
    return;
  }
  std::filesystem::create_symlink(outside, fixture.path / "diagnostics" / "last-run.json.tmp");
  wfg::FileLogStorage storage(fixture.path / "diagnostics");
  CHECK(!wfg::IsOk(storage.AppendEvent("event", 1, 2)));
  CHECK(!wfg::IsOk(storage.WriteSummaryAtomically("{\"run\":2}")));
  CHECK(StorageFixture::Read(outside) == "private sentinel");
}

TEST("diagnostics.full_mutation_buffer_refuses_without_allocation_or_io") {
  auto config = wfg::DefaultConfiguration();
  FakeStorage storage;
  wfg::RunLogger logger(config, storage);
  const auto event = Event(wfg::EventLevel::DebugDetail, wfg::EventCode::MutationStep);
  for (std::size_t index = 0; index < 32; ++index) CHECK(wfg::IsOk(logger.DeferMutationEvent(event)));
  tracked_allocation_count.store(0, std::memory_order_relaxed);
  track_allocations.store(true, std::memory_order_relaxed);
  const auto overflow = logger.DeferMutationEvent(event);
  const auto emit = logger.Emit(event);
  track_allocations.store(false, std::memory_order_relaxed);
  CHECK(!wfg::IsOk(overflow));
  CHECK(!wfg::IsOk(emit));
  CHECK(tracked_allocation_count.load(std::memory_order_relaxed) == 0);
  CHECK(storage.event_writes == 0);
  CHECK(storage.summary_writes == 0);
}

#ifdef _WIN32
TEST("diagnostics.windows_failed_replacement_preserves_old_summary") {
  StorageFixture fixture;
  wfg::FileLogStorage storage(fixture.path / "diagnostics");
  CHECK(wfg::IsOk(storage.WriteSummaryAtomically("{\"run\":1}")));
  const auto destination = fixture.path / "diagnostics" / "last-run.json";
  const auto locked = CreateFileW(destination.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  CHECK(locked != INVALID_HANDLE_VALUE);
  const auto replacement = storage.WriteSummaryAtomically("{\"run\":2}");
  const auto close_result = CloseHandle(locked);
  CHECK(close_result != 0);
  CHECK(!wfg::IsOk(replacement));
  CHECK(StorageFixture::Read(destination) == "{\"run\":1}");
  CHECK(!std::filesystem::exists(fixture.path / "diagnostics" / "last-run.json.tmp"));
  CHECK(wfg::IsOk(storage.WriteSummaryAtomically("{\"run\":3}")));
  CHECK(StorageFixture::Read(destination) == "{\"run\":3}");
}

TEST("diagnostics.windows_localappdata_resolution_is_absolute_and_available") {
  wchar_t* previous_value = nullptr;
  std::size_t previous_size = 0;
  CHECK(_wdupenv_s(&previous_value, &previous_size, L"LOCALAPPDATA") == 0);
  const std::wstring previous = previous_value ? previous_value : L"";
  std::free(previous_value);
  StorageFixture fixture;
  CHECK(_wputenv_s(L"LOCALAPPDATA", fixture.path.c_str()) == 0);
  const auto resolved = wfg::LocalAppDataDiagnosticsDirectory();
  CHECK(_wputenv_s(L"LOCALAPPDATA", L"") == 0);
  const auto missing = wfg::LocalAppDataDiagnosticsDirectory();
  CHECK(_wputenv_s(L"LOCALAPPDATA", L"relative-path") == 0);
  const auto relative = wfg::LocalAppDataDiagnosticsDirectory();
  CHECK(_wputenv_s(L"LOCALAPPDATA", previous.c_str()) == 0);
  CHECK(wfg::IsOk(resolved));
  if (wfg::IsOk(resolved)) CHECK(wfg::Value(resolved) == fixture.path / L"WitcherFG" / L"diagnostics");
  CHECK(!wfg::IsOk(missing));
  CHECK(!wfg::IsOk(relative));
}
#endif
