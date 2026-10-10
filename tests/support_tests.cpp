#include "support.hpp"
#include "wfg/diagnostics.hpp"
#include "wfg/support_bundle.hpp"
#include "wfg/support_storage_windows.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
#include <sddl.h>
#endif

namespace {
wfg::Error SupportError() { return {wfg::ErrorCode::SupportBundleFailure, wfg::Stage::Support, "SupportFixture", {}, {}}; }
class ProducerCapture final : public wfg::ILogStorage {
public:
  wfg::Result<wfg::Unit> AppendEvent(std::string_view line, std::uint32_t, std::uint32_t) override { events += std::string(line) + '\n'; return wfg::Unit{}; }
  wfg::Result<wfg::Unit> WriteSummaryAtomically(std::string_view json) override { summary = json; return wfg::Unit{}; }
  void WriteConsoleLine(std::string_view) override {}
  std::string summary, events;
};
wfg::DiagnosticRecord Record() {
  wfg::DiagnosticRecord record;
  record.safety = wfg::SafetyDisposition::VerifiedNoMutation;
  record.outcome = wfg::RunOutcome::Refused;
  record.game_package = {std::string("CDPROJEKTSA.Witcher3GOTY"), wfg::Availability::Observed, "package_manifest", "verified"};
  record.game_version = {std::string("5.0.4.0"), wfg::Availability::Observed, "package_manifest", "verified"};
  record.failure = wfg::ExplainFailure({wfg::ErrorCode::UnsupportedBuild, wfg::Stage::build_validation, "ValidateBuild", {}, {}});
  record.elapsed_ms = 42;
  return record;
}
wfg::SupportInputs Inputs() {
  return {1, {1, std::string(40, 'a'), "5.0.4.0", false, "PRIVATE_ENV_SECRET"}, {}, {}};
}
std::vector<std::byte> Encode(std::string_view text) { return {reinterpret_cast<const std::byte*>(text.data()), reinterpret_cast<const std::byte*>(text.data() + text.size())}; }
class SupportFixture final : public wfg::ISupportStorage {
public:
  SupportFixture() {
    auto record = Record();
    wfg::RunLogger logger(record.effective_config, producer);
    wfg::DiagnosticEvent event;
    event.level = wfg::EventLevel::Failure;
    event.code = wfg::EventCode::Failure;
    event.failure = wfg::Error{wfg::ErrorCode::UnsupportedBuild, wfg::Stage::build_validation, "ValidateBuild", 5U, {}};
    event.elapsed_ms = 42;
    std::memcpy(event.time_utc.data(),"2026-10-10T12:34:56Z",20);
    const std::array fields{wfg::DiagnosticField{wfg::DiagnosticFieldKey::ImportCount,wfg::DiagnosticFieldSource::PeInspection,std::uint64_t{4}}};
    event.fields = fields;
    CHECK(wfg::IsOk(logger.Emit(event)));
    CHECK(wfg::IsOk(logger.Finish(record)));
    CHECK(producer.summary == REQUIRE_OK(wfg::SerializeSummary(record)));
  }
  wfg::Result<wfg::SupportReadHandle> OpenSanitizedSummary() override { return missing ? wfg::SupportReadHandle{} : wfg::SupportReadHandle{7, true, reparse}; }
  wfg::Result<wfg::SupportRead> ReadSanitizedSummary(const wfg::SupportReadHandle&, std::size_t) override { ++reads; return wfg::SupportRead{Encode(producer.summary), replacement ? 8U : 7U, true, false}; }
  wfg::Result<wfg::SupportReadHandle> OpenRecentEvents() override { return producer.events.empty() ? wfg::SupportReadHandle{} : wfg::SupportReadHandle{9, true, false}; }
  wfg::Result<wfg::SupportRead> ReadRecentEvents(const wfg::SupportReadHandle&, std::size_t) override { return wfg::SupportRead{Encode(producer.events), 9, true, false}; }
  wfg::Result<std::filesystem::path> CommitArchiveAtomically(std::span<const std::byte> bytes) override {
    if (fail_write) return SupportError();
    archive.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    ++commits;
    const auto* selected = std::getenv("WFG_SUPPORT_FIXTURE");
    output = selected ? std::filesystem::path(selected) : std::filesystem::temp_directory_path() / "wfg-support-fixture.zip";
    std::ofstream file(output, std::ios::binary); file.write(archive.data(), static_cast<std::streamsize>(archive.size()));
    if (!file) return SupportError();
    return output;
  }
  std::filesystem::path CompletedArchiveLocation() const override { return output; }
  ProducerCapture producer;
  std::string archive;
  std::filesystem::path output;
  unsigned reads{}, commits{};
  bool reparse{}, replacement{}, fail_write{}, missing{};
};
void Replace(std::string& text, std::string_view old_value, std::string_view new_value) {
  const auto position = text.find(old_value); CHECK(position != std::string::npos); text.replace(position, old_value.size(), new_value);
}
}

TEST("support.real_producer_roundtrip_preserves_failure_and_observation") {
  SupportFixture fixture;
  const auto bundle = REQUIRE_OK(wfg::CollectSupport(Inputs(), fixture));
  CHECK(fixture.archive.find("CDPROJEKTSA.Witcher3GOTY") != std::string::npos);
  CHECK(fixture.archive.find("WFG-E-BUILD_UNSUPPORTED") != std::string::npos);
  CHECK(fixture.archive.find("package_manifest") != std::string::npos);
  CHECK(fixture.archive.find("not_observed") != std::string::npos);
  CHECK(fixture.archive.find("PRIVATE_ENV_SECRET") == std::string::npos);
  CHECK(bundle.entries.size() >= 4);
}
TEST("support.persisted_unknown_nested_fields_and_free_text_are_discarded") {
  SupportFixture fixture;
  fixture.producer.summary.insert(1, R"("private":{"token":"PRIVATE_SECRET","path":"C:\\Users\\private","numbers":[-1,1.25,2e3]},)");
  Replace(fixture.producer.summary, "\"build\":{", R"("build":{"private":"PRIVATE_SECRET",)");
  Replace(fixture.producer.summary, "Use a supported game build; no imports were changed.", "PRIVATE_SECRET");
  fixture.producer.events.insert(1, R"("private":{"token":"PRIVATE_SECRET"},)");
  REQUIRE_OK(wfg::CollectSupport(Inputs(), fixture));
  CHECK(fixture.archive.find("PRIVATE_SECRET") == std::string::npos);
  CHECK(fixture.archive.find("C:\\\\Users") == std::string::npos);
}
TEST("support.invalid_utf8_and_duplicate_schema_refuse") {
  for (const auto bad : {std::string("\xc0\xaf", 2), std::string("\xed\xa0\x80", 3), std::string("\xf4\x90\x80\x80", 4)}) {
    SupportFixture fixture;
    fixture.producer.summary.insert(1, "\"private\":\"" + bad + "\",");
    REQUIRE_ERROR(wfg::CollectSupport(Inputs(), fixture), wfg::ErrorCode::SupportBundleFailure);
    CHECK(fixture.commits == 0);
  }
  SupportFixture fixture;
  fixture.producer.summary.insert(1, "\"schema_version\":1,");
  REQUIRE_ERROR(wfg::CollectSupport(Inputs(), fixture), wfg::ErrorCode::SupportBundleFailure);
}
TEST("support.unknown_schema_invalid_shapes_and_json_refuse") {
  for (const auto replacement : {"0", "2", "\"1\"", "1.0", "01"}) {
    SupportFixture fixture; Replace(fixture.producer.summary, "\"schema_version\":1", std::string("\"schema_version\":") + replacement);
    REQUIRE_ERROR(wfg::CollectSupport(Inputs(), fixture), wfg::ErrorCode::SupportBundleFailure);
  }
  for (const auto bad : {"{}", "{", "{\"schema_version\":1}", "[]"}) {
    SupportFixture fixture; fixture.producer.summary = bad;
    REQUIRE_ERROR(wfg::CollectSupport(Inputs(), fixture), wfg::ErrorCode::SupportBundleFailure);
  }
}
TEST("support.missing_logs_still_produces_current_metadata") {
  SupportFixture fixture; fixture.missing = true; fixture.producer.events.clear();
  REQUIRE_OK(wfg::CollectSupport(Inputs(), fixture)); CHECK(fixture.reads == 0); CHECK(fixture.commits == 1);
}
TEST("support.reparse_and_identity_replacement_refuse_before_commit") {
  SupportFixture fixture; fixture.reparse = true;
  REQUIRE_ERROR(wfg::CollectSupport(Inputs(), fixture), wfg::ErrorCode::SupportBundleFailure); CHECK(fixture.reads == 0);
  fixture.reparse = false; fixture.replacement = true;
  REQUIRE_ERROR(wfg::CollectSupport(Inputs(), fixture), wfg::ErrorCode::SupportBundleFailure); CHECK(fixture.commits == 0);
}
TEST("support.metadata_secret_tokens_are_not_exported") {
  SupportFixture fixture; auto inputs = Inputs(); inputs.metadata.build_id = "PRIVATE_SECRET"; inputs.metadata.package_version = "PRIVATE_SECRET";
  REQUIRE_OK(wfg::CollectSupport(inputs, fixture)); CHECK(fixture.archive.find("PRIVATE_SECRET") == std::string::npos);
}
TEST("support.unknown_operation_tokens_are_redacted_from_typed_and_persisted_records") {
  for (unsigned scenario = 0; scenario < 4; ++scenario) {
    SupportFixture fixture;
    auto inputs = Inputs();
    const wfg::SupportRunRecord run{1,wfg::SupportRunOutcome::Refused,wfg::Stage::build_validation,wfg::ErrorCode::UnsupportedBuild,"PRIVATE_SECRET",42,{}};
    const wfg::SupportEventRecord event{1,wfg::Stage::build_validation,wfg::ErrorCode::UnsupportedBuild,"PRIVATE_SECRET",1,{}};
    if (scenario == 0) inputs.runs = std::span<const wfg::SupportRunRecord>(&run,1);
    if (scenario == 1) inputs.events = std::span<const wfg::SupportEventRecord>(&event,1);
    if (scenario == 2) Replace(fixture.producer.summary,"\"operation\":\"ValidateBuild\"","\"operation\":\"PRIVATE_SECRET\"");
    if (scenario == 3) {
      while (fixture.producer.events.find("\"operation\":\"ValidateBuild\"") != std::string::npos)
        Replace(fixture.producer.events,"\"operation\":\"ValidateBuild\"","\"operation\":\"PRIVATE_SECRET\"");
    }
    REQUIRE_OK(wfg::CollectSupport(inputs,fixture));
    CHECK(fixture.archive.find("PRIVATE_SECRET") == std::string::npos);
    CHECK(fixture.archive.find("WFG-E-BUILD_UNSUPPORTED") != std::string::npos);
  }
}
TEST("support.typed_invalid_enums_refuse_instead_of_becoming_safe_defaults") {
  auto inputs = Inputs();
  wfg::SupportRunRecord run{1,static_cast<wfg::SupportRunOutcome>(999),wfg::Stage::build_validation,wfg::ErrorCode::UnsupportedBuild,"ValidateBuild",42,{}};
  inputs.runs = std::span<const wfg::SupportRunRecord>(&run,1);
  REQUIRE_ERROR(wfg::BuildSupportPayload(inputs),wfg::ErrorCode::SupportBundleFailure);
  run.outcome = wfg::SupportRunOutcome::Refused; run.stage = static_cast<wfg::Stage>(999);
  REQUIRE_ERROR(wfg::BuildSupportPayload(inputs),wfg::ErrorCode::SupportBundleFailure);
  run.stage = wfg::Stage::build_validation; run.error = static_cast<wfg::ErrorCode>(999);
  REQUIRE_ERROR(wfg::BuildSupportPayload(inputs),wfg::ErrorCode::SupportBundleFailure);
}
TEST("support.large_partial_jsonl_is_explicitly_truncated") {
  SupportFixture fixture; const auto line = fixture.producer.events;
  fixture.producer.events.clear(); for (unsigned index = 0; index < 50; ++index) fixture.producer.events += line;
  fixture.producer.events += "{\"partial\":";
  const auto bundle = REQUIRE_OK(wfg::CollectSupport(Inputs(), fixture)); CHECK(bundle.truncated);
  CHECK(fixture.archive.find("\"truncated\":true") != std::string::npos);
}
TEST("support.write_failure_has_no_completed_output") {
  SupportFixture fixture; fixture.fail_write = true;
  REQUIRE_ERROR(wfg::CollectSupport(Inputs(), fixture), wfg::ErrorCode::SupportBundleFailure); CHECK(fixture.commits == 0);
}
TEST("support.persisted_invalid_failure_config_and_observation_refuse") {
  for (unsigned scenario = 0; scenario < 4; ++scenario) {
    SupportFixture fixture;
    if (scenario == 0) Replace(fixture.producer.summary,"\"max_file_mib\":2","\"max_file_mib\":999");
    if (scenario == 1) Replace(fixture.producer.summary,"\"availability\":\"observed\"","\"availability\":\"working\"");
    if (scenario == 2) Replace(fixture.producer.summary,"\"stage\":\"build_validation\"","\"stage\":\"secret\"");
    if (scenario == 3) Replace(fixture.producer.summary,"\"code\":\"WFG-E-BUILD_UNSUPPORTED\"","\"code\":\"PRIVATE_SECRET\"");
    REQUIRE_ERROR(wfg::CollectSupport(Inputs(),fixture),wfg::ErrorCode::SupportBundleFailure);
    CHECK(fixture.commits == 0);
  }
}
TEST("support.malformed_retained_jsonl_and_schema_refuse") {
  for (unsigned scenario = 0; scenario < 4; ++scenario) {
    SupportFixture fixture;
    if (scenario == 0) fixture.producer.events = "{\"malformed\":}\n";
    if (scenario == 1) fixture.producer.events.insert(1,"\"schema_version\":2,");
    if (scenario == 2) fixture.producer.events.insert(1,"\"private\":\"\\ud800\",");
    if (scenario == 3) fixture.producer.events.insert(1,std::string("\"private\":\"") + std::string("\xff",1) + "\",");
    REQUIRE_ERROR(wfg::CollectSupport(Inputs(),fixture),wfg::ErrorCode::SupportBundleFailure);
    CHECK(fixture.commits == 0);
  }
}
TEST("support.final_python_fixture_uses_real_producer") {
  SupportFixture fixture; REQUIRE_OK(wfg::CollectSupport(Inputs(), fixture));
}

#ifdef WFG_SUPPORT_TESTING
TEST("support.archive_entry_limits_names_and_duplicates") {
  std::vector<wfg::BundleEntry> entries{{"metadata.json", std::vector<std::byte>(10U * 1024U * 1024U)}};
  REQUIRE_OK(wfg::ValidateSupportArchiveEntriesForTest(entries));
  entries[0].bytes.push_back(std::byte{});
  REQUIRE_ERROR(wfg::ValidateSupportArchiveEntriesForTest(entries), wfg::ErrorCode::SupportBundleFailure);
  entries[0].bytes.clear();
  entries.push_back(entries[0]);
  REQUIRE_ERROR(wfg::ValidateSupportArchiveEntriesForTest(entries), wfg::ErrorCode::SupportBundleFailure);
  for (const auto name : {"../private","C:/private","nested/../../save","unknown.txt","METADATA.JSON","metadata.json:stream"}) {
    entries = {{name,{}}};
    REQUIRE_ERROR(wfg::ValidateSupportArchiveEntriesForTest(entries), wfg::ErrorCode::SupportBundleFailure);
  }
  entries.assign(33, {"metadata.json",{}});
  REQUIRE_ERROR(wfg::ValidateSupportArchiveEntriesForTest(entries), wfg::ErrorCode::SupportBundleFailure);
}
#endif

#if defined(_WIN32) && defined(WFG_SUPPORT_TESTING)
namespace {
class NativeFixture {
public:
  NativeFixture() {
    GUID id{}; CHECK(SUCCEEDED(CoCreateGuid(&id)));
    wchar_t text[40]{}; CHECK(StringFromGUID2(id,text,40) != 0);
    directory = std::filesystem::temp_directory_path() / (std::wstring(L"WfgTask8-") + text);
    CHECK(std::filesystem::create_directory(directory));
    root = directory / L"WitcherFG";
    std::filesystem::create_directories(root / L"diagnostics");
  }
  ~NativeFixture() { std::error_code error; std::filesystem::remove_all(directory,error); }
  void Produce(unsigned events = 1) {
    auto record = Record(); wfg::FileLogStorage storage(root / L"diagnostics"); wfg::RunLogger logger(record.effective_config,storage);
    wfg::DiagnosticEvent event; event.level = wfg::EventLevel::Failure; event.code = wfg::EventCode::Failure;
    event.failure = wfg::Error{wfg::ErrorCode::UnsupportedBuild,wfg::Stage::build_validation,"ValidateBuild",{},{}};
    std::memcpy(event.time_utc.data(),"2026-10-10T12:34:56Z",20);
    const std::array fields{wfg::DiagnosticField{wfg::DiagnosticFieldKey::ImportCount,wfg::DiagnosticFieldSource::PeInspection,std::uint64_t{4}}};
    event.fields = fields;
    for (unsigned index = 0; index < events; ++index) { event.elapsed_ms = index; REQUIRE_OK(logger.Emit(event)); }
    REQUIRE_OK(logger.Finish(record));
  }
  unsigned ZipCount() const {
    unsigned count{}; if (!std::filesystem::exists(root / L"support")) return count;
    for (const auto& entry : std::filesystem::directory_iterator(root / L"support")) if (entry.path().extension() == L".zip") ++count;
    return count;
  }
  std::filesystem::path directory, root;
};
std::string ReadFileText(const std::filesystem::path& path) {
  std::ifstream input(path,std::ios::binary); CHECK(input.good());
  return {std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
}
void Junction(const std::filesystem::path& link, const std::filesystem::path& target) {
  std::filesystem::create_directory(link);
  const HANDLE handle = CreateFileW(link.c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
  CHECK(handle != INVALID_HANDLE_VALUE);
  struct MountPoint {
    DWORD tag; WORD length; WORD reserved;
    WORD substitute_offset; WORD substitute_length; WORD print_offset; WORD print_length;
    wchar_t buffer[2048];
  } data{};
  const auto substitute = std::wstring(L"\\??\\") + target.wstring();
  CHECK(substitute.size() + target.wstring().size() + 2 < 2048);
  data.tag = IO_REPARSE_TAG_MOUNT_POINT;
  data.substitute_length = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
  data.print_offset = static_cast<WORD>(data.substitute_length + sizeof(wchar_t));
  data.print_length = static_cast<WORD>(target.wstring().size() * sizeof(wchar_t));
  std::memcpy(data.buffer,substitute.c_str(),data.substitute_length);
  std::memcpy(reinterpret_cast<std::byte*>(data.buffer) + data.print_offset,target.c_str(),data.print_length);
  data.length = static_cast<WORD>(8 + data.substitute_length + data.print_length + 4);
  DWORD returned{}; const bool okay = DeviceIoControl(handle,FSCTL_SET_REPARSE_POINT,&data,data.length + 8,nullptr,0,&returned,nullptr) != 0;
  CloseHandle(handle); CHECK(okay);
}
}
TEST("support.native_missing_logs_and_fixed_directory_output") {
  NativeFixture fixture;
  wfg::WindowsSupportStorage storage(fixture.root);
  const auto result = wfg::CollectSupport(Inputs(),storage);
  if (!wfg::IsOk(result)) throw std::runtime_error(std::string(wfg::Failure(result).operation.view()) + " win32=" + std::to_string(wfg::Failure(result).native_code.value_or(0)));
  const auto bundle = wfg::Value(result);
  CHECK(bundle.local_output.parent_path() == fixture.root / L"support");
  CHECK(fixture.ZipCount() == 1);
}
TEST("support.native_hardlinks_refuse_source_without_output") {
  NativeFixture fixture; fixture.Produce();
  CHECK(CreateHardLinkW((fixture.root / L"diagnostics" / L"extra-link").c_str(),(fixture.root / L"diagnostics" / L"last-run.json").c_str(),nullptr));
  wfg::WindowsSupportStorage storage(fixture.root);
  REQUIRE_ERROR(wfg::CollectSupport(Inputs(),storage),wfg::ErrorCode::SupportBundleFailure);
  CHECK(fixture.ZipCount() == 0);
}
TEST("support.native_unexpected_files_refuse_without_reading_them") {
  NativeFixture fixture; fixture.Produce();
  { std::ofstream private_file(fixture.root / L"diagnostics" / L"private.dmp"); private_file << "PRIVATE_SECRET"; }
  wfg::WindowsSupportStorage storage(fixture.root);
  REQUIRE_ERROR(wfg::CollectSupport(Inputs(),storage),wfg::ErrorCode::SupportBundleFailure);
  CHECK(fixture.ZipCount() == 0);
}
TEST("support.native_hardlink_added_after_open_refuses_read") {
  NativeFixture fixture; fixture.Produce();
  wfg::WindowsSupportStorage storage(fixture.root);
  const auto handle = REQUIRE_OK(storage.OpenSanitizedSummary());
  CHECK(CreateHardLinkW((fixture.directory / L"new-link").c_str(),(fixture.root / L"diagnostics" / L"last-run.json").c_str(),nullptr));
  REQUIRE_ERROR(storage.ReadSanitizedSummary(handle,16U * 1024U),wfg::ErrorCode::SupportBundleFailure);
  CHECK(fixture.ZipCount() == 0);
}
TEST("support.native_reparse_directory_refuses") {
  NativeFixture fixture; fixture.Produce();
  std::filesystem::rename(fixture.root / L"diagnostics",fixture.directory / L"external");
  Junction(fixture.root / L"diagnostics",fixture.directory / L"external");
  {
    wfg::WindowsSupportStorage storage(fixture.root);
    REQUIRE_ERROR(wfg::CollectSupport(Inputs(),storage),wfg::ErrorCode::SupportBundleFailure);
    CHECK(fixture.ZipCount() == 0);
  }
  CHECK(RemoveDirectoryW((fixture.root / L"diagnostics").c_str()));
}
TEST("support.native_parent_and_file_replacement_denied_while_pinned") {
  NativeFixture fixture; fixture.Produce();
  wfg::WindowsSupportStorage storage(fixture.root);
  const auto handle = REQUIRE_OK(storage.OpenSanitizedSummary());
  CHECK(!MoveFileExW(fixture.root.c_str(),(fixture.directory / L"moved-root").c_str(),0));
  CHECK(!MoveFileExW((fixture.root / L"diagnostics").c_str(),(fixture.root / L"moved-diagnostics").c_str(),0));
  CHECK(!MoveFileExW((fixture.root / L"diagnostics" / L"last-run.json").c_str(),(fixture.root / L"diagnostics" / L"moved.json").c_str(),0));
  const HANDLE writer = CreateFileW((fixture.root / L"diagnostics" / L"last-run.json").c_str(),GENERIC_WRITE,FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
  CHECK(writer == INVALID_HANDLE_VALUE);
  REQUIRE_OK(storage.ReadSanitizedSummary(handle,16U * 1024U));
  REQUIRE_ERROR(storage.ReadSanitizedSummary(handle,16U * 1024U),wfg::ErrorCode::SupportBundleFailure);
}
TEST("support.native_source_sharing_denial_refuses") {
  NativeFixture fixture; fixture.Produce();
  const HANDLE writer = CreateFileW((fixture.root / L"diagnostics" / L"last-run.json").c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
  CHECK(writer != INVALID_HANDLE_VALUE);
  { wfg::WindowsSupportStorage storage(fixture.root); REQUIRE_ERROR(wfg::CollectSupport(Inputs(),storage),wfg::ErrorCode::SupportBundleFailure); CHECK(fixture.ZipCount() == 0); }
  CloseHandle(writer);
}
TEST("support.native_output_write_denied_by_disposable_directory_acl") {
  NativeFixture fixture; fixture.Produce();
  const auto path = fixture.root / L"support"; std::filesystem::create_directory(path);
  DWORD needed{}; GetFileSecurityW(path.c_str(),DACL_SECURITY_INFORMATION,nullptr,0,&needed); CHECK(needed != 0);
  std::vector<std::byte> original(needed); CHECK(GetFileSecurityW(path.c_str(),DACL_SECURITY_INFORMATION,original.data(),needed,&needed));
  struct Restore {
    std::filesystem::path path; std::vector<std::byte>& descriptor;
    ~Restore() { SetFileSecurityW(path.c_str(),DACL_SECURITY_INFORMATION,descriptor.data()); }
  } restore{path,original};
  PSECURITY_DESCRIPTOR denied{};
  CHECK(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(D;;0x00000002;;;WD)(A;;FA;;;WD)",SDDL_REVISION_1,&denied,nullptr));
  const bool changed = SetFileSecurityW(path.c_str(),DACL_SECURITY_INFORMATION,denied) != 0;
  LocalFree(denied); CHECK(changed);
  wfg::WindowsSupportStorage storage(fixture.root);
  REQUIRE_ERROR(wfg::CollectSupport(Inputs(),storage),wfg::ErrorCode::SupportBundleFailure);
  CHECK(fixture.ZipCount() == 0); CHECK(storage.CompletedArchiveLocation().empty());
}
TEST("support.native_actual_persisted_schema_and_utf8_attacks") {
  for (unsigned scenario = 0; scenario < 4; ++scenario) {
    NativeFixture fixture; fixture.Produce();
    const auto source = fixture.root / L"diagnostics" / L"last-run.json";
    auto text = ReadFileText(source);
    if (scenario == 0) Replace(text,"\"schema_version\":1","\"schema_version\":2");
    if (scenario == 1) text.insert(1,std::string("\"private\":\"") + std::string("\xff",1) + "\",");
    if (scenario == 2) text.insert(1,"\"schema_version\":1,");
    if (scenario == 3) text.insert(1,R"("private":{"path":"C:\\private","token":"PRIVATE_SECRET"},)");
    { std::ofstream file(source,std::ios::binary); file.write(text.data(),static_cast<std::streamsize>(text.size())); }
    wfg::WindowsSupportStorage storage(fixture.root);
    if (scenario == 3) {
      const auto bundle = REQUIRE_OK(wfg::CollectSupport(Inputs(),storage));
      CHECK(ReadFileText(bundle.local_output).find("PRIVATE_SECRET") == std::string::npos);
    } else {
      REQUIRE_ERROR(wfg::CollectSupport(Inputs(),storage),wfg::ErrorCode::SupportBundleFailure);
      CHECK(fixture.ZipCount() == 0);
    }
  }
}
TEST("support.native_absent_diagnostics_directory_works") {
  NativeFixture fixture;
  CHECK(std::filesystem::remove(fixture.root / L"diagnostics"));
  wfg::WindowsSupportStorage storage(fixture.root);
  REQUIRE_OK(wfg::CollectSupport(Inputs(),storage)); CHECK(fixture.ZipCount() == 1);
}
TEST("support.native_atomic_write_and_rename_failure_cleanup") {
  for (bool rename : {false,true}) {
    NativeFixture fixture; fixture.Produce();
    wfg::WindowsSupportStorage storage(fixture.root);
    storage.fail_before_write = !rename; storage.fail_before_rename = rename;
    REQUIRE_ERROR(wfg::CollectSupport(Inputs(),storage),wfg::ErrorCode::SupportBundleFailure);
    CHECK(storage.CompletedArchiveLocation().empty()); CHECK(fixture.ZipCount() == 0);
    CHECK(std::filesystem::is_empty(fixture.root / L"support"));
  }
}
TEST("support.native_support_reparse_and_parent_component_refuse") {
  NativeFixture fixture; fixture.Produce();
  std::filesystem::create_directory(fixture.directory / L"external");
  Junction(fixture.root / L"support",fixture.directory / L"external");
  { wfg::WindowsSupportStorage storage(fixture.root); REQUIRE_ERROR(wfg::CollectSupport(Inputs(),storage),wfg::ErrorCode::SupportBundleFailure); CHECK(std::filesystem::is_empty(fixture.directory / L"external")); }
  CHECK(RemoveDirectoryW((fixture.root / L"support").c_str()));
  Junction(fixture.directory / L"alias",fixture.root);
  { wfg::WindowsSupportStorage storage(fixture.directory / L"alias"); REQUIRE_ERROR(wfg::CollectSupport(Inputs(),storage),wfg::ErrorCode::SupportBundleFailure); }
  CHECK(RemoveDirectoryW((fixture.directory / L"alias").c_str()));
}
TEST("support.native_large_partial_and_rotated_producer_jsonl") {
  NativeFixture fixture; fixture.Produce(400);
  const auto source = fixture.root / L"diagnostics" / L"events.jsonl";
  std::filesystem::rename(source,fixture.root / L"diagnostics" / L"events.1.jsonl");
  { std::ofstream partial(source,std::ios::binary); partial << "{\"partial\":"; }
  wfg::WindowsSupportStorage storage(fixture.root);
  const auto bundle = REQUIRE_OK(wfg::CollectSupport(Inputs(),storage)); CHECK(bundle.truncated);
  CHECK(ReadFileText(bundle.local_output).find("\"truncated\":true") != std::string::npos);
}
TEST("support.native_real_producer_zip_for_independent_python") {
  NativeFixture fixture; fixture.Produce(2);
  const auto before = ReadFileText(fixture.root / L"diagnostics" / L"last-run.json");
  {
    wfg::WindowsSupportStorage storage(fixture.root);
    const auto bundle = REQUIRE_OK(wfg::CollectSupport(Inputs(),storage));
    CHECK(before == ReadFileText(fixture.root / L"diagnostics" / L"last-run.json"));
    const auto* selected = std::getenv("WFG_SUPPORT_NATIVE_FIXTURE"); CHECK(selected != nullptr);
    std::filesystem::copy_file(bundle.local_output,selected,std::filesystem::copy_options::overwrite_existing);
  }
}
#endif
