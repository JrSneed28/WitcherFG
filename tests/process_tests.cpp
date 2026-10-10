#include "support.hpp"
#include "wfg/windows_process.hpp"
#include "fixtures/process_identity.hpp"
#include "fixtures/runtime_images.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <filesystem>

namespace {
std::unique_ptr<wfg::ProcessObserver> Observer(wfg::Result<std::unique_ptr<wfg::ProcessObserver>> result) {
  CHECK(wfg::IsOk(result));
  return std::move(std::get<std::unique_ptr<wfg::ProcessObserver>>(result));
}
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
