#include "support.hpp"
#include "wfg/windows_process.hpp"
#include "fixtures/process_identity.hpp"
#include "fixtures/verified_identity.hpp"
#include "fixtures/runtime_images.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <cstring>

namespace {
struct BundledDebugHelperFacts {
  wfg::process_detail::FileEvidence file{1138120, {}, "6.2.9200.16384", true, wfg::DependencySigner::Microsoft};
  std::vector<std::byte> header=std::vector<std::byte>(512);
  std::string package="CDPROJEKTSA.Witcher3GOTY",version="5.0.4.0";
  std::wstring path=L"bin\\Gaming.Desktop.x64\\dbghelp.dll";
  BundledDebugHelperFacts() {
    constexpr std::string_view hash="7460518255f4c024b81f171ef7f5c0671b0a2b1d64f509cac848c6c8da7ba241";
    const auto hex=[](char c){return c<='9'?c-'0':c-'a'+10;};
    for(std::size_t i=0;i<file.sha256.size();++i)file.sha256[i]=std::byte((hex(hash[i*2])<<4)|hex(hash[i*2+1]));
    Put<std::uint16_t>(0,IMAGE_DOS_SIGNATURE);Put<std::uint32_t>(60,128);
    Put<std::uint32_t>(128,IMAGE_NT_SIGNATURE);Put<std::uint16_t>(132,IMAGE_FILE_MACHINE_I386);
    Put<std::uint32_t>(136,0x5010a75d);Put<std::uint16_t>(148,sizeof(IMAGE_OPTIONAL_HEADER32));
    Put<std::uint16_t>(152,IMAGE_NT_OPTIONAL_HDR32_MAGIC);Put<std::uint32_t>(208,0x12c000);
  }
  template<class T>void Put(std::size_t offset,T value){std::memcpy(header.data()+offset,&value,sizeof(value));}
  bool Qualifies()const{return wfg::process_detail::QualifiesBundledDebugHelper(file,header,package,version,path);}
};
std::unique_ptr<wfg::ProcessObserver> Observer(wfg::Result<std::unique_ptr<wfg::ProcessObserver>> result) {
  CHECK(wfg::IsOk(result));
  return std::move(std::get<std::unique_ptr<wfg::ProcessObserver>>(result));
}
}

namespace {
struct PackageDocuments {
  std::filesystem::path root;
  wfg::process_detail::RegisteredPackageMetadata identity{L"CDPROJEKTSA.Witcher3GOTY", L"CN=Test Publisher", L"5.0.4.0", L"TEST", L"x64"};
  std::string manifest = R"(<Package xmlns="http://schemas.microsoft.com/appx/manifest/foundation/windows10"><Identity Name="CDPROJEKTSA.Witcher3GOTY" Publisher="CN=Test Publisher" Version="5.0.4.0" ResourceId="TEST" ProcessorArchitecture="x64"/><Applications><Application Id="bin.Gaming.Desktop.x64.witcher3" Executable="GameLaunchHelper.exe" EntryPoint="Windows.FullTrustApplication"/></Applications></Package>)";
  std::string config = R"(<Game configVersion="1"><Identity Name="CDPROJEKTSA.Witcher3GOTY" Publisher="CN=Test Publisher" Version="5.0.4.0" ResourceId="TEST"/><ExecutableList><Executable Name="bin\Gaming.Desktop.x64\witcher3.exe" TargetDeviceFamily="PC"/></ExecutableList></Game>)";
  PackageDocuments() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    for (unsigned i=0;i<10;++i) {
      root=std::filesystem::temp_directory_path()/("wfg-package-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(nonce)+"-"+std::to_string(i));
      if(std::filesystem::create_directory(root))return;
    }
    throw std::runtime_error("package fixture directory");
  }
  ~PackageDocuments(){std::error_code error;std::filesystem::remove_all(root,error);}
  static void Replace(std::string& text,std::string_view from,std::string_view to){const auto at=text.find(from);CHECK(at!=std::string::npos);text.replace(at,from.size(),to);}
  wfg::Result<std::wstring> Resolve(bool write_config=true){
    {std::ofstream file(root/L"AppxManifest.xml",std::ios::binary);file<<manifest;CHECK(file.good());}
    if(write_config){std::ofstream file(root/L"MicrosoftGame.Config",std::ios::binary);file<<config;CHECK(file.good());}
    return wfg::process_detail::ResolvePackageExecutable(root.wstring(),identity);
  }
};
}

TEST("process.package_helper_resolves_exact_pc_game_target") {
  PackageDocuments f;
  CHECK(REQUIRE_OK(f.Resolve())==(f.root/L"bin\\Gaming.Desktop.x64\\witcher3.exe").wstring());
}
TEST("process.package_manifest_root_must_be_the_foundation_package_element") {
  // The root guard at the top of ManifestExecutable had no test. Everything downstream -- the
  // Identity, Applications and Application shape checks -- keys off depth and parent names that
  // only mean what they appear to mean if the document root really is the windows10 foundation
  // <Package> element. A bundle manifest or a foreign namespace reaching those checks would be
  // interpreted against the wrong schema.
  {
    // Wrong root element name.
    PackageDocuments f;
    PackageDocuments::Replace(f.manifest, "<Package ", "<Bundle ");
    PackageDocuments::Replace(f.manifest, "</Package>", "</Bundle>");
    const auto refused = f.Resolve();
    REQUIRE_ERROR(refused, wfg::ErrorCode::UnsupportedBuild);
    CHECK(wfg::Failure(refused).operation.view() == "manifest_root");
  }
  {
    // Right element name, wrong namespace.
    PackageDocuments f;
    PackageDocuments::Replace(f.manifest,
      "http://schemas.microsoft.com/appx/manifest/foundation/windows10",
      "http://schemas.microsoft.com/appx/manifest/bundle/windows10");
    const auto refused = f.Resolve();
    REQUIRE_ERROR(refused, wfg::ErrorCode::UnsupportedBuild);
    CHECK(wfg::Failure(refused).operation.view() == "manifest_root");
  }
  {
    // Foundation <Package> present but nested under another root, so it is not at depth 0.
    PackageDocuments f;
    PackageDocuments::Replace(f.manifest, "<Package ", "<Wrapper><Package ");
    PackageDocuments::Replace(f.manifest, "</Package>", "</Package></Wrapper>");
    const auto refused = f.Resolve();
    REQUIRE_ERROR(refused, wfg::ErrorCode::UnsupportedBuild);
    CHECK(wfg::Failure(refused).operation.view() == "manifest_root");
  }
}
TEST("process.package_direct_manifest_retains_valid_resolution_without_config") {
  PackageDocuments f;PackageDocuments::Replace(f.manifest,"GameLaunchHelper.exe","bin\\Gaming.Desktop.x64\\witcher3.exe");
  CHECK(REQUIRE_OK(f.Resolve(false))==(f.root/L"bin\\Gaming.Desktop.x64\\witcher3.exe").wstring());
}
TEST("process.package_helper_requires_config_and_exact_helper_shape") {
  PackageDocuments missing;CHECK(!wfg::IsOk(missing.Resolve(false)));
  for(const auto replacement:{"OtherHelper.exe","sub\\GameLaunchHelper.exe","..\\GameLaunchHelper.exe","C:\\GameLaunchHelper.exe"}){
    PackageDocuments f;PackageDocuments::Replace(f.manifest,"GameLaunchHelper.exe",replacement);CHECK(!wfg::IsOk(f.Resolve()));
  }
  PackageDocuments f;PackageDocuments::Replace(f.manifest,"Windows.FullTrustApplication","Other.EntryPoint");CHECK(!wfg::IsOk(f.Resolve()));
}
TEST("process.package_identity_is_bound_to_registration") {
  for(const auto field:{"Name=\"CDPROJEKTSA.Witcher3GOTY\"","Publisher=\"CN=Test Publisher\"","Version=\"5.0.4.0\"","ResourceId=\"TEST\""}){
    PackageDocuments f;PackageDocuments::Replace(f.config,field,"Changed=\"invalid\"");CHECK(!wfg::IsOk(f.Resolve()));
    PackageDocuments direct;PackageDocuments::Replace(direct.manifest,"GameLaunchHelper.exe","bin\\Gaming.Desktop.x64\\witcher3.exe");PackageDocuments::Replace(direct.manifest,field,"Changed=\"invalid\"");CHECK(!wfg::IsOk(direct.Resolve(false)));
  }
}
TEST("process.package_rejects_unsupported_registered_metadata") {
  for(unsigned field=0;field<5;++field){PackageDocuments f;if(field==0)f.identity.name=L"other";if(field==1)f.identity.publisher.clear();if(field==2)f.identity.version=L"5.0.5.0";if(field==3)f.identity.resource_id=L"other";if(field==4)f.identity.architecture=L"arm64";CHECK(!wfg::IsOk(f.Resolve()));}
}
TEST("process.package_config_target_cannot_escape_or_switch_game") {
  for(const auto target:{"..\\witcher3.exe","C:\\witcher3.exe","/witcher3.exe","bin/Gaming.Desktop.x64/witcher3.exe","bin\\other\\witcher3.exe","bin\\Gaming.Desktop.x64\\other.exe","bin\\Gaming.Desktop.x64\\.\\witcher3.exe","bin\\Gaming.Desktop.x64\\witcher3.exe:stream"}){
    PackageDocuments f;PackageDocuments::Replace(f.config,"bin\\Gaming.Desktop.x64\\witcher3.exe",target);CHECK(!wfg::IsOk(f.Resolve()));
  }
}
TEST("process.package_duplicate_and_misnested_identity_target_refuse") {
  for(const auto insert:{"<Identity Name=\"duplicate\"/>","<ExecutableList/>","<Executable Name=\"bin\\Gaming.Desktop.x64\\witcher3.exe\" TargetDeviceFamily=\"PC\"/>"}){PackageDocuments f;PackageDocuments::Replace(f.config,"</Game>",std::string(insert)+"</Game>");CHECK(!wfg::IsOk(f.Resolve()));}
  PackageDocuments f;PackageDocuments::Replace(f.config,"</ExecutableList>","<Executable Name=\"bin\\Gaming.Desktop.x64\\witcher3.exe\" TargetDeviceFamily=\"PC\"/></ExecutableList>");CHECK(!wfg::IsOk(f.Resolve()));
  PackageDocuments m;PackageDocuments::Replace(m.manifest,"</Applications>","<Application Id=\"bin.Gaming.Desktop.x64.witcher3\" Executable=\"GameLaunchHelper.exe\" EntryPoint=\"Windows.FullTrustApplication\"/></Applications>");CHECK(!wfg::IsOk(m.Resolve()));
}
TEST("process.package_malformed_dtd_oversized_and_wrong_namespace_refuse") {
  for(unsigned variant=0;variant<7;++variant){PackageDocuments f;if(variant==0)f.config+="<broken";if(variant==1)f.config="<!DOCTYPE Game [<!ENTITY target 'witcher3.exe'>]>"+f.config;if(variant==2)f.config=std::string(1024*1024+1,' ');if(variant==3)PackageDocuments::Replace(f.config,"<Game ","<Game xmlns=\"urn:unexpected\" ");if(variant==4)PackageDocuments::Replace(f.config,"configVersion=\"1\"","configVersion=\"2\"");if(variant==5)PackageDocuments::Replace(f.config,"TargetDeviceFamily=\"PC\"","TargetDeviceFamily=\"XboxOne\"");if(variant==6)PackageDocuments::Replace(f.config,"<Identity ","<Identity ProcessorArchitecture=\"arm64\" ");CHECK(!wfg::IsOk(f.Resolve()));}
  PackageDocuments direct;PackageDocuments::Replace(direct.manifest,"GameLaunchHelper.exe","bin\\Gaming.Desktop.x64\\witcher3.exe");direct.manifest="<!DOCTYPE Package [<!ENTITY x 'y'>]>"+direct.manifest;CHECK(!wfg::IsOk(direct.Resolve(false)));
}

TEST("process.native_read_into_exact_short_guard_and_overflow") {
  std::array<std::byte, 4> source{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
  std::array<std::byte, 4> destination{};
  REQUIRE_OK(wfg::process_detail::ReadIntoHandle(GetCurrentProcess(), reinterpret_cast<wfg::Address>(source.data()), destination));
  CHECK(source == destination);
  auto* allocation = static_cast<std::byte*>(VirtualAlloc(nullptr, 8192, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  CHECK(allocation != nullptr);
  struct Free { void* address; ~Free() { VirtualFree(address, 0, MEM_RELEASE); } } free{allocation};
  DWORD previous{};
  CHECK(VirtualProtect(allocation + 4096, 4096, PAGE_NOACCESS, &previous));
  REQUIRE_ERROR(wfg::process_detail::ReadIntoHandle(GetCurrentProcess(), reinterpret_cast<wfg::Address>(allocation + 4094), destination), wfg::ErrorCode::UnreadableMemory);
  REQUIRE_ERROR(wfg::process_detail::ReadIntoHandle(GetCurrentProcess(), UINT64_MAX - 1, destination), wfg::ErrorCode::UnreadableMemory);
}

TEST("process.native_missing_denied_and_unsigned_dll_refuse_trust") {
  wchar_t temp[MAX_PATH]{};
  CHECK(GetTempPathW(MAX_PATH, temp));
  const auto path = std::wstring(temp) + L"wfg-task3-" + std::to_wstring(GetCurrentProcessId()) + L".dll";
  REQUIRE_ERROR(wfg::process_detail::InspectFile(path, true), wfg::ErrorCode::AccessDenied);
  const auto raw = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
  CHECK(raw != INVALID_HANDLE_VALUE);
  struct Close { HANDLE handle; ~Close() { CloseHandle(handle); } } close{raw};
  REQUIRE_ERROR(wfg::process_detail::InspectFile(path, true), wfg::ErrorCode::AccessDenied);
  DWORD written{};
  CHECK(WriteFile(raw, "unsigned", 8, &written, nullptr));
  CHECK(written == 8);
}

TEST("process.native_unsigned_file_fails_offline_trust") {
  wchar_t temp[MAX_PATH]{};
  CHECK(GetTempPathW(MAX_PATH, temp));
  const auto path = std::wstring(temp) + L"wfg-task3-unsigned-" + std::to_wstring(GetCurrentProcessId()) + L".dll";
  struct Delete { std::wstring path; ~Delete() { DeleteFileW(path.c_str()); } } cleanup{path};
  auto file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
  CHECK(file != INVALID_HANDLE_VALUE);
  DWORD written{};
  CHECK(WriteFile(file, "unsigned", 8, &written, nullptr));
  CloseHandle(file);
  REQUIRE_ERROR(wfg::process_detail::InspectFile(path, true), wfg::ErrorCode::UntrustedInterposer);
}

TEST("process.preexisting_between_observer_and_activation_is_refused") {
  ProcessIdentityFixture fixture;
  auto observer = Observer(wfg::InstallProcessObserver(fixture.launch, fixture.backend, {fixture.backend.Now() + std::chrono::seconds(1)}));
  fixture.backend.candidates = {fixture.candidate};
  REQUIRE_ERROR(observer->Activate(), wfg::ErrorCode::AlreadyRunning);
  CHECK(!fixture.backend.ActivationRequested());
}

TEST("process.missing_registration_and_removal_while_waiting_are_refused") {
  ProcessIdentityFixture fixture;
  fixture.backend.registered = false;
  fixture.backend.registration_error = wfg::ErrorCode::NoMatchingProcess;
  REQUIRE_ERROR(wfg::InstallProcessObserver(fixture.launch, fixture.backend, {fixture.backend.Now() + std::chrono::seconds(1)}), wfg::ErrorCode::NoMatchingProcess);
  fixture.backend.registered = true;
  auto observer = Observer(wfg::InstallProcessObserver(fixture.launch, fixture.backend, {fixture.backend.Now() + std::chrono::seconds(1)}));
  REQUIRE_OK(observer->Activate());
  fixture.backend.registered = false;
  fixture.backend.registration_error = wfg::ErrorCode::IdentityChanged;
  REQUIRE_ERROR(observer->WaitForCandidate(), wfg::ErrorCode::IdentityChanged);
}

TEST("process.slow_enumeration_cannot_return_after_deadline") {
  ProcessIdentityFixture fixture;
  auto observer = Observer(wfg::InstallProcessObserver(fixture.launch, fixture.backend, {fixture.backend.Now() + std::chrono::milliseconds(10)}));
  REQUIRE_OK(observer->Activate());
  fixture.backend.enumeration_time = std::chrono::milliseconds(10);
  fixture.backend.candidates = {fixture.candidate};
  REQUIRE_ERROR(observer->WaitForCandidate(), wfg::ErrorCode::TimedOut);
}

TEST("process.native_microsoft_signature_is_not_nvidia") {
  wchar_t system[MAX_PATH]{};
  CHECK(GetSystemDirectoryW(system, MAX_PATH));
  const auto path = std::wstring(system) + L"\\kernel32.dll";
  REQUIRE_ERROR(wfg::process_detail::InspectFile(path, true), wfg::ErrorCode::UntrustedInterposer);
  const auto evidence = REQUIRE_OK(wfg::process_detail::InspectFile(path, false));
  CHECK(evidence.trusted_signature);
  CHECK(evidence.signer == wfg::DependencySigner::Microsoft);
}

TEST("process.wait_requires_activation_and_is_single_consumption") {
  ProcessIdentityFixture fixture;
  auto observer = Observer(wfg::InstallProcessObserver(fixture.launch, fixture.backend, {fixture.backend.Now() + std::chrono::seconds(1)}));
  REQUIRE_ERROR(observer->WaitForCandidate(), wfg::ErrorCode::LaunchFailed);
  REQUIRE_OK(observer->Activate());
  fixture.backend.candidates = {fixture.candidate};
  REQUIRE_OK(observer->WaitForCandidate());
  REQUIRE_ERROR(observer->WaitForCandidate(), wfg::ErrorCode::IdentityChanged);
}

TEST("process.injected_backend_cannot_mint_owned_process_authority") {
  ProcessIdentityFixture fixture;
  auto observer = Observer(wfg::InstallProcessObserver(fixture.launch, fixture.backend, {fixture.backend.Now() + std::chrono::seconds(1)}));
  REQUIRE_OK(observer->Activate());
  fixture.candidate.pin = std::shared_ptr<void>(GetCurrentProcess(), [](void*) noexcept {});
  fixture.backend.candidates = {fixture.candidate};
  const auto result = observer->WaitForFreshProcess();
  CHECK(!wfg::IsOk(result));
  CHECK(wfg::Failure(result).code == wfg::ErrorCode::IdentityChanged);
  CHECK(wfg::Failure(result).operation.view() == "native_observer_required");
}

TEST("process.broker_and_reused_pid_are_rejected") {
  ProcessIdentityFixture fixture;
  auto broker = fixture.candidate;
  broker.executable = L"C:\\Windows\\explorer.exe";
  REQUIRE_ERROR(wfg::SelectCandidate(std::span(&broker, 1), fixture.launch), wfg::ErrorCode::NoMatchingProcess);
  auto reused = fixture.candidate;
  reused.pinned_creation_time++;
  REQUIRE_ERROR(wfg::SelectCandidate(std::span(&reused, 1), fixture.launch), wfg::ErrorCode::IdentityChanged);
}

TEST("process.ambiguous_or_existing_game_is_not_attached") {
  ProcessIdentityFixture fixture;
  std::array candidates{fixture.candidate, fixture.candidate};
  candidates[1].pid++;
  candidates[1].pinned_pid++;
  REQUIRE_ERROR(wfg::SelectCandidate(candidates, fixture.launch), wfg::ErrorCode::AmbiguousProcess);
  candidates[0].creation_time = 99;
  candidates[0].pinned_creation_time = 99;
  REQUIRE_ERROR(wfg::SelectCandidate(std::span(candidates.data(), 1), fixture.launch), wfg::ErrorCode::AlreadyRunning);
}

TEST("process.session_user_package_and_executable_are_mandatory") {
  ProcessIdentityFixture fixture;
  for (int field = 0; field != 4; ++field) {
    auto candidate = fixture.candidate;
    if (field == 0) candidate.session++;
    if (field == 1) candidate.user = L"another_user";
    if (field == 2) candidate.package = L"another_package";
    if (field == 3) candidate.executable = L"D:\\other\\witcher3.exe";
    REQUIRE_ERROR(wfg::SelectCandidate(std::span(&candidate, 1), fixture.launch), wfg::ErrorCode::NoMatchingProcess);
  }
  auto valid = REQUIRE_OK(wfg::SelectCandidate(std::span(&fixture.candidate, 1), fixture.launch));
  CHECK(valid.pid == 42);
}

TEST("process.different_drive_and_pinned_identity_are_supported") {
  ProcessIdentityFixture fixture;
  fixture.launch.executable = L"Z:\\XboxGames\\Witcher\\bin\\witcher3.exe";
  fixture.candidate.executable = fixture.launch.executable;
  REQUIRE_OK(wfg::SelectCandidate(std::span(&fixture.candidate, 1), fixture.launch));
  fixture.candidate.pinned_pid++;
  REQUIRE_ERROR(wfg::SelectCandidate(std::span(&fixture.candidate, 1), fixture.launch), wfg::ErrorCode::IdentityChanged);
}

TEST("process.observer_precedes_activation_and_polls_at_twenty_ms") {
  ProcessIdentityFixture fixture;
  auto observer = Observer(wfg::InstallProcessObserver(fixture.launch, fixture.backend, {fixture.backend.Now() + std::chrono::seconds(60)}));
  CHECK(fixture.backend.events == std::vector<std::string>{"install"});
  REQUIRE_OK(observer->Activate());
  fixture.backend.candidates = {fixture.candidate};
  REQUIRE_OK(observer->WaitForCandidate());
  CHECK(fixture.backend.events[1] == "activate");
}

TEST("process.discovery_has_compiled_thirty_second_monotonic_bound") {
  ProcessIdentityFixture fixture;
  auto observer = Observer(wfg::InstallProcessObserver(fixture.launch, fixture.backend, {fixture.backend.Now() + std::chrono::seconds(60)}));
  REQUIRE_OK(observer->Activate());
  REQUIRE_ERROR(observer->WaitForCandidate(), wfg::ErrorCode::TimedOut);
  CHECK(fixture.backend.elapsed == std::chrono::seconds(30));
  CHECK(fixture.backend.sleeps == 1500);
  CHECK(fixture.backend.max_sleep == std::chrono::milliseconds(20));
}

TEST("process.expired_deadline_and_package_removal_refuse") {
  ProcessIdentityFixture fixture;
  REQUIRE_ERROR(wfg::InstallProcessObserver(fixture.launch, fixture.backend, {fixture.backend.Now()}), wfg::ErrorCode::TimedOut);
  auto observer = Observer(wfg::InstallProcessObserver(fixture.launch, fixture.backend, {fixture.backend.Now() + std::chrono::seconds(1)}));
  fixture.backend.registered = false;
  REQUIRE_ERROR(observer->Activate(), wfg::ErrorCode::IdentityChanged);
}

TEST("process.readonly_selection_refuses_ambiguity_and_reuse") {
  ProcessIdentityFixture fixture;
  fixture.launch.require_fresh = false;
  std::array candidates{fixture.candidate, fixture.candidate};
  candidates[1].pid++;
  candidates[1].pinned_pid++;
  REQUIRE_ERROR(wfg::SelectCandidate(candidates, fixture.launch), wfg::ErrorCode::AmbiguousProcess);
  fixture.candidate.pinned_creation_time++;
  REQUIRE_ERROR(wfg::SelectCandidate(std::span(&fixture.candidate, 1), fixture.launch), wfg::ErrorCode::IdentityChanged);
}

TEST("process.environment_never_guesses_active_adapter_or_hags") {
  const auto environment = wfg::ObserveEnvironment(nullptr);
  CHECK(!environment.selected_adapter.value);
  CHECK(!environment.hags_active.value);
}

TEST("process.environment_records_os_enumerated_adapters_drivers_and_configured_hags") {
  EnvironmentBackendFixture backend;
  const auto environment = wfg::process_detail::ObserveHostEnvironment(backend);
  CHECK(environment.os.value == "Windows 10.0 build 26200");
  CHECK(environment.os.availability == wfg::Availability::Observed);
  CHECK(environment.os.source == "ntdll_RtlGetVersion");
  CHECK(environment.gpu.value && environment.gpu.value->find("NVIDIA Test GPU") != std::string::npos);
  CHECK(environment.gpu.value->find("Intel Test GPU") != std::string::npos);
  CHECK(environment.gpu.source == "DXGI_EnumAdapters1_GetDesc1");
  CHECK(environment.driver.value && environment.driver.value->find("32.0.15.1234") != std::string::npos);
  CHECK(environment.driver.value->find("31.0.101.1234") != std::string::npos);
  CHECK(environment.driver.source == "DXGI_CheckInterfaceSupport_IDXGIDevice");
  CHECK(environment.hags_configured.value == true);
  CHECK(!environment.selected_adapter.value && !environment.hags_active.value);
  backend.hags = std::optional<std::uint32_t>{1};
  CHECK(wfg::process_detail::ObserveHostEnvironment(backend).hags_configured.value == false);
}

TEST("process.environment_failed_queries_preserve_failure_provenance_and_unknowns") {
  EnvironmentBackendFixture backend;
  backend.os = wfg::Error{wfg::ErrorCode::AccessDenied, wfg::Stage::Diagnostic, "os_version_query", 5};
  backend.adapter_error = wfg::Error{wfg::ErrorCode::AccessDenied, wfg::Stage::Diagnostic, "dxgi_factory_trust", 5};
  backend.hags = wfg::Error{wfg::ErrorCode::AccessDenied, wfg::Stage::Diagnostic, "hags_registry_read", 5};
  const auto environment = wfg::process_detail::ObserveHostEnvironment(backend);
  CHECK(!environment.os.value && environment.os.availability == wfg::Availability::ReadFailed);
  CHECK(environment.os.reason == "os_version_query_native_5");
  CHECK(!environment.gpu.value && environment.gpu.availability == wfg::Availability::ReadFailed);
  CHECK(environment.gpu.reason == "dxgi_factory_trust_native_5");
  CHECK(!environment.driver.value && environment.driver.availability == wfg::Availability::ReadFailed);
  CHECK(!environment.hags_configured.value && environment.hags_configured.availability == wfg::Availability::ReadFailed);
  CHECK(environment.hags_configured.reason == "hags_registry_read_native_5");
  CHECK(!environment.selected_adapter.value && !environment.hags_active.value);
}

TEST("process.environment_hags_missing_or_unknown_and_driver_failure_do_not_guess") {
  EnvironmentBackendFixture backend;
  backend.hags = std::optional<std::uint32_t>{};
  backend.adapters[0].driver_version = wfg::Error{wfg::ErrorCode::UnsupportedObservation, wfg::Stage::Diagnostic, "dxgi_driver_unsupported"};
  auto environment = wfg::process_detail::ObserveHostEnvironment(backend);
  CHECK(environment.gpu.value.has_value());
  CHECK(!environment.driver.value && environment.driver.availability == wfg::Availability::UnsupportedLayout);
  CHECK(!environment.hags_configured.value && environment.hags_configured.availability == wfg::Availability::NotObserved);
  backend.hags = std::optional<std::uint32_t>{0};
  environment = wfg::process_detail::ObserveHostEnvironment(backend);
  CHECK(!environment.hags_configured.value && environment.hags_configured.availability == wfg::Availability::UnsupportedLayout);
}

TEST("process.environment_adapter_enumeration_is_bounded") {
  EnvironmentBackendFixture backend;
  backend.repeat_adapter = true;
  const auto environment = wfg::process_detail::ObserveHostEnvironment(backend);
  CHECK(backend.adapter_queries <= 32);
  CHECK(!environment.gpu.value && environment.gpu.availability == wfg::Availability::UnsupportedLayout);
  CHECK(!environment.driver.value);
}

TEST("process.native_environment_queries_host_without_asserting_active_state") {
  const auto environment = wfg::ObserveEnvironment(nullptr);
  CHECK(environment.os.value && environment.os.availability == wfg::Availability::Observed);
  CHECK(environment.os.source == "ntdll_RtlGetVersion");
  CHECK(environment.gpu.source == "DXGI_EnumAdapters1_GetDesc1");
  CHECK(environment.gpu.value || !environment.gpu.reason.empty());
  CHECK(environment.hags_configured.source == "HKLM_GraphicsDrivers_HwSchMode");
  CHECK(environment.hags_configured.value || !environment.hags_configured.reason.empty());
  CHECK(!environment.selected_adapter.value && !environment.hags_active.value);
}

TEST("process.native_catalog_trusted_dxgi_reaches_adapter_enumeration") {
  const auto environment = wfg::ObserveEnvironment(nullptr);
  if (!environment.gpu.value) throw std::runtime_error("expected native GPU observation; actual=" + environment.gpu.reason);
  CHECK(environment.gpu.availability == wfg::Availability::Observed);
  CHECK(environment.gpu.source == "DXGI_EnumAdapters1_GetDesc1");
  CHECK(environment.gpu.value->find("adapter 0 ") != std::string::npos);
  if (environment.driver.value) CHECK(environment.driver.availability == wfg::Availability::Observed);
  else {
    CHECK(environment.driver.availability == wfg::Availability::UnsupportedLayout || environment.driver.availability == wfg::Availability::ReadFailed);
    CHECK(!environment.driver.reason.empty());
  }
  CHECK(!environment.selected_adapter.value && !environment.hags_active.value);
}

TEST("process.prelaunch_delay_cannot_cross_activation_deadline") {
  ActivationOperationsFixture operations;
  operations.preparation_time = std::chrono::milliseconds(10);
  const auto activation_result = wfg::process_detail::ActivateBeforeDeadline(operations, {operations.Now() + std::chrono::milliseconds(10)});
  REQUIRE_ERROR(activation_result, wfg::ErrorCode::TimedOut);
  CHECK(wfg::Failure(activation_result).stage == wfg::Stage::Launch);
  CHECK(operations.launch_calls == 0);
}

TEST("process.prelaunch_failure_and_unexpired_launch_keep_exact_sideeffect_count") {
  ActivationOperationsFixture operations;
  operations.preparation = wfg::Error{wfg::ErrorCode::LaunchFailed, wfg::Stage::Launch, "explorer_offline_trust"};
  REQUIRE_ERROR(wfg::process_detail::ActivateBeforeDeadline(operations, {operations.Now() + std::chrono::milliseconds(10)}), wfg::ErrorCode::LaunchFailed);
  CHECK(operations.launch_calls == 0);
  operations.preparation = wfg::Unit{};
  operations.preparation_time = std::chrono::milliseconds(9);
  REQUIRE_OK(wfg::process_detail::ActivateBeforeDeadline(operations, {operations.Now() + std::chrono::milliseconds(10)}));
  CHECK(operations.launch_calls == 1);
}

TEST("process.bundled_debug_helper_requires_exact_trusted_file_identity") {
  CHECK(BundledDebugHelperFacts{}.Qualifies());
  for(int change=0;change<7;++change){BundledDebugHelperFacts f;
    switch(change){case 0:f.file.trusted_signature=false;break;case 1:f.file.signer.reset();break;
      case 2:f.file.signer=wfg::DependencySigner::Nvidia;break;case 3:f.file.sha256[0]^=std::byte{1};break;
      case 4:++f.file.length;break;case 5:f.file.version="6.2.9200.16385";break;case 6:f.file.sha256={};break;}
    CHECK(!f.Qualifies());
  }
}
TEST("process.bundled_debug_helper_is_bound_to_known_package_relative_path") {
  for(const auto& package:{"", "other", "CDPROJEKTSA.Witcher3GOTY_other"}){BundledDebugHelperFacts f;f.package=package;CHECK(!f.Qualifies());}
  for(const auto& version:{"", "5.0.3.0", "5.0.4.1"}){BundledDebugHelperFacts f;f.version=version;CHECK(!f.Qualifies());}
  for(const auto* path:{L"dbghelp.dll",L"bin\\Gaming.Desktop.x64\\dxgi.dll",L"other\\Gaming.Desktop.x64\\dbghelp.dll",L"bin\\Gaming.Desktop.x64\\..\\dbghelp.dll",L"C:\\bin\\Gaming.Desktop.x64\\dbghelp.dll",L"bin/Gaming.Desktop.x64/dbghelp.dll"}){BundledDebugHelperFacts f;f.path=path;CHECK(!f.Qualifies());}
  BundledDebugHelperFacts f;f.path=L"BIN\\GAMING.DESKTOP.X64\\DBGHELP.DLL";CHECK(f.Qualifies());
}
TEST("process.bundled_debug_helper_rejects_wrong_or_truncated_pe_shape") {
  for(int change=0;change<8;++change){BundledDebugHelperFacts f;
    switch(change){case 0:f.Put<std::uint16_t>(0,0);break;case 1:f.Put<std::uint32_t>(60,0xffffffff);break;
      case 2:f.Put<std::uint32_t>(128,0);break;case 3:f.Put<std::uint16_t>(132,IMAGE_FILE_MACHINE_AMD64);break;
      case 4:f.Put<std::uint32_t>(136,0);break;case 5:f.Put<std::uint16_t>(148,0);break;
      case 6:f.Put<std::uint16_t>(152,IMAGE_NT_OPTIONAL_HDR64_MAGIC);break;case 7:f.Put<std::uint32_t>(208,0x13c000);break;}
    CHECK(!f.Qualifies());
  }
  for(const auto size:{0u,63u,128u,151u,375u}){BundledDebugHelperFacts f;f.header.resize(size);CHECK(!f.Qualifies());}
}
TEST("process.bundled_debug_helper_does_not_relax_other_proxy_classification") {
  for(const auto* name:{"dbghelp.dll","dxgi.dll","d3d12.dll","version.dll"}){
    wfg::RuntimeInventory inventory;inventory.application_local_loaders.push_back({name,"",false,false});
    REQUIRE_ERROR(wfg::ClassifyLoaderConflicts(inventory,wfg::SupportedBuild()),wfg::ErrorCode::DllConflict);
    inventory.application_local_loaders[0].trusted_signature=true;
    REQUIRE_ERROR(wfg::ClassifyLoaderConflicts(inventory,wfg::SupportedBuild()),wfg::ErrorCode::DllConflict);
  }
}

TEST("process.local_iteration_failure_is_dependency_refusal_not_exception") {
  LocalDirectoryFixture directory;
  directory.fail_increment = true;
  std::optional<wfg::Result<std::vector<std::filesystem::path>>> outcome;
  bool escaped{};
  try { outcome = wfg::process_detail::EnumerateLocalFiles(directory); }
  catch (const std::filesystem::filesystem_error&) { escaped = true; }
  CHECK(!escaped);
  CHECK(outcome.has_value());
  const auto& enumeration_result = *outcome;
  REQUIRE_ERROR(enumeration_result, wfg::ErrorCode::AccessDenied);
  CHECK(wfg::Failure(enumeration_result).stage == wfg::Stage::dependencies);
  CHECK(wfg::Failure(enumeration_result).operation.view() == "local_loader_increment");
  CHECK(wfg::Failure(enumeration_result).native_code.has_value());
}

TEST("process.local_iteration_preserves_all_entries_and_bounds_count") {
  LocalDirectoryFixture directory;
  const auto files = REQUIRE_OK(wfg::process_detail::EnumerateLocalFiles(directory));
  CHECK(files == directory.paths);
  directory.index = 0;
  directory.paths.assign(4097, L"preserved.dll");
  REQUIRE_ERROR(wfg::process_detail::EnumerateLocalFiles(directory), wfg::ErrorCode::DllConflict);
}

TEST("process.only_missing_modules_are_retryable") {
  SupportedRuntimeFixture fixture;
  REQUIRE_OK(wfg::process_detail::ValidateStartupInventory(fixture.inventory));
  fixture.inventory.interposer.loaded_module.image.base = 0;
  REQUIRE_ERROR(wfg::process_detail::ValidateStartupInventory(fixture.inventory), wfg::ErrorCode::ModuleNotReady);
  fixture = SupportedRuntimeFixture{};
  fixture.inventory.trusted_system_modules.clear();
  fixture.inventory.interposer.trusted_nvidia_signer = false;
  REQUIRE_ERROR(wfg::process_detail::ValidateStartupInventory(fixture.inventory), wfg::ErrorCode::UntrustedInterposer);
}

TEST("process.native_module_names_are_case_insensitive") {
  SupportedRuntimeFixture fixture;
  fixture.inventory.trusted_system_modules[0].module.name = "D3D12.DLL";
  fixture.inventory.trusted_system_modules[1].module.name = "DXGI.DLL";
  REQUIRE_OK(wfg::process_detail::ValidateStartupInventory(fixture.inventory));
}

TEST("process.present_invalid_interposer_never_becomes_module_not_ready") {
  for (auto mutation : {Mutation::InterposerVersion, Mutation::InterposerLength, Mutation::InterposerHash, Mutation::Signer, Mutation::Correspondence}) {
    SupportedRuntimeFixture fixture;
    fixture.Mutate(mutation);
    REQUIRE_ERROR(wfg::process_detail::ValidateStartupInventory(fixture.inventory), wfg::ErrorCode::UntrustedInterposer);
    CHECK(fixture.memory.write_count() == 0);
  }
}

TEST("process.ota_ngx_role_uses_authenticated_exports_not_filename") {
  RuntimeMemory memory;
  constexpr wfg::Address base = 0x100000;
  memory.AddRegion(base, 0x1000);
  RuntimeFixtureDetail::Exports(memory, base, {"NVSDK_NGX_D3D12_Init", "NVSDK_NGX_D3D12_EvaluateFeature"});
  auto image = REQUIRE_OK(wfg::InspectPe(memory, base));
  wfg::process_detail::FileEvidence evidence{1234, {}, "1.2.3", true, wfg::DependencySigner::Nvidia};
  auto role = wfg::process_detail::IdentifyDependencyRole(memory, image, evidence, "ota_cached_plugin.dll");
  CHECK(role.value == wfg::DependencyRole::NgxPlugin);
  evidence.trusted_signature = false;
  role = wfg::process_detail::IdentifyDependencyRole(memory, image, evidence, "sl.ngx.dll");
  CHECK(!role.value);
  evidence.trusted_signature = true;
  RuntimeFixtureDetail::Exports(memory, base, {"unrelated"});
  role = wfg::process_detail::IdentifyDependencyRole(memory, image, evidence, "sl.dlss_g.dll");
  CHECK(!role.value);
  RuntimeFixtureDetail::Exports(memory, base, {"slGetPluginFunction"});
  role = wfg::process_detail::IdentifyDependencyRole(memory, image, evidence, "arbitrary_plugin.dll");
  CHECK(role.value == wfg::DependencyRole::StreamlinePlugin);
  evidence.signer = wfg::DependencySigner::Microsoft;
  role = wfg::process_detail::IdentifyDependencyRole(memory, image, evidence, "arbitrary_plugin.dll");
  CHECK(!role.value);
}

TEST("process.native_backing_identity_rejects_other_file") {
  wchar_t system[MAX_PATH]{};
  CHECK(GetSystemDirectoryW(system, MAX_PATH));
  const auto module = GetModuleHandleW(L"kernel32.dll");
  CHECK(module != nullptr);
  class LocalMemory final : public wfg::IMemory {
  public:
    wfg::Result<wfg::Bytes> Read(wfg::Address address, std::size_t size) const override {
      wfg::Bytes bytes(size);
      const auto read = wfg::process_detail::ReadIntoHandle(GetCurrentProcess(), address, bytes);
      if (!wfg::IsOk(read)) return wfg::Failure(read);
      return bytes;
    }
    wfg::Result<wfg::MemoryRegion> Query(wfg::Address address) const override {
      MEMORY_BASIC_INFORMATION region{};
      if (!VirtualQuery(reinterpret_cast<void*>(address), &region, sizeof(region))) return wfg::Error{wfg::ErrorCode::UnreadableMemory, wfg::Stage::Inspect, "fixture_query"};
      return wfg::MemoryRegion{reinterpret_cast<wfg::Address>(region.BaseAddress), region.RegionSize, region.State == MEM_COMMIT, true, false, true, region.Protect};
    }
  } memory;
  const auto mismatch = wfg::process_detail::VerifyBackingImage(GetCurrentProcess(), reinterpret_cast<wfg::Address>(module), std::wstring(system) + L"\\user32.dll", memory);
  CHECK(!wfg::IsOk(mismatch));
  if (wfg::Failure(mismatch).code != wfg::ErrorCode::UntrustedInterposer) throw std::runtime_error(std::string(wfg::Failure(mismatch).operation.view()) + ":" + std::to_string(wfg::Failure(mismatch).native_code.value_or(0)));
  REQUIRE_OK(wfg::process_detail::VerifyBackingImage(GetCurrentProcess(), reinterpret_cast<wfg::Address>(module), std::wstring(system) + L"\\kernel32.dll", memory));
}

TEST("process.owned_observation_duplicate_keeps_identity_and_reduces_rights") {
  FILETIME created{},exited{},kernel{},user{};
  CHECK(GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user));
  const auto creation=(static_cast<std::uint64_t>(created.dwHighDateTime)<<32U)|created.dwLowDateTime;
  auto owner=wfg::VerifiedIdentityTestFactory::Owned(GetCurrentProcess(),[](void*) noexcept {},GetCurrentProcessId(),creation);
  auto duplicated=wfg::DuplicateObservedProcess(owner);
  CHECK(wfg::IsOk(duplicated));
  auto duplicate=std::move(std::get<wfg::ObservedProcess>(duplicated));
  CHECK(duplicate.pid()==owner.pid());CHECK(duplicate.creation_time()==owner.creation_time());
  CHECK(GetProcessId(duplicate.native_handle())==GetCurrentProcessId());
  int value=1,replacement=2;SIZE_T written{};
  CHECK(!WriteProcessMemory(duplicate.native_handle(),&value,&replacement,sizeof(value),&written));
  CHECK(value==1);
  auto stale=wfg::VerifiedIdentityTestFactory::Owned(GetCurrentProcess(),[](void*) noexcept {},GetCurrentProcessId(),creation+1);
  REQUIRE_ERROR(wfg::DuplicateObservedProcess(stale),wfg::ErrorCode::IdentityChanged);
}

TEST("process.startup_readiness_skips_full_qualification_until_present") {
  SupportedRuntimeFixture f;unsigned inspections{};
  const auto inspect=[&]() -> wfg::Result<wfg::RuntimeInventory> {++inspections;return f.inventory;};
  const auto clean=[]() -> wfg::Result<wfg::Unit> {return wfg::Unit{};};
  std::vector<std::string> names{"witcher3.exe","d3d12.dll","dxgi.dll"};
  const auto missing=wfg::process_detail::CompleteStartupInventory(names,clean,inspect);
  REQUIRE_ERROR(missing,wfg::ErrorCode::ModuleNotReady);CHECK(inspections==0);
  names.push_back("SL.INTERPOSER.DLL");const auto ready=wfg::process_detail::CompleteStartupInventory(names,clean,inspect);
  CHECK(wfg::IsOk(ready));CHECK(inspections==1);
}
TEST("process.startup_readiness_reports_conflicts_before_the_presence_gate") {
  // The cheap presence check used to run first and short-circuit everything, so a user whose
  // application-local d3d12.dll or dxgi.dll proxy PREVENTS sl.interposer.dll from loading hit
  // the gate, no conflict guard ran, application_local_loaders stayed empty, and
  // ClassifyLoaderConflicts then affirmatively reported no conflict. The launcher retried in
  // silence to the deadline and refused an opaque TimedOut -- for precisely the population most
  // likely to hit it, and the spec's unsupported_or_conflicting_refusal row requires a named
  // refusal before any write. The conflict pre-pass must therefore run even when the gate will
  // refuse, while still being cheap enough to repeat on every retry pass.
  SupportedRuntimeFixture f;unsigned inspections{},prechecks{};
  const auto inspect=[&]() -> wfg::Result<wfg::RuntimeInventory> {++inspections;return f.inventory;};
  const auto conflicted=[&]() -> wfg::Result<wfg::Unit> {
    ++prechecks;
    return wfg::Error{wfg::ErrorCode::DllConflict,wfg::Stage::dependencies,"native_module_location"};
  };
  // sl.interposer.dll absent: the presence gate alone would have said startup_modules_missing.
  std::vector<std::string> names{"witcher3.exe","d3d12.dll","dxgi.dll"};
  const auto refused=wfg::process_detail::CompleteStartupInventory(names,conflicted,inspect);
  REQUIRE_ERROR(refused,wfg::ErrorCode::DllConflict);
  CHECK(wfg::Failure(refused).operation.view()=="native_module_location");
  CHECK(prechecks==1&&inspections==0);
  // With no conflict the gate still governs, and the retryable refusal is unchanged.
  const auto clean=[&]() -> wfg::Result<wfg::Unit> {++prechecks;return wfg::Unit{};};
  const auto retry=wfg::process_detail::CompleteStartupInventory(names,clean,inspect);
  REQUIRE_ERROR(retry,wfg::ErrorCode::ModuleNotReady);
  CHECK(wfg::Failure(retry).operation.view()=="startup_modules_missing");
  CHECK(prechecks==2&&inspections==0);
}
TEST("process.startup_readiness_requires_each_provider_and_preserves_failure") {
  const std::array<std::string,3> all{"d3d12.dll","dxgi.dll","sl.interposer.dll"};
  unsigned inspections{};const auto inspect=[&]() -> wfg::Result<wfg::RuntimeInventory> {++inspections;return wfg::Error{wfg::ErrorCode::UntrustedInterposer,wfg::Stage::dependencies,"fixture_provider_untrusted"};};
  const auto clean=[]() -> wfg::Result<wfg::Unit> {return wfg::Unit{};};
  for(std::size_t omit=0;omit<all.size();++omit){std::vector<std::string> names;for(std::size_t i=0;i<all.size();++i)if(i!=omit)names.push_back(all[i]);const auto missing=wfg::process_detail::CompleteStartupInventory(names,clean,inspect);REQUIRE_ERROR(missing,wfg::ErrorCode::ModuleNotReady);}
  CHECK(inspections==0);const auto untrusted=wfg::process_detail::CompleteStartupInventory(all,clean,inspect);REQUIRE_ERROR(untrusted,wfg::ErrorCode::UntrustedInterposer);CHECK(inspections==1);
}

TEST("process.manual_hint_accepts_only_registered_file_or_useful_folder") {
  PackageDocuments f;
  REQUIRE_OK(f.Resolve());
  const auto containing = f.root / L"bin" / L"Gaming.Desktop.x64";
  std::filesystem::create_directories(containing);
  const auto executable = containing / L"witcher3.exe";
  {std::ofstream file(containing/L"other.exe"); file << "fixture";}
  {std::ofstream file(executable); file << "fixture";}
  for (const auto& hint : {f.root, f.root/L"bin", containing, executable}) {
    const auto result = wfg::process_detail::ResolveManualExecutableHint(f.root.wstring(), f.identity, hint.wstring());
    CHECK(wfg::IsOk(result));
    if (wfg::IsOk(result)) CHECK(wfg::Value(result).ends_with(L"\\bin\\Gaming.Desktop.x64\\witcher3.exe"));
  }
  PackageDocuments unrelated;
  REQUIRE_OK(unrelated.Resolve());
  std::filesystem::create_directories(unrelated.root / L"bin" / L"Gaming.Desktop.x64");
  {std::ofstream file(unrelated.root/L"bin"/L"Gaming.Desktop.x64"/L"witcher3.exe"); file << "fixture";}
  CHECK(!wfg::IsOk(wfg::process_detail::ResolveManualExecutableHint(f.root.wstring(), f.identity, unrelated.root.wstring())));
  std::filesystem::create_directory(f.root/L"unrelated");
  for (const auto& hint : {f.root/L"unrelated", containing/L"missing.exe", containing/L"other.exe"})
    CHECK(!wfg::IsOk(wfg::process_detail::ResolveManualExecutableHint(f.root.wstring(), f.identity, hint.wstring())));
}
TEST("process.manual_hint_preserves_manifest_and_registration_guards") {
  PackageDocuments f;
  REQUIRE_OK(f.Resolve());
  std::filesystem::create_directories(f.root/L"bin"/L"Gaming.Desktop.x64");
  {std::ofstream file(f.root/L"bin"/L"Gaming.Desktop.x64"/L"witcher3.exe"); file << "fixture";}
  f.identity.version = L"5.0.5.0";
  CHECK(!wfg::IsOk(wfg::process_detail::ResolveManualExecutableHint(f.root.wstring(), f.identity, f.root.wstring())));
  f.identity.version = L"5.0.4.0";
  PackageDocuments::Replace(f.manifest, "GameLaunchHelper.exe", "evil.exe");
  REQUIRE_ERROR(f.Resolve(), wfg::ErrorCode::UnsupportedBuild);
  CHECK(!wfg::IsOk(wfg::process_detail::ResolveManualExecutableHint(f.root.wstring(), f.identity, f.root.wstring())));
}
TEST("process.manual_prompt_refuses_redirected_input_without_reading") {
  int reads{};
  const auto result = wfg::process_detail::RequestManualGamePath(false, [&]() -> wfg::Result<std::wstring> {++reads; return std::wstring{L"C:\\Game"};});
  CHECK(!wfg::IsOk(result)); CHECK(reads == 0);
  if (!wfg::IsOk(result)) CHECK(wfg::Failure(result).operation.view() == "manual_game_path_required");
}
TEST("process.manual_prompt_empty_and_cancelled_input_refuse") {
  CHECK(!wfg::IsOk(wfg::process_detail::RequestManualGamePath(true, []() -> wfg::Result<std::wstring> {return std::wstring{};})));
  const auto result = wfg::process_detail::RequestManualGamePath(true, []() -> wfg::Result<std::wstring> {return wfg::Error{wfg::ErrorCode::TimedOut,wfg::Stage::package_discovery,"cancelled"};});
  CHECK(!wfg::IsOk(result)); CHECK(wfg::Failure(result).code == wfg::ErrorCode::TimedOut);
}

TEST("process.manual_hint_unicode_root_and_missing_executable_are_checked_by_handles") {
  PackageDocuments f;
  auto unicode_root = f.root; unicode_root += L"\u732b";
  std::filesystem::rename(f.root, unicode_root); f.root = unicode_root;
  REQUIRE_OK(f.Resolve());
  CHECK(!wfg::IsOk(wfg::process_detail::ResolveManualExecutableHint(f.root.wstring(), f.identity, f.root.wstring())));
  std::filesystem::create_directories(f.root/L"bin"/L"Gaming.Desktop.x64");
  {std::ofstream file(f.root/L"bin"/L"Gaming.Desktop.x64"/L"witcher3.exe"); file << "fixture";}
  CHECK(wfg::IsOk(wfg::process_detail::ResolveManualExecutableHint(f.root.wstring(), f.identity, f.root.wstring())));
}

TEST("process.manual_prompt_gate_only_accepts_resolvable_missing_executable_documents") {
  using namespace wfg;
  CHECK(process_detail::CanRequestManualGamePath({ErrorCode::AccessDenied,Stage::process_identity,"executable_attributes",2U}));
  CHECK(process_detail::CanRequestManualGamePath({ErrorCode::AccessDenied,Stage::package_discovery,"package_document",3U}));
  for (const auto& error : {Error{ErrorCode::UnsupportedBuild,Stage::package_discovery,"manifest_identity",2U},
      Error{ErrorCode::AccessDenied,Stage::package_discovery,"package_document",5U},
      Error{ErrorCode::AccessDenied,Stage::package_discovery,"package_path",3U},
      Error{ErrorCode::AccessDenied,Stage::package_discovery,"package_not_registered",2U},
      Error{ErrorCode::IdentityChanged,Stage::process_identity,"executable_attributes",2U}})
    CHECK(!process_detail::CanRequestManualGamePath(error));
}
