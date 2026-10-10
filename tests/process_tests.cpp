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
  CHECK(fixture.backend.attach_count == 0);
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
  CHECK(fixture.backend.attach_count == 0);
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
