#include "support.hpp"
#include "fixtures/worker_allocation_probe.hpp"
#include "wfg/diagnostics.hpp"

#include <algorithm>
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
  worker_allocation_probe::Record();
  if (track_allocations.load(std::memory_order_relaxed)) tracked_allocation_count.fetch_add(1, std::memory_order_relaxed);
  if (void* memory = std::malloc(size)) return memory;
  throw std::bad_alloc();
}

TEST("diagnostics.concurrent_producer_is_refused_for_writer_lifetime") {
  StorageFixture fixture;
  wfg::FileLogStorage first(fixture.path / "diagnostics");
  wfg::FileLogStorage second(fixture.path / "diagnostics");
  REQUIRE_OK(first.AppendEvent("{}", 2, 5));
  REQUIRE_ERROR(second.AppendEvent("{}", 2, 5), wfg::ErrorCode::DiagnosticFailure);
}
void* operator new[](std::size_t size) {
  worker_allocation_probe::Record();
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
    const auto stage = value.code == wfg::ErrorCode::AccessDenied ? wfg::Stage::debug_attach : wfg::Stage::import_transaction;
    const auto failure = wfg::ExplainFailure({value.code, stage, value.operation, 5U, std::nullopt});
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
  if (wfg::IsOk(with_code)) CHECK(wfg::Value(with_code).find("WFG-E-FG_REJECTED") != std::string::npos);
}

TEST("diagnostics.fix_i1_specific_failure_event_and_deferred_payload") {
  for (const bool deferred : {false, true}) {
    auto config = wfg::DefaultConfiguration();
    FakeStorage storage;
    storage.fail_events = true;
    wfg::RunLogger logger(config, storage);
    auto event = Event(wfg::EventLevel::Failure, wfg::EventCode::Failure);
    event.failure = wfg::Error{wfg::ErrorCode::StartupTooLate, wfg::Stage::pre_factory_guard, "CheckFactory", 5U, std::nullopt};
    const std::string_view utc = "2026-10-10T12:34:56Z";
    std::copy(utc.begin(), utc.end(), event.time_utc.begin());
    event.elapsed_ms = 1234;
    event.vendor_code = 42U;
    if (deferred) {
      tracked_allocation_count = 0;
      track_allocations = true;
      const auto result = logger.DeferMutationEvent(event);
      track_allocations = false;
      CHECK(wfg::IsOk(result));
      CHECK(tracked_allocation_count == 0);
      CHECK(storage.event_writes == 0);
      event.failure.reset();
      event.time_utc.fill('x');
      CHECK(!wfg::IsOk(logger.Finish(Record())));
    } else {
      CHECK(!wfg::IsOk(logger.Emit(event)));
    }
    CHECK(!storage.console.empty());
    const auto& json = storage.console.front();
    CHECK(json.find("WFG-E-STARTUP_TOO_LATE") != std::string::npos);
    CHECK(json.find("\"stage\":\"pre_factory_guard\"") != std::string::npos);
    CHECK(json.find("\"operation\":\"CheckFactory\"") != std::string::npos);
    CHECK(json.find("\"time_utc\":\"2026-10-10T12:34:56Z\"") != std::string::npos);
    CHECK(json.find("\"elapsed_ms\":1234") != std::string::npos);
    CHECK(json.find("\"raw_code\":5") != std::string::npos);
    CHECK(json.find("\"vendor_code\":42") != std::string::npos);
    CHECK(json.find("launch this tool before the game") != std::string::npos);
  }
}

TEST("diagnostics.fix_i2_initiating_iat_failure_never_asserts_rollback") {
  for (const auto operation : {"IAT_WRITE", "IAT_VERIFY"}) {
    for (const auto safety : {wfg::SafetyDisposition::VerifiedRollback, wfg::SafetyDisposition::VerifiedAbort}) {
      auto record = Record();
      record.safety = safety;
      record.outcome = safety == wfg::SafetyDisposition::VerifiedAbort ? wfg::RunOutcome::StartupAborted : wfg::RunOutcome::RolledBack;
      record.failure = wfg::ExplainFailure({wfg::ErrorCode::MutationFailure, wfg::Stage::import_transaction, operation, 998U, std::nullopt});
      CHECK(record.failure->action.find("was rolled back") == std::string::npos);
      const auto json = REQUIRE_OK(wfg::SerializeSummary(record));
      CHECK(json.find(std::string("WFG-E-") + operation) != std::string::npos);
      CHECK(json.find("\"raw_code\":998") != std::string::npos);
      CHECK(json.find("was rolled back") == std::string::npos);
      auto config = wfg::DefaultConfiguration();
      FakeStorage storage;
      wfg::RunLogger logger(config, storage);
      CHECK(wfg::IsOk(logger.Finish(record)));
      CHECK(storage.summary == json);
      CHECK(record.failure->operation == operation);
      CHECK(record.failure->raw_code == 998U);
    }
  }
}

TEST("diagnostics.fix_i3_rejection_events_require_sourced_decoded_code") {
  auto config = wfg::DefaultConfiguration();
  FakeStorage storage;
  wfg::RunLogger logger(config, storage);
  auto event = Event(wfg::EventLevel::CriticalWarning, wfg::EventCode::FgRejected);
  CHECK(wfg::IsOk(logger.Emit(event)));
  CHECK(storage.events.back().find("WFG-W-OBSERVATION_UNAVAILABLE") != std::string::npos);
  CHECK(storage.events.back().find("FG_ACTIVITY_NOT_OBSERVED") == std::string::npos);
  const std::array fields{wfg::DiagnosticField{wfg::DiagnosticFieldKey::DecodedRejectionCode, wfg::DiagnosticFieldSource::RuntimeObservation, std::uint64_t{0x1234}}};
  event.fields = fields;
  CHECK(wfg::IsOk(logger.Emit(event)));
  CHECK(storage.events.back().find("WFG-E-FG_REJECTED") != std::string::npos);
  for (const auto& field : {
         wfg::DiagnosticField{wfg::DiagnosticFieldKey::DecodedRejectionCode, wfg::DiagnosticFieldSource::Launcher, std::uint64_t{0x1234}},
         wfg::DiagnosticField{wfg::DiagnosticFieldKey::DecodedRejectionCode, wfg::DiagnosticFieldSource::RuntimeObservation, true},
         wfg::DiagnosticField{wfg::DiagnosticFieldKey::DecodedRejectionCode, wfg::DiagnosticFieldSource::RuntimeObservation, std::uint64_t{0x100000000ULL}}}) {
    event.fields = std::span<const wfg::DiagnosticField>(&field, 1);
    CHECK(wfg::IsOk(logger.Emit(event)));
    CHECK(storage.events.back().find("WFG-W-OBSERVATION_UNAVAILABLE") != std::string::npos);
  }
  auto record = Record();
  record.frame_generation = wfg::FrameGenerationState::Rejected;
  const auto json = REQUIRE_OK(wfg::SerializeSummary(record));
  CHECK(json.find("\"frame_generation\":\"unknown\"") != std::string::npos);
  CHECK(json.find("WFG-W-OBSERVATION_UNAVAILABLE") != std::string::npos);
}

TEST("diagnostics.fix_i4_utf8_boundary_and_malformed_direct_deferred_summary") {
  const std::array values{std::string(95, 'A') + "\xc3\xa9", std::string("\xc3"), std::string("\xc0\xaf"), std::string("\xed\xa0\x80"), std::string("\xf4\x90\x80\x80"), std::string("\x80"), std::string("\xe2\x28\xa1"), std::string(94, 'A') + "\xc3\xa9", std::string("\xe2\x82\xac"), std::string("\xf0\x9f\x98\x80")};
  std::size_t value_index = 0;
  for (const auto& value : values) {
    for (const bool deferred : {false, true}) {
      auto config = wfg::DefaultConfiguration();
      config.log_level = wfg::LogLevel::Debug;
      FakeStorage storage;
      wfg::RunLogger logger(config, storage);
      const std::array fields{wfg::DiagnosticField{wfg::DiagnosticFieldKey::AdapterName, wfg::DiagnosticFieldSource::DxgiObservation, std::string_view(value)}};
      auto event = Event(wfg::EventLevel::DebugDetail, wfg::EventCode::DependencyObserved);
      event.fields = fields;
      tracked_allocation_count = 0;
      if (deferred) track_allocations = true;
      const auto result = deferred ? logger.DeferMutationEvent(event) : logger.Emit(event);
      track_allocations = false;
      CHECK(wfg::IsOk(result));
      if (deferred) {
        CHECK(tracked_allocation_count == 0);
        CHECK(storage.event_writes == 0);
        CHECK(wfg::IsOk(logger.Finish(Record())));
      }
      const auto& json = storage.events.front();
      const auto expected = value_index < 7 ? std::string("redacted") : value;
      CHECK(json.find("\"value\":\"" + expected + "\"") != std::string::npos);
      std::cout << "WFG_UTF8_JSON:" << json << '\n';
    }
    auto record = Record();
    record.environment.gpu = wfg::Observed<std::string>(value, wfg::Availability::Observed, "dxgi", "query_ok");
    const auto json = REQUIRE_OK(wfg::SerializeSummary(record));
    const auto expected = value_index < 7 ? std::string("redacted") : value;
    CHECK(json.find("\"value\":\"" + expected + "\"") != std::string::npos);
    std::cout << "WFG_UTF8_JSON:" << json << '\n';
    ++value_index;
  }
}

TEST("diagnostics.filtered_deferred_events_do_not_consume_critical_capacity") {
  auto config = wfg::DefaultConfiguration();
  FakeStorage storage;
  wfg::RunLogger logger(config, storage);
  const auto invisible = Event(wfg::EventLevel::DebugDetail, wfg::EventCode::MutationStep);
  tracked_allocation_count = 0;
  track_allocations = true;
  bool successful = true;
  for (std::size_t index = 0; index < 100; ++index) successful = wfg::IsOk(logger.DeferMutationEvent(invisible)) && successful;
  const auto critical = logger.DeferMutationEvent(Event(wfg::EventLevel::CriticalWarning, wfg::EventCode::ObservationUnavailable));
  track_allocations = false;
  CHECK(successful);
  CHECK(wfg::IsOk(critical));
  CHECK(tracked_allocation_count == 0);
  CHECK(storage.event_writes == 0);
  CHECK(wfg::IsOk(logger.Finish(Record())));
  CHECK(storage.events.size() == 1);
}

TEST("diagnostics.overlong_operation_has_identical_direct_deferred_redaction") {
  auto config = wfg::DefaultConfiguration();
  FakeStorage storage;
  wfg::RunLogger logger(config, storage);
  const std::string operation(65, 'A');
  auto event = Event(wfg::EventLevel::Failure, wfg::EventCode::Failure);
  event.operation = operation;
  CHECK(wfg::IsOk(logger.Emit(event)));
  CHECK(wfg::IsOk(logger.DeferMutationEvent(event)));
  CHECK(wfg::IsOk(logger.Finish(Record())));
  CHECK(storage.events.size() == 2);
  CHECK(storage.events[0] == storage.events[1]);
  CHECK(storage.events[0].find("\"operation\":\"redacted\"") != std::string::npos);
}

TEST("diagnostics.fix_i5_stage_operation_cross_product") {
  for (const auto stage : {wfg::Stage::debug_attach, wfg::Stage::dependencies, wfg::Stage::import_transaction, wfg::Stage::activation}) {
    const auto denial = wfg::ExplainFailure({wfg::ErrorCode::AccessDenied, stage, "ReadInterposer", 5U, std::nullopt});
    CHECK((denial.code == "WFG-E-ATTACH_DENIED") == (stage == wfg::Stage::debug_attach));
    if (stage != wfg::Stage::debug_attach) CHECK(denial.action.find("current user session") == std::string::npos);
    for (const auto operation : {"IatVerify", "VerifyIat", "IAT_VERIFY", "IAT_WRITE", "RestoreProtection", "ROLLBACK_ABORT"}) {
      const auto failure = wfg::ExplainFailure({wfg::ErrorCode::MutationFailure, stage, operation, 5U, std::nullopt});
      CHECK((failure.code != "WFG-E-LAUNCH_FAILED") == (stage == wfg::Stage::import_transaction));
    }
  }
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
  {
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
  }
  std::filesystem::remove_all(directory);
}

TEST("diagnostics.summary_replacement_publishes_complete_new_content") {
  StorageFixture fixture;
  wfg::FileLogStorage storage(fixture.path / "diagnostics");
  CHECK(wfg::IsOk(storage.WriteSummaryAtomically("{\"run\":1}")));
  const auto replacement = storage.WriteSummaryAtomically("{\"run\":2,\"complete\":true}");
  if (!wfg::IsOk(replacement)) std::cout << "SUMMARY_NATIVE=" << wfg::Failure(replacement).native_code.value_or(0) << '\n';
  CHECK(wfg::IsOk(replacement));
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
  config.log_level = wfg::LogLevel::Debug;
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
  REQUIRE_OK(missing);
  REQUIRE_OK(relative);
  CHECK(wfg::Value(resolved).is_absolute());
  CHECK(wfg::Value(resolved) == wfg::Value(missing));
  CHECK(wfg::Value(resolved) == wfg::Value(relative));
  CHECK(wfg::Value(resolved) != fixture.path / L"WitcherFG" / L"diagnostics");
}
#endif

TEST("diagnostics.optional_runtime_fields_are_bounded_and_source_preserving") {
  auto record=Record();record.runtime_observation.emplace();auto& runtime=*record.runtime_observation;
  runtime.latest.requested_mode={std::nullopt,wfg::Availability::UnsupportedLayout,"read_only_runtime","No independently validated SDK diagnostic layout."};
  runtime.latest.frame_id={UINT64_MAX,wfg::Availability::Observed,"read_only_runtime",""};
  runtime.latest.sdk_code={std::nullopt,wfg::Availability::ReadFailed,"C:\\Users\\PRIVATE_SECRET","PRIVATE_SECRET"};
  runtime.samples=2;runtime.duration_ms=30000;
  const auto text=REQUIRE_OK(wfg::SerializeSummary(record));
  CHECK(text.find("\"runtime_observation\":{")!=std::string::npos);
  CHECK(text.find("diagnostic_layout_unavailable")!=std::string::npos);
  CHECK(text.find("18446744073709551615")!=std::string::npos);
  CHECK(text.find("PRIVATE_SECRET")==std::string::npos);
  runtime.duration_ms=122001;CHECK(!wfg::IsOk(wfg::SerializeSummary(record)));
}

TEST("diagnostics.completion_time_cannot_serialize_private_or_malformed_text") {
  auto record=Record();std::memcpy(record.completed_utc.data(),"C:\\Users\\PRIVATE____",20);
  auto json=REQUIRE_OK(wfg::SerializeSummary(record));CHECK(json.find("PRIVATE")==std::string::npos);
  CHECK(json.find("\"completed_utc\":\"unknown\"")!=std::string::npos);
  std::memcpy(record.completed_utc.data(),"2026-99-10T12:34:56Z",20);
  json=REQUIRE_OK(wfg::SerializeSummary(record));CHECK(json.find("\"completed_utc\":\"unknown\"")!=std::string::npos);
}
