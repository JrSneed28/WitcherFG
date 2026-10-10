#include "wfg/windows_process.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <appmodel.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <sddl.h>
#include <shlwapi.h>
#include <xmllite.h>
#include <wintrust.h>
#include <softpub.h>
#include <mscat.h>
#include <bcrypt.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <limits>
#include <thread>

namespace wfg {
namespace {
constexpr DWORD ReadRights = PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | SYNCHRONIZE;
constexpr DWORD OwnedRights = ReadRights | PROCESS_TERMINATE | PROCESS_VM_WRITE | PROCESS_VM_OPERATION;
constexpr wchar_t Family[] = L"CDPROJEKTSA.Witcher3GOTY_0xkrdv0gjmk28";
constexpr wchar_t Application[] = L"bin.Gaming.Desktop.x64.witcher3";
constexpr wchar_t PackageName[] = L"CDPROJEKTSA.Witcher3GOTY";
constexpr std::size_t MaxPath = 32768;

Error Fail(ErrorCode code, const char* operation, Stage stage = Stage::process_identity, DWORD native = 0) {
  return {code, stage, operation, native == 0 ? std::nullopt : std::optional<std::uint32_t>{native}};
}
void CloseNative(void* handle) noexcept { CloseHandle(handle); }
using Pin = std::shared_ptr<void>;
Pin Hold(HANDLE handle) { return Pin(handle, CloseNative); }
bool Equal(std::wstring_view left, std::wstring_view right) {
  return left.size() <= INT_MAX && right.size() <= INT_MAX && CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}
bool EqualAscii(std::string_view left, std::string_view right) {
  if (left.size() != right.size()) return false;
  const auto lower = [](char character) { return character >= 'A' && character <= 'Z' ? static_cast<char>(character + ('a' - 'A')) : character; };
  for (std::size_t index = 0; index < left.size(); ++index) if (lower(left[index]) != lower(right[index])) return false;
  return true;
}
std::string Safe(std::wstring_view value) {
  if (value.size() > 256) return {};
  std::string output;
  for (auto character : value) {
    if (!((character >= L'a' && character <= L'z') || (character >= L'A' && character <= L'Z') || (character >= L'0' && character <= L'9') || character == L'.' || character == L'_' || character == L'-')) return {};
    output.push_back(static_cast<char>(character));
  }
  return output;
}
std::uint64_t Time(FILETIME value) { return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime; }
Result<std::wstring> CanonicalExecutable(const std::wstring& path) {
  auto file = Hold(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (file.get() == INVALID_HANDLE_VALUE) return Fail(ErrorCode::AccessDenied, "executable_attributes", Stage::process_identity, GetLastError());
  FILE_STANDARD_INFO standard{};
  if (!GetFileInformationByHandleEx(file.get(), FileStandardInfo, &standard, sizeof(standard)) || standard.Directory) return Fail(ErrorCode::IdentityChanged, "executable_file_identity");
  std::wstring normalized(MaxPath, L'\0');
  const auto length = GetFinalPathNameByHandleW(file.get(), normalized.data(), static_cast<DWORD>(normalized.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (length == 0 || length >= normalized.size()) return Fail(ErrorCode::AccessDenied, "executable_canonical_path", Stage::process_identity, GetLastError());
  normalized.resize(length);
  return normalized;
}
Result<std::uint64_t> Creation(HANDLE handle) {
  FILETIME created{}, exit{}, kernel{}, user{};
  if (!GetProcessTimes(handle, &created, &exit, &kernel, &user)) return Fail(ErrorCode::AccessDenied, "process_times", Stage::process_identity, GetLastError());
  return Time(created);
}
Result<std::wstring> User(HANDLE process) {
  HANDLE raw{};
  if (!OpenProcessToken(process, TOKEN_QUERY, &raw)) return Fail(ErrorCode::AccessDenied, "process_token", Stage::process_identity, GetLastError());
  auto token = Hold(raw);
  DWORD size{};
  GetTokenInformation(raw, TokenUser, nullptr, 0, &size);
  if (size == 0 || size > 65536) return Fail(ErrorCode::IdentityChanged, "token_size");
  std::vector<std::byte> bytes(size);
  if (!GetTokenInformation(raw, TokenUser, bytes.data(), size, &size)) return Fail(ErrorCode::AccessDenied, "token_user");
  auto* user = reinterpret_cast<TOKEN_USER*>(bytes.data());
  LPWSTR sid{};
  if (!IsValidSid(user->User.Sid) || !ConvertSidToStringSidW(user->User.Sid, &sid)) return Fail(ErrorCode::IdentityChanged, "token_sid");
  std::wstring result(sid);
  LocalFree(sid);
  return result;
}
Result<std::wstring> ProcessPackage(HANDLE process) {
  UINT32 size{};
  auto result = GetPackageFullName(process, &size, nullptr);
  if (result != ERROR_INSUFFICIENT_BUFFER || size == 0 || size > MaxPath) return Fail(ErrorCode::IdentityChanged, "process_package", Stage::process_identity, result);
  std::wstring name(size, L'\0');
  result = GetPackageFullName(process, &size, name.data());
  if (result != ERROR_SUCCESS || size < 2 || size > name.size()) return Fail(ErrorCode::IdentityChanged, "process_package", Stage::process_identity, result);
  name.resize(size - 1);
  return name;
}
Result<std::wstring> Executable(HANDLE process) {
  std::wstring path(MaxPath, L'\0');
  DWORD size = static_cast<DWORD>(path.size());
  if (!QueryFullProcessImageNameW(process, 0, path.data(), &size) || size == 0 || size >= MaxPath) return Fail(ErrorCode::AccessDenied, "executable_path", Stage::process_identity, GetLastError());
  path.resize(size);
  return CanonicalExecutable(path);
}
Result<Candidate> InspectCandidate(HANDLE process) {
  Candidate result;
  result.pid = result.pinned_pid = GetProcessId(process);
  const auto creation = Creation(process);
  const auto user = User(process);
  const auto package = ProcessPackage(process);
  const auto executable = Executable(process);
  if (!IsOk(creation)) return Failure(creation);
  if (!IsOk(user)) return Failure(user);
  if (!IsOk(package)) return Failure(package);
  if (!IsOk(executable)) return Failure(executable);
  DWORD session{};
  if (result.pid == 0 || !ProcessIdToSessionId(result.pid, &session) || WaitForSingleObject(process, 0) != WAIT_TIMEOUT) return Fail(ErrorCode::IdentityChanged, "process_alive");
  result.session = session;
  result.creation_time = result.pinned_creation_time = Value(creation);
  result.user = Value(user);
  result.package = Value(package);
  result.executable = Value(executable);
  return result;
}
Result<std::wstring> RegisteredFullName() {
  UINT32 count{}, buffer_size{};
  auto status = FindPackagesByPackageFamily(Family, PACKAGE_FILTER_HEAD, &count, nullptr, &buffer_size, nullptr, nullptr);
  if (status != ERROR_INSUFFICIENT_BUFFER || count == 0 || count > 32 || buffer_size > 65536) return Fail(ErrorCode::NoMatchingProcess, "package_registration", Stage::Discover, status);
  std::vector<PWSTR> names(count);
  std::vector<wchar_t> buffer(buffer_size);
  std::vector<UINT32> properties(count);
  status = FindPackagesByPackageFamily(Family, PACKAGE_FILTER_HEAD, &count, names.data(), &buffer_size, buffer.data(), properties.data());
  if (status != ERROR_SUCCESS) return Fail(ErrorCode::IdentityChanged, "package_registration", Stage::Discover, status);
  if (count != 1) return Fail(ErrorCode::AmbiguousProcess, "package_registration", Stage::Discover);
  return std::wstring(names[0]);
}
Result<std::wstring> RegisteredPath(const std::wstring& full_name) {
  UINT32 size{};
  auto status = GetPackagePathByFullName(full_name.c_str(), &size, nullptr);
  if (status != ERROR_INSUFFICIENT_BUFFER || size == 0 || size > MaxPath) return Fail(ErrorCode::IdentityChanged, "package_location", Stage::Discover, status);
  std::wstring path(size, L'\0');
  status = GetPackagePathByFullName(full_name.c_str(), &size, path.data());
  if (status != ERROR_SUCCESS || size < 2 || size > path.size()) return Fail(ErrorCode::IdentityChanged, "package_location", Stage::Discover, status);
  path.resize(size - 1);
  return path;
}
Result<Unit> RegisteredApplication(const std::wstring& full_name) {
  PACKAGE_INFO_REFERENCE reference{};
  const auto opened = OpenPackageInfoByFullName(full_name.c_str(), 0, &reference);
  if (opened != ERROR_SUCCESS) return Fail(ErrorCode::IdentityChanged, "package_application_registration", Stage::Discover, opened);
  struct Close { PACKAGE_INFO_REFERENCE reference; ~Close() { ClosePackageInfo(reference); } } close{reference};
  UINT32 size{}, count{};
  if (GetPackageApplicationIds(reference, &size, nullptr, &count) != ERROR_INSUFFICIENT_BUFFER || count == 0 || count > 128 || size > 65536 || size < count * sizeof(PCWSTR)) return Fail(ErrorCode::UnsupportedBuild, "registered_application_ids", Stage::Discover);
  std::vector<BYTE> bytes(size);
  if (GetPackageApplicationIds(reference, &size, bytes.data(), &count) != ERROR_SUCCESS || count > 128 || size < count * sizeof(PCWSTR)) return Fail(ErrorCode::IdentityChanged, "registered_application_ids", Stage::Discover);
  const auto* ids = reinterpret_cast<PCWSTR*>(bytes.data());
  const auto expected = std::wstring(Family) + L"!" + Application;
  unsigned matches{};
  for (std::size_t index = 0; index < count; ++index) if (std::wstring_view(ids[index]) == expected) ++matches;
  if (matches != 1) return Fail(ErrorCode::UnsupportedBuild, "registered_aumid", Stage::Discover);
  return Unit{};
}
Result<std::wstring> ManifestExecutable(const std::wstring& root) {
  const auto manifest = root + L"\\AppxManifest.xml";
  auto file = Hold(CreateFileW(manifest.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  LARGE_INTEGER length{};
  if (file.get() == INVALID_HANDLE_VALUE || !GetFileSizeEx(file.get(), &length)) return Fail(ErrorCode::AccessDenied, "package_manifest", Stage::Discover, GetLastError());
  if (length.QuadPart <= 0 || length.QuadPart > 1024 * 1024) return Fail(ErrorCode::UnsupportedBuild, "manifest_size", Stage::Discover);
  Microsoft::WRL::ComPtr<IStream> stream;
  if (FAILED(SHCreateStreamOnFileEx(manifest.c_str(), STGM_READ | STGM_SHARE_DENY_WRITE, FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, &stream))) return Fail(ErrorCode::AccessDenied, "manifest_stream", Stage::Discover);
  Microsoft::WRL::ComPtr<IXmlReader> reader;
  if (FAILED(CreateXmlReader(__uuidof(IXmlReader), reinterpret_cast<void**>(reader.GetAddressOf()), nullptr)) || FAILED(reader->SetProperty(XmlReaderProperty_DtdProcessing, DtdProcessing_Prohibit)) || FAILED(reader->SetProperty(XmlReaderProperty_MaxElementDepth, 32)) || FAILED(reader->SetInput(stream.Get()))) return Fail(ErrorCode::UnsupportedBuild, "manifest_parser", Stage::Discover);
  std::wstring executable;
  XmlNodeType type{};
  HRESULT status{};
  unsigned nodes{};
  while ((status = reader->Read(&type)) == S_OK) {
    if (++nodes > 100000) return Fail(ErrorCode::UnsupportedBuild, "manifest_nodes", Stage::Discover);
    if (type != XmlNodeType_Element) continue;
    const wchar_t* local{};
    const wchar_t* uri{};
    if (FAILED(reader->GetLocalName(&local, nullptr)) || FAILED(reader->GetNamespaceUri(&uri, nullptr))) return Fail(ErrorCode::UnsupportedBuild, "manifest_element", Stage::Discover);
    if (std::wstring_view(local) != L"Application" || std::wstring_view(uri) != L"http://schemas.microsoft.com/appx/manifest/foundation/windows10") continue;
    if (reader->MoveToAttributeByName(L"Id", nullptr) != S_OK) continue;
    const wchar_t* id{};
    if (FAILED(reader->GetValue(&id, nullptr)) || std::wstring_view(id) != Application) { reader->MoveToElement(); continue; }
    reader->MoveToElement();
    if (!executable.empty() || reader->MoveToAttributeByName(L"Executable", nullptr) != S_OK) return Fail(ErrorCode::UnsupportedBuild, "manifest_executable", Stage::Discover);
    const wchar_t* value{};
    if (FAILED(reader->GetValue(&value, nullptr))) return Fail(ErrorCode::UnsupportedBuild, "manifest_executable", Stage::Discover);
    executable = value;
    reader->MoveToElement();
  }
  if (status != S_FALSE || executable.empty() || executable.size() >= MaxPath || executable.front() == L'\\' || executable.find(L':') != executable.npos || executable.find(L'/') != executable.npos) return Fail(ErrorCode::UnsupportedBuild, "manifest_executable", Stage::Discover);
  for (const auto& part : std::filesystem::path(executable)) if (part == L".." || part == L".") return Fail(ErrorCode::UnsupportedBuild, "manifest_traversal", Stage::Discover);
  if (!Equal(std::filesystem::path(executable).filename().wstring(), L"witcher3.exe")) return Fail(ErrorCode::UnsupportedBuild, "manifest_target", Stage::Discover);
  return root + L"\\" + executable;
}

class ReadMemory : public IMemory {
public:
  explicit ReadMemory(Pin handle) : handle_(std::move(handle)) {}
  Result<Bytes> Read(Address address, std::size_t size) const override {
    if (size > 128 * 1024 * 1024) return Fail(ErrorCode::UnreadableMemory, "read_limit");
    Bytes bytes(size);
    const auto read = ReadExact(address, bytes);
    if (!IsOk(read)) return Failure(read);
    return bytes;
  }
  Result<MemoryRegion> Query(Address address) const override {
    MEMORY_BASIC_INFORMATION information{};
    if (VirtualQueryEx(handle_.get(), reinterpret_cast<LPCVOID>(address), &information, sizeof(information)) != sizeof(information)) return Fail(ErrorCode::UnreadableMemory, "query_memory", Stage::Inspect, GetLastError());
    const DWORD access = information.Protect & 0xff;
    const bool guarded = (information.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0;
    return MemoryRegion{reinterpret_cast<Address>(information.BaseAddress), information.RegionSize, information.State == MEM_COMMIT,
      !guarded && (access == PAGE_READONLY || access == PAGE_READWRITE || access == PAGE_WRITECOPY || access == PAGE_EXECUTE_READ || access == PAGE_EXECUTE_READWRITE || access == PAGE_EXECUTE_WRITECOPY),
      !guarded && (access == PAGE_READWRITE || access == PAGE_WRITECOPY || access == PAGE_EXECUTE_READWRITE || access == PAGE_EXECUTE_WRITECOPY),
      !guarded && (access == PAGE_EXECUTE || access == PAGE_EXECUTE_READ || access == PAGE_EXECUTE_READWRITE || access == PAGE_EXECUTE_WRITECOPY), information.Protect};
  }
  Result<Unit> ReadExact(Address address, std::span<std::byte> bytes) const {
    return process_detail::ReadIntoHandle(handle_.get(), address, bytes);
  }
protected:
  Pin handle_;
};
class WritableMemory final : public IWritableMemory {
public:
  explicit WritableMemory(Pin handle) : reader_(handle), handle_(std::move(handle)) {}
  Result<Bytes> Read(Address address, std::size_t size) const override { return reader_.Read(address, size); }
  Result<MemoryRegion> Query(Address address) const override { return reader_.Query(address); }
  Result<Unit> ReadInto(Address address, std::span<std::byte> bytes) const override { return reader_.ReadExact(address, bytes); }
  Result<Unit> Write(Address address, std::span<const std::byte> bytes) override {
    if (bytes.size() > std::numeric_limits<Address>::max() - address) return Fail(ErrorCode::MutationFailure, "write_overflow", Stage::Transaction);
    SIZE_T received{};
    if (!bytes.empty() && (!WriteProcessMemory(handle_.get(), reinterpret_cast<LPVOID>(address), bytes.data(), bytes.size(), &received) || received != bytes.size())) return Fail(ErrorCode::MutationFailure, "write_exact", Stage::Transaction, GetLastError());
    return Unit{};
  }
  Result<std::uint32_t> Protect(Address address, std::size_t size, std::uint32_t protection) override {
    DWORD previous{};
    if (size == 0 || size > std::numeric_limits<Address>::max() - address || !VirtualProtectEx(handle_.get(), reinterpret_cast<LPVOID>(address), size, protection, &previous)) return Fail(ErrorCode::MutationFailure, "protect_memory", Stage::Transaction, GetLastError());
    return previous;
  }
private:
  ReadMemory reader_;
  Pin handle_;
};
Result<Pin> Duplicate(HANDLE handle, DWORD rights) {
  HANDLE duplicate{};
  if (!DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(), &duplicate, rights, FALSE, 0)) return Fail(ErrorCode::AccessDenied, "duplicate_process", Stage::process_identity, GetLastError());
  return Hold(duplicate);
}
}

struct PackageIdentity::State {
  std::wstring full_name;
  std::wstring root;
  std::wstring executable;
  std::wstring user;
  std::wstring version;
  std::uint32_t session{};
  std::weak_ptr<IProcessBackend> observer;
};
std::string PackageIdentity::name() const { return Safe(PackageName); }
std::string PackageIdentity::version() const { return Safe(state_->version); }
std::string PackageIdentity::family() const { return Safe(Family); }
std::string PackageIdentity::application_id() const { return Safe(Application); }
std::uint16_t PackageIdentity::architecture() const { return IMAGE_FILE_MACHINE_AMD64; }

Result<Candidate> SelectCandidate(std::span<const Candidate> candidates, const LaunchIdentity& launch) {
  const Candidate* selected{};
  for (const auto& candidate : candidates) {
    if (candidate.session != launch.session || candidate.user != launch.user || candidate.package != launch.package || !Equal(candidate.executable, launch.executable)) continue;
    if (candidate.pid == 0 || candidate.pid != candidate.pinned_pid || candidate.creation_time != candidate.pinned_creation_time) return Fail(ErrorCode::IdentityChanged, "pinned_identity");
    if (launch.require_fresh && candidate.creation_time <= launch.earliest_creation_time) return Fail(ErrorCode::AlreadyRunning, "preexisting_process");
    if (selected != nullptr) return Fail(ErrorCode::AmbiguousProcess, "multiple_candidates");
    selected = &candidate;
  }
  if (selected == nullptr) return Fail(ErrorCode::NoMatchingProcess, "no_candidate");
  return *selected;
}

Result<std::unique_ptr<ProcessObserver>> InstallProcessObserver(LaunchIdentity launch, IProcessBackend& backend, Deadline deadline) {
  const auto now = backend.Now();
  deadline.value = std::min(deadline.value, now + std::chrono::seconds(30));
  if (now >= deadline.value) return Fail(ErrorCode::TimedOut, "observer_deadline");
  const auto install = backend.Install();
  if (!IsOk(install)) return Failure(install);
  const auto registration = backend.CheckRegistration();
  if (!IsOk(registration)) return Failure(registration);
  const auto existing = backend.Enumerate();
  if (!IsOk(existing)) return Failure(existing);
  const auto selection = SelectCandidate(Value(existing), launch);
  if (IsOk(selection) || Failure(selection).code != ErrorCode::NoMatchingProcess) return Fail(IsOk(selection) ? ErrorCode::AlreadyRunning : Failure(selection).code, "observer_existing");
  if (backend.Now() >= deadline.value) return Fail(ErrorCode::TimedOut, "observer_deadline");
  return std::unique_ptr<ProcessObserver>(new ProcessObserver(std::move(launch), backend, deadline));
}
Result<Unit> ProcessObserver::Activate() {
  if (activated_ || consumed_) return Fail(ErrorCode::LaunchFailed, "activation_state", Stage::Launch);
  if (backend_->Now() >= deadline_.value) return Fail(ErrorCode::TimedOut, "activation_deadline", Stage::Launch);
  const auto registration = backend_->CheckRegistration();
  if (!IsOk(registration)) return Failure(registration);
  const auto candidates = backend_->Enumerate();
  if (!IsOk(candidates)) return Failure(candidates);
  auto diagnostic_launch = launch_;
  diagnostic_launch.require_fresh = false;
  const auto existing = SelectCandidate(Value(candidates), diagnostic_launch);
  if (IsOk(existing) || Failure(existing).code != ErrorCode::NoMatchingProcess) return Fail(IsOk(existing) ? ErrorCode::AlreadyRunning : Failure(existing).code, "activation_existing", Stage::Launch);
  if (backend_->Now() >= deadline_.value) return Fail(ErrorCode::TimedOut, "activation_deadline", Stage::Launch);
  const auto result = backend_->Activate();
  if (IsOk(result)) activated_ = true;
  return result;
}
Result<Candidate> ProcessObserver::WaitForCandidate() {
  if (consumed_) return Fail(ErrorCode::IdentityChanged, "observer_consumed");
  if (!activated_ && !backend_->ActivationRequested()) return Fail(ErrorCode::LaunchFailed, "activation_required");
  while (backend_->Now() < deadline_.value) {
    const auto registration = backend_->CheckRegistration();
    if (!IsOk(registration)) return Failure(registration);
    const auto candidates = backend_->Enumerate();
    if (!IsOk(candidates)) return Failure(candidates);
    const auto selected = SelectCandidate(Value(candidates), launch_);
    if (backend_->Now() >= deadline_.value) return Fail(ErrorCode::TimedOut, "discovery_deadline");
    if (IsOk(selected)) { consumed_ = true; return selected; }
    if (Failure(selected).code != ErrorCode::NoMatchingProcess) return Failure(selected);
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline_.value - backend_->Now());
    if (remaining <= std::chrono::milliseconds::zero()) break;
    backend_->Sleep(std::min(remaining, std::chrono::milliseconds(20)));
  }
  return Fail(ErrorCode::TimedOut, "discovery_deadline");
}

Result<PackageIdentity> DiscoverPackage() {
  const auto full_name = RegisteredFullName();
  if (!IsOk(full_name)) return Failure(full_name);
  UINT32 size{};
  if (PackageIdFromFullName(Value(full_name).c_str(), 0, &size, nullptr) != ERROR_INSUFFICIENT_BUFFER || size > 65536) return Fail(ErrorCode::UnsupportedBuild, "package_metadata", Stage::Discover);
  std::vector<std::byte> bytes(size);
  if (PackageIdFromFullName(Value(full_name).c_str(), 0, &size, reinterpret_cast<BYTE*>(bytes.data())) != ERROR_SUCCESS) return Fail(ErrorCode::UnsupportedBuild, "package_metadata", Stage::Discover);
  const auto* id = reinterpret_cast<PACKAGE_ID*>(bytes.data());
  if (std::wstring_view(id->name) != PackageName || id->processorArchitecture != PROCESSOR_ARCHITECTURE_AMD64) return Fail(ErrorCode::UnsupportedBuild, "package_architecture", Stage::Discover);
  const auto root = RegisteredPath(Value(full_name));
  if (!IsOk(root)) return Failure(root);
  const auto application = RegisteredApplication(Value(full_name));
  if (!IsOk(application)) return Failure(application);
  const auto executable = ManifestExecutable(Value(root));
  if (!IsOk(executable)) return Failure(executable);
  const auto user = User(GetCurrentProcess());
  if (!IsOk(user)) return Failure(user);
  HANDLE impersonation{};
  if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &impersonation)) {
    CloseHandle(impersonation);
    return Fail(ErrorCode::AccessDenied, "impersonated_user", Stage::Discover);
  }
  if (GetLastError() != ERROR_NO_TOKEN) return Fail(ErrorCode::AccessDenied, "interactive_user_token", Stage::Discover);
  const auto canonical_executable = CanonicalExecutable(Value(executable));
  if (!IsOk(canonical_executable)) return Failure(canonical_executable);
  auto state = std::make_shared<PackageIdentity::State>();
  DWORD session{};
  if (!ProcessIdToSessionId(GetCurrentProcessId(), &session) || session == 0) return Fail(ErrorCode::AccessDenied, "interactive_session", Stage::Discover);
  state->session = session;
  state->full_name = Value(full_name);
  state->root = Value(root);
  state->executable = Value(canonical_executable);
  state->user = Value(user);
  state->version = std::to_wstring(id->version.Major) + L"." + std::to_wstring(id->version.Minor) + L"." + std::to_wstring(id->version.Build) + L"." + std::to_wstring(id->version.Revision);
  return PackageIdentity(std::move(state));
}

class NativeProcessBackend final : public IProcessBackend {
public:
  explicit NativeProcessBackend(PackageIdentity package) : package_(std::move(package)) {}
  std::chrono::steady_clock::time_point Now() const override { return std::chrono::steady_clock::now(); }
  void Sleep(std::chrono::milliseconds duration) override { std::this_thread::sleep_for(duration); }
  Result<Unit> Install() override { installed_ = true; return Unit{}; }
  Result<Unit> CheckRegistration() override {
    const auto current = DiscoverPackage();
    if (!IsOk(current)) return Fail(ErrorCode::IdentityChanged, "registration_changed");
    const auto& state = *Value(current).state_;
    const auto& previous = *package_.state_;
    if (state.full_name != previous.full_name || state.user != previous.user || state.session != previous.session || !Equal(state.executable, previous.executable) || !Equal(state.root, previous.root)) return Fail(ErrorCode::IdentityChanged, "registration_changed");
    return Unit{};
  }
  Result<Unit> Activate() override;
  bool ActivationRequested() const override { return activated_; }
  void SetDeadline(Deadline deadline) { deadline_ = deadline; }
  Result<std::vector<Candidate>> Enumerate() override {
    auto snapshot = Hold(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (snapshot.get() == INVALID_HANDLE_VALUE) return Fail(ErrorCode::AccessDenied, "process_snapshot");
    PROCESSENTRY32W entry{sizeof(entry)};
    std::vector<Candidate> candidates;
    if (!Process32FirstW(snapshot.get(), &entry)) return Fail(ErrorCode::AccessDenied, "process_enumeration");
    do {
      if (!Equal(entry.szExeFile, L"witcher3.exe")) continue;
      DWORD session{};
      if (!ProcessIdToSessionId(entry.th32ProcessID, &session)) return Fail(ErrorCode::IdentityChanged, "candidate_session");
      if (session != package_.state_->session) continue;
      HANDLE raw = OpenProcess(ReadRights, FALSE, entry.th32ProcessID);
      if (!raw) return Fail(ErrorCode::AccessDenied, "candidate_open", Stage::process_identity, GetLastError());
      auto handle = Hold(raw);
      auto inspected = InspectCandidate(raw);
      if (!IsOk(inspected)) return Failure(inspected);
      auto candidate = Value(inspected);
      candidate.pin = std::move(handle);
      candidates.push_back(std::move(candidate));
    } while (Process32NextW(snapshot.get(), &entry));
    if (GetLastError() != ERROR_NO_MORE_FILES) return Fail(ErrorCode::AccessDenied, "process_enumeration");
    return candidates;
  }
  LaunchIdentity Identity(bool fresh) const {
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    return {Time(now), package_.state_->session, package_.state_->user, package_.state_->full_name, package_.state_->executable, fresh};
  }
private:
  PackageIdentity package_;
  bool installed_{};
  bool activated_{};
  Deadline deadline_{std::chrono::steady_clock::time_point::max()};
};

class VerifiedIdentityFactory {
public:
  static Result<OwnedProcess> Owned(const Candidate& candidate, const LaunchIdentity& launch) {
    const auto verified = Verify(candidate, launch);
    if (!IsOk(verified)) return Failure(verified);
    auto handle = Hold(OpenProcess(OwnedRights, FALSE, candidate.pid));
    if (!handle) return Fail(ErrorCode::AccessDenied, "owned_rights");
    const auto actual = InspectCandidate(handle.get());
    if (!IsOk(actual)) return Failure(actual);
    if (Value(actual).pid != candidate.pid || Value(actual).creation_time != candidate.creation_time) return Fail(ErrorCode::IdentityChanged, "owned_rights_identity");
    const auto checked = SelectCandidate(std::span(&Value(actual), 1), launch);
    if (!IsOk(checked)) return Failure(checked);
    auto user = Safe(candidate.user);
    auto session = std::to_string(candidate.session);
    auto package = Safe(candidate.package);
    if (user.empty() || package.empty()) return Fail(ErrorCode::IdentityChanged, "owned_snapshot_invalid");
    HANDLE transferred{};
    if (!DuplicateHandle(GetCurrentProcess(), handle.get(), GetCurrentProcess(), &transferred, OwnedRights, FALSE, 0)) return Fail(ErrorCode::AccessDenied, "owned_pin");
    return OwnedProcess(transferred, candidate.pid, candidate.creation_time, std::move(user), std::move(session), std::move(package), CloseNative);
  }
  static Result<ObservedProcess> ObservedTarget(const Candidate& candidate, const LaunchIdentity& launch) {
    const auto verified = Verify(candidate, launch);
    if (!IsOk(verified)) return Failure(verified);
    HANDLE transferred{};
    if (!DuplicateHandle(GetCurrentProcess(), candidate.pin.get(), GetCurrentProcess(), &transferred, ReadRights, FALSE, 0)) return Fail(ErrorCode::AccessDenied, "observed_pin");
    return ObservedProcess(transferred, candidate.pid, candidate.creation_time, CloseNative);
  }
  static Result<Candidate> Verify(const Candidate& candidate, const LaunchIdentity& launch) {
    if (!candidate.pin) return Fail(ErrorCode::IdentityChanged, "missing_pin");
    const auto actual = InspectCandidate(candidate.pin.get());
    if (!IsOk(actual)) return Failure(actual);
    if (Value(actual).pid != candidate.pid || Value(actual).creation_time != candidate.creation_time) return Fail(ErrorCode::IdentityChanged, "pin_changed");
    return SelectCandidate(std::span(&Value(actual), 1), launch);
  }
  static Result<Candidate> Revalidate(HANDLE handle, std::uint32_t pid, std::uint64_t creation) {
    const auto package = DiscoverPackage();
    if (!IsOk(package)) return Failure(package);
    NativeProcessBackend backend(Value(package));
    auto actual = InspectCandidate(handle);
    if (!IsOk(actual)) return Failure(actual);
    if (Value(actual).pid != pid || Value(actual).creation_time != creation) return Fail(ErrorCode::IdentityChanged, "pinned_process_changed");
    return SelectCandidate(std::span(&Value(actual), 1), backend.Identity(false));
  }
};

Result<std::unique_ptr<ProcessObserver>> StartObserver(const PackageIdentity& package, Deadline deadline) {
  if (!package.state_->observer.expired()) return Fail(ErrorCode::AlreadyRunning, "observer_already_installed");
  auto backend = std::make_shared<NativeProcessBackend>(package);
  backend->SetDeadline({std::min(deadline.value, backend->Now() + std::chrono::seconds(30))});
  auto installed = InstallProcessObserver(backend->Identity(true), *backend, deadline);
  if (!IsOk(installed)) return Failure(installed);
  auto observer = std::move(std::get<std::unique_ptr<ProcessObserver>>(installed));
  observer->owner_ = backend;
  package.state_->observer = backend;
  return observer;
}
Result<Unit> ActivateRegistered(const PackageIdentity& package) {
  const auto observer = package.state_->observer.lock();
  if (!observer) return Fail(ErrorCode::LaunchFailed, "observer_required", Stage::Launch);
  const auto registered = observer->CheckRegistration();
  if (!IsOk(registered)) return Failure(registered);
  return observer->Activate();
}
Result<OwnedProcess> ProcessObserver::WaitForFreshProcess() {
  if (!owner_) return Fail(ErrorCode::IdentityChanged, "native_observer_required");
  const auto selected = WaitForCandidate();
  if (!IsOk(selected)) return Failure(selected);
  return VerifiedIdentityFactory::Owned(Value(selected), launch_);
}
Result<ObservedProcess> OpenDiagnosticTarget(const PackageIdentity& package) {
  NativeProcessBackend backend(package);
  const auto registration = backend.CheckRegistration();
  if (!IsOk(registration)) return Failure(registration);
  const auto candidates = backend.Enumerate();
  if (!IsOk(candidates)) return Failure(candidates);
  const auto launch = backend.Identity(false);
  const auto selected = SelectCandidate(Value(candidates), launch);
  if (!IsOk(selected)) return Failure(selected);
  return VerifiedIdentityFactory::ObservedTarget(Value(selected), launch);
}
Result<std::unique_ptr<IMemory>> OpenReadOnlyMemory(const ObservedProcess& process) {
  const auto verified = VerifiedIdentityFactory::Revalidate(process.native_handle(), process.pid(), process.creation_time());
  if (!IsOk(verified)) return Failure(verified);
  auto handle = Duplicate(process.native_handle(), ReadRights);
  if (!IsOk(handle)) return Failure(handle);
  return std::unique_ptr<IMemory>(new ReadMemory(Value(handle)));
}
Result<std::unique_ptr<IWritableMemory>> OpenMemory(const OwnedProcess& process) {
  const auto verified = VerifiedIdentityFactory::Revalidate(process.native_handle(), process.pid(), process.creation_time());
  if (!IsOk(verified)) return Failure(verified);
  if (Safe(Value(verified).user) != process.user_identity() || std::to_string(Value(verified).session) != process.session_identity() || Safe(Value(verified).package) != process.package_identity()) return Fail(ErrorCode::IdentityChanged, "owned_snapshot_changed");
  auto handle = Duplicate(process.native_handle(), OwnedRights);
  if (!IsOk(handle)) return Failure(handle);
  const auto fresh = VerifiedIdentityFactory::Revalidate(Value(handle).get(), process.pid(), process.creation_time());
  if (!IsOk(fresh)) return Failure(fresh);
  return std::unique_ptr<IWritableMemory>(new WritableMemory(Value(handle)));
}

EnvironmentSnapshot ObserveEnvironment(const ObservedProcess*) {
  EnvironmentSnapshot result;
  result.selected_adapter.source = "game_adapter_not_queried";
  result.hags_active.source = "active_hags_not_queried";
  result.gpu.source = "adapter_enumeration_not_queried";
  result.driver.source = "driver_enumeration_not_queried";
  result.hags_configured.source = "hags_configuration_not_queried";
  return result;
}

namespace {
Result<Pin> OpenFile(const std::wstring& path) {
  HANDLE raw = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (raw == INVALID_HANDLE_VALUE) return Fail(ErrorCode::AccessDenied, "backing_file_open", Stage::dependencies, GetLastError());
  return Hold(raw);
}
Result<std::wstring> FinalPath(HANDLE file) {
  std::wstring path(MaxPath, L'\0');
  const DWORD length = GetFinalPathNameByHandleW(file, path.data(), static_cast<DWORD>(path.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (length == 0 || length >= path.size()) return Fail(ErrorCode::AccessDenied, "backing_final_path", Stage::dependencies, GetLastError());
  path.resize(length);
  return path;
}
bool SameFile(HANDLE left, HANDLE right) {
  FILE_ID_INFO first{}, second{};
  return GetFileInformationByHandleEx(left, FileIdInfo, &first, sizeof(first)) && GetFileInformationByHandleEx(right, FileIdInfo, &second, sizeof(second)) && first.VolumeSerialNumber == second.VolumeSerialNumber && std::memcmp(&first.FileId, &second.FileId, sizeof(first.FileId)) == 0;
}
bool TrustedSigner(WINTRUST_DATA& trust, const wchar_t* organization) {
  GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
  trust.cbStruct = sizeof(trust);
  trust.dwUIChoice = WTD_UI_NONE;
  trust.fdwRevocationChecks = WTD_REVOKE_NONE;
  trust.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_REVOCATION_CHECK_NONE;
  trust.dwStateAction = WTD_STATEACTION_VERIFY;
  const LONG status = WinVerifyTrust(nullptr, &policy, &trust);
  bool accepted{};
  if (status == ERROR_SUCCESS) {
    const auto* provider = WTHelperProvDataFromStateData(trust.hWVTStateData);
    auto* signer = provider ? WTHelperGetProvSignerFromChain(const_cast<CRYPT_PROVIDER_DATA*>(provider), 0, FALSE, 0) : nullptr;
    if (signer && signer->csCertChain != 0 && signer->pasCertChain[0].pCert) {
      const auto certificate = signer->pasCertChain[0].pCert;
      char attribute[] = szOID_ORGANIZATION_NAME;
      const DWORD count = CertGetNameStringW(certificate, CERT_NAME_ATTR_TYPE, 0, attribute, nullptr, 0);
      if (count > 1 && count <= 256) {
        std::wstring name(count, L'\0');
        if (CertGetNameStringW(certificate, CERT_NAME_ATTR_TYPE, 0, attribute, name.data(), count) == count) {
          name.resize(count - 1);
          accepted = name == organization;
        }
      }
    }
  }
  trust.dwStateAction = WTD_STATEACTION_CLOSE;
  WinVerifyTrust(nullptr, &policy, &trust);
  return accepted;
}
bool Signature(HANDLE file, const std::wstring& path, bool nvidia) {
  WINTRUST_FILE_INFO information{sizeof(information)};
  information.pcwszFilePath = path.c_str();
  information.hFile = file;
  WINTRUST_DATA trust{};
  trust.dwUnionChoice = WTD_CHOICE_FILE;
  trust.pFile = &information;
  const auto* organization = nvidia ? L"NVIDIA Corporation" : L"Microsoft Corporation";
  if (TrustedSigner(trust, organization)) return true;
  if (nvidia) return false;
  HCATADMIN administrator{};
  GUID subsystem = DRIVER_ACTION_VERIFY;
  if (!CryptCATAdminAcquireContext2(&administrator, &subsystem, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) return false;
  struct ReleaseAdmin { HCATADMIN handle; ~ReleaseAdmin() { CryptCATAdminReleaseContext(handle, 0); } } release{administrator};
  DWORD size{};
  if (!CryptCATAdminCalcHashFromFileHandle2(administrator, file, &size, nullptr, 0) || size == 0 || size > 128) return false;
  std::vector<BYTE> hash(size);
  if (!CryptCATAdminCalcHashFromFileHandle2(administrator, file, &size, hash.data(), 0)) return false;
  std::wstring tag;
  for (auto byte : hash) { tag += L"0123456789ABCDEF"[byte >> 4]; tag += L"0123456789ABCDEF"[byte & 15]; }
  HCATINFO catalog = CryptCATAdminEnumCatalogFromHash(administrator, hash.data(), size, 0, nullptr);
  unsigned attempts{};
  while (catalog && attempts++ < 32) {
    CATALOG_INFO catalog_info{sizeof(catalog_info)};
    bool accepted{};
    if (CryptCATCatalogInfoFromContext(catalog, &catalog_info, 0)) {
      WINTRUST_CATALOG_INFO member{sizeof(member)};
      member.pcwszCatalogFilePath = catalog_info.wszCatalogFile;
      member.pcwszMemberTag = tag.c_str();
      member.pcwszMemberFilePath = path.c_str();
      member.hMemberFile = file;
      member.pbCalculatedFileHash = hash.data();
      member.cbCalculatedFileHash = size;
      member.hCatAdmin = administrator;
      WINTRUST_DATA catalog_trust{};
      catalog_trust.dwUnionChoice = WTD_CHOICE_CATALOG;
      catalog_trust.pCatalog = &member;
      accepted = TrustedSigner(catalog_trust, organization);
    }
    if (accepted) { CryptCATAdminReleaseCatalogContext(administrator, catalog, 0); return true; }
    HCATINFO previous = catalog;
    catalog = CryptCATAdminEnumCatalogFromHash(administrator, hash.data(), size, 0, &previous);
  }
  if (catalog) CryptCATAdminReleaseCatalogContext(administrator, catalog, 0);
  return false;
}
Result<Bytes> FileBytes(HANDLE file) {
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file, &size)) return Fail(ErrorCode::AccessDenied, "file_length", Stage::dependencies);
  if (size.QuadPart <= 0 || size.QuadPart > 128 * 1024 * 1024) return Fail(ErrorCode::UntrustedInterposer, "file_size_limit", Stage::dependencies);
  LARGE_INTEGER zero{};
  if (!SetFilePointerEx(file, zero, nullptr, FILE_BEGIN)) return Fail(ErrorCode::AccessDenied, "file_position", Stage::dependencies);
  Bytes bytes(static_cast<std::size_t>(size.QuadPart));
  DWORD received{};
  if (!ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &received, nullptr) || received != bytes.size()) return Fail(ErrorCode::AccessDenied, "file_read_exact", Stage::dependencies);
  return bytes;
}
Result<Hash256> Digest(std::span<const std::byte> bytes) {
  Hash256 hash{};
  BCRYPT_ALG_HANDLE algorithm{};
  if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return Fail(ErrorCode::UntrustedInterposer, "sha256_open", Stage::dependencies);
  const auto status = BCryptHash(algorithm, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<std::byte*>(bytes.data())), static_cast<ULONG>(bytes.size()), reinterpret_cast<PUCHAR>(hash.data()), static_cast<ULONG>(hash.size()));
  BCryptCloseAlgorithmProvider(algorithm, 0);
  if (status < 0) return Fail(ErrorCode::UntrustedInterposer, "sha256_hash", Stage::dependencies);
  return hash;
}
std::string FileVersion(const std::wstring& path) {
  DWORD ignored{};
  const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
  if (size == 0 || size > 1024 * 1024) return {};
  std::vector<std::byte> bytes(size);
  if (!GetFileVersionInfoW(path.c_str(), 0, size, bytes.data())) return {};
  VS_FIXEDFILEINFO* info{};
  UINT length{};
  if (!VerQueryValueW(bytes.data(), L"\\", reinterpret_cast<void**>(&info), &length) || length < sizeof(*info) || info->dwSignature != 0xfeef04bd) return {};
  auto version = std::to_string(HIWORD(info->dwFileVersionMS)) + "." + std::to_string(LOWORD(info->dwFileVersionMS)) + "." + std::to_string(HIWORD(info->dwFileVersionLS));
  if (LOWORD(info->dwFileVersionLS) != 0) version += "." + std::to_string(LOWORD(info->dwFileVersionLS));
  return version;
}
Result<std::wstring> MappedPath(HANDLE process, Address base) {
  std::wstring path(MaxPath, L'\0');
  DWORD length = GetMappedFileNameW(process, reinterpret_cast<LPVOID>(base), path.data(), static_cast<DWORD>(path.size()));
  if (length == 0 || length >= path.size() - 1) return Fail(ErrorCode::AccessDenied, "mapped_image_path", Stage::dependencies, GetLastError());
  path.resize(length);
  if (!path.starts_with(L"\\Device\\")) return Fail(ErrorCode::UntrustedInterposer, "mapped_image_namespace", Stage::dependencies);
  return L"\\\\?\\GLOBALROOT" + path;
}
Result<process_detail::FileEvidence> Evidence(HANDLE file, const std::wstring& path, bool nvidia) {
  const auto bytes = FileBytes(file);
  if (!IsOk(bytes)) return Failure(bytes);
  const auto hash = Digest(Value(bytes));
  if (!IsOk(hash)) return Failure(hash);
  if (!Signature(file, path, nvidia)) return Fail(ErrorCode::UntrustedInterposer, "offline_signer_trust", Stage::dependencies);
  const auto version = FileVersion(path);
  if (version.empty()) return Fail(ErrorCode::UntrustedInterposer, "file_version", Stage::dependencies);
  return process_detail::FileEvidence{Value(bytes).size(), Value(hash), version, true, nvidia ? DependencySigner::Nvidia : DependencySigner::Microsoft};
}
}

namespace process_detail {
Observed<DependencyRole> IdentifyDependencyRole(const IMemory& memory, const PeImage& image, const FileEvidence& evidence, std::string_view) {
  Observed<DependencyRole> role;
  role.source = "authenticated_loaded_exports";
  if (!evidence.trusted_signature || evidence.signer != DependencySigner::Nvidia || evidence.version.empty()) {
    role.reason = "nvidia_file_identity_unverified";
    return role;
  }
  const auto exported = [&](std::string_view name) {
    const auto entry = FindExport(memory, image, name);
    return IsOk(entry) && !Value(entry).forwarder && Value(entry).rva != 0 && Value(entry).rva < image.size_of_image;
  };
  const auto& required = SupportedBuild().interposer;
  if (evidence.length == required.length && evidence.sha256 == required.sha256 && evidence.version == required.version && exported("D3D12CreateDevice") && exported("CreateDXGIFactory") && exported("CreateDXGIFactory1") && exported("CreateDXGIFactory2")) role.value = DependencyRole::Interposer;
  else if (exported("slGetPluginFunction")) role.value = DependencyRole::StreamlinePlugin;
  else if ((exported("NVSDK_NGX_D3D12_Init") && exported("NVSDK_NGX_D3D12_EvaluateFeature")) || (exported("NVSDK_NGX_D3D12_CreateFeature") && exported("NVSDK_NGX_D3D12_EvaluateFeature"))) role.value = DependencyRole::NgxPlugin;
  if (role.value) role.availability = Availability::Observed;
  else { role.availability = Availability::UnsupportedLayout; role.reason = "export_role_not_identified"; }
  return role;
}
Result<Unit> ValidateStartupInventory(const RuntimeInventory& inventory) {
  const auto& expected = SupportedBuild().interposer;
  if (inventory.interposer.loaded_module.image.base != 0) {
    const auto& interposer = inventory.interposer;
    if (!interposer.trusted_nvidia_signer || !interposer.backing_image_matches_loaded_module || interposer.version != expected.version || interposer.length != expected.length || interposer.sha256 != expected.sha256) return Fail(ErrorCode::UntrustedInterposer, "interposer_startup_identity", Stage::dependencies);
  }
  bool d3d12{}, dxgi{};
  for (const auto& module : inventory.trusted_system_modules) {
    if (!module.trusted_system || module.module.image.base == 0) return Fail(ErrorCode::UntrustedInterposer, "native_startup_identity", Stage::dependencies);
    d3d12 |= EqualAscii(module.module.name, "d3d12.dll");
    dxgi |= EqualAscii(module.module.name, "dxgi.dll");
  }
  if (inventory.executable.image.base == 0 || inventory.interposer.loaded_module.image.base == 0 || !d3d12 || !dxgi) return Fail(ErrorCode::ModuleNotReady, "startup_modules_missing", Stage::dependencies);
  return Unit{};
}
Result<Unit> ReadIntoHandle(void* handle, Address address, std::span<std::byte> bytes) {
  if (!handle || bytes.size() > std::numeric_limits<Address>::max() - address) return Fail(ErrorCode::UnreadableMemory, "read_overflow");
  SIZE_T received{};
  if (!bytes.empty() && (!ReadProcessMemory(handle, reinterpret_cast<LPCVOID>(address), bytes.data(), bytes.size(), &received) || received != bytes.size())) return Fail(ErrorCode::UnreadableMemory, "read_exact", Stage::Inspect, GetLastError());
  return Unit{};
}
Result<FileEvidence> InspectFile(const std::wstring& path, bool require_nvidia) {
  const auto file = OpenFile(path);
  if (!IsOk(file)) return Failure(file);
  return Evidence(Value(file).get(), path, require_nvidia);
}
Result<Unit> VerifyBackingImage(void* handle, Address base, const std::wstring& path, const IMemory& memory) {
  const auto backing = OpenFile(path);
  if (!IsOk(backing)) return Failure(backing);
  const auto mapped_path = MappedPath(handle, base);
  if (!IsOk(mapped_path)) return Failure(mapped_path);
  const auto mapped_file = OpenFile(Value(mapped_path));
  if (!IsOk(mapped_file)) return Failure(mapped_file);
  if (!SameFile(Value(backing).get(), Value(mapped_file).get())) return Fail(ErrorCode::UntrustedInterposer, "backing_file_identity", Stage::dependencies);
  MEMORY_BASIC_INFORMATION region{};
  if (VirtualQueryEx(handle, reinterpret_cast<LPCVOID>(base), &region, sizeof(region)) != sizeof(region) || region.Type != MEM_IMAGE || region.AllocationBase != reinterpret_cast<LPVOID>(base)) return Fail(ErrorCode::UntrustedInterposer, "backing_image_mapping", Stage::dependencies);
  auto mapping = Hold(CreateFileMappingW(Value(backing).get(), nullptr, PAGE_READONLY | SEC_IMAGE_NO_EXECUTE, 0, 0, nullptr));
  if (!mapping) return Fail(ErrorCode::UntrustedInterposer, "backing_image_section", Stage::dependencies);
  const auto* view = static_cast<const std::byte*>(MapViewOfFile(mapping.get(), FILE_MAP_READ, 0, 0, 0));
  if (!view) return Fail(ErrorCode::UntrustedInterposer, "backing_image_view", Stage::dependencies);
  struct Unmap { const void* view; ~Unmap() { UnmapViewOfFile(view); } } unmap{view};
  auto local_handle = Duplicate(GetCurrentProcess(), ReadRights);
  if (!IsOk(local_handle)) return Failure(local_handle);
  ReadMemory local(Value(local_handle));
  const auto local_image = InspectPe(local, reinterpret_cast<Address>(view));
  const auto remote_image = InspectPe(memory, base);
  if (!IsOk(local_image)) return Failure(local_image);
  if (!IsOk(remote_image)) return Failure(remote_image);
  const auto& image = Value(local_image);
  if (image.machine != IMAGE_FILE_MACHINE_AMD64 || image.timestamp != Value(remote_image).timestamp || image.size_of_image != Value(remote_image).size_of_image || image.size_of_image > 128 * 1024 * 1024) return Fail(ErrorCode::UntrustedInterposer, "backing_pe_identity", Stage::dependencies);
  IMAGE_DOS_HEADER dos{};
  auto read_dos = local.Read(reinterpret_cast<Address>(view), sizeof(dos));
  if (!IsOk(read_dos)) return Failure(read_dos);
  std::memcpy(&dos, Value(read_dos).data(), sizeof(dos));
  IMAGE_NT_HEADERS64 header{};
  auto read_header = local.Read(reinterpret_cast<Address>(view) + dos.e_lfanew, sizeof(header));
  if (!IsOk(read_header)) return Failure(read_header);
  std::memcpy(&header, Value(read_header).data(), sizeof(header));
  if (header.OptionalHeader.SizeOfHeaders > image.size_of_image || header.OptionalHeader.SizeOfHeaders > 1024 * 1024) return Fail(ErrorCode::UntrustedInterposer, "backing_headers_size", Stage::dependencies);
  const auto expected_header = local.Read(reinterpret_cast<Address>(view), header.OptionalHeader.SizeOfHeaders);
  const auto actual_header = memory.Read(base, header.OptionalHeader.SizeOfHeaders);
  if (!IsOk(expected_header) || !IsOk(actual_header) || Value(expected_header) != Value(actual_header)) return Fail(ErrorCode::UntrustedInterposer, "backing_headers_match", Stage::dependencies);
  const auto& relocation = image.directories[IMAGE_DIRECTORY_ENTRY_BASERELOC];
  std::vector<Rva> relocations;
  if (relocation.size != 0) {
    if (relocation.rva > image.size_of_image || relocation.size > image.size_of_image - relocation.rva || relocation.size > 16 * 1024 * 1024) return Fail(ErrorCode::UntrustedInterposer, "backing_relocations", Stage::dependencies);
    const auto bytes = local.Read(reinterpret_cast<Address>(view) + relocation.rva, relocation.size);
    if (!IsOk(bytes)) return Failure(bytes);
    std::size_t offset{};
    while (offset < Value(bytes).size()) {
      IMAGE_BASE_RELOCATION block{};
      if (Value(bytes).size() - offset < sizeof(block)) return Fail(ErrorCode::UntrustedInterposer, "backing_relocation_block", Stage::dependencies);
      std::memcpy(&block, Value(bytes).data() + offset, sizeof(block));
      if (block.SizeOfBlock < sizeof(block) || block.SizeOfBlock > Value(bytes).size() - offset || block.SizeOfBlock % 2 != 0) return Fail(ErrorCode::UntrustedInterposer, "backing_relocation_block", Stage::dependencies);
      for (std::size_t entry = sizeof(block); entry < block.SizeOfBlock; entry += 2) {
        WORD value{};
        std::memcpy(&value, Value(bytes).data() + offset + entry, sizeof(value));
        if ((value >> 12) == IMAGE_REL_BASED_ABSOLUTE) continue;
        const auto displacement = static_cast<std::uint32_t>(value & 0xfff);
        if ((value >> 12) != IMAGE_REL_BASED_DIR64 || block.VirtualAddress >= image.size_of_image || displacement > image.size_of_image - block.VirtualAddress || image.size_of_image - block.VirtualAddress - displacement < 8) return Fail(ErrorCode::UntrustedInterposer, "backing_relocation_type", Stage::dependencies);
        relocations.push_back(block.VirtualAddress + (value & 0xfff));
      }
      offset += block.SizeOfBlock;
    }
  }
  const auto compare = [&](Rva rva, std::uint32_t size) -> Result<Unit> {
    if (rva > image.size_of_image || size > image.size_of_image - rva || size > 64 * 1024 * 1024) return Fail(ErrorCode::UntrustedInterposer, "backing_range", Stage::dependencies);
    auto expected = local.Read(reinterpret_cast<Address>(view) + rva, size);
    auto actual = memory.Read(base + rva, size);
    if (!IsOk(expected) || !IsOk(actual)) return Fail(ErrorCode::UntrustedInterposer, "backing_code_read", Stage::dependencies);
    auto normalized = std::move(std::get<Bytes>(expected));
    for (auto relocation_rva : relocations) {
      if (relocation_rva >= rva && relocation_rva - rva < size) {
        if (size - (relocation_rva - rva) < 8) return Fail(ErrorCode::UntrustedInterposer, "backing_relocation_boundary", Stage::dependencies);
        std::uint64_t pointer{};
        std::memcpy(&pointer, normalized.data() + relocation_rva - rva, 8);
        pointer += base - header.OptionalHeader.ImageBase;
        std::memcpy(normalized.data() + relocation_rva - rva, &pointer, 8);
      }
    }
    if (normalized != Value(actual)) return Fail(ErrorCode::UntrustedInterposer, "backing_code_match", Stage::dependencies);
    return Unit{};
  };
  const Address section_table = reinterpret_cast<Address>(view) + dos.e_lfanew + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + header.FileHeader.SizeOfOptionalHeader;
  for (std::size_t index = 0; index < header.FileHeader.NumberOfSections; ++index) {
    const auto section_bytes = local.Read(section_table + index * sizeof(IMAGE_SECTION_HEADER), sizeof(IMAGE_SECTION_HEADER));
    if (!IsOk(section_bytes)) return Failure(section_bytes);
    IMAGE_SECTION_HEADER section{};
    std::memcpy(&section, Value(section_bytes).data(), sizeof(section));
    if ((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) continue;
    if (section.Characteristics & IMAGE_SCN_MEM_WRITE) return Fail(ErrorCode::UntrustedInterposer, "backing_writable_code", Stage::dependencies);
    const auto matched = compare(section.VirtualAddress, section.Misc.VirtualSize);
    if (!IsOk(matched)) return Failure(matched);
  }
  const auto& exports = image.directories[IMAGE_DIRECTORY_ENTRY_EXPORT];
  if (exports.size != 0) {
    const auto matched = compare(exports.rva, exports.size);
    if (!IsOk(matched)) return Failure(matched);
  }
  return Unit{};
}
}

Result<Unit> NativeProcessBackend::Activate() {
  if (!installed_ || activated_) return Fail(ErrorCode::LaunchFailed, "activation_state", Stage::Launch);
  if (Now() >= deadline_.value) return Fail(ErrorCode::TimedOut, "activation_deadline", Stage::Launch);
  const auto registered = CheckRegistration();
  if (!IsOk(registered)) return Failure(registered);
  const auto existing = Enumerate();
  if (!IsOk(existing)) return Failure(existing);
  const auto selected = SelectCandidate(Value(existing), Identity(false));
  if (IsOk(selected) || Failure(selected).code != ErrorCode::NoMatchingProcess) return Fail(IsOk(selected) ? ErrorCode::AlreadyRunning : Failure(selected).code, "activation_existing", Stage::Launch);
  wchar_t windows[MAX_PATH]{};
  const auto length = GetWindowsDirectoryW(windows, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) return Fail(ErrorCode::LaunchFailed, "windows_directory", Stage::Launch);
  const auto explorer = std::wstring(windows) + L"\\explorer.exe";
  const auto file = OpenFile(explorer);
  if (!IsOk(file)) return Failure(file);
  if (!Signature(Value(file).get(), explorer, false)) return Fail(ErrorCode::LaunchFailed, "explorer_offline_trust", Stage::Launch);
  std::wstring command = L"\"" + explorer + L"\" \"shell:AppsFolder\\" + Family + L"!" + Application + L"\"";
  STARTUPINFOW startup{sizeof(startup)};
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(explorer.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT, nullptr, windows, &startup, &process)) return Fail(ErrorCode::LaunchFailed, "registered_activation", Stage::Launch, GetLastError());
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  activated_ = true;
  return Unit{};
}

namespace {
DependencyIdentity Dependency(const std::string& basename, const process_detail::FileEvidence& evidence, Observed<DependencyRole> role, const char* source) {
  DependencyIdentity identity;
  identity.basename = basename;
  identity.version = {evidence.version, Availability::Observed, source, {}};
  identity.sha256 = {evidence.sha256, Availability::Observed, source, {}};
  identity.signer = {evidence.signer, evidence.signer ? Availability::Observed : Availability::NotObserved, "offline_authenticode", {}};
  identity.role = std::move(role);
  identity.availability = Availability::Observed;
  identity.source = source;
  return identity;
}
bool ProxyName(const std::wstring& basename) {
  constexpr std::array names{L"dxgi.dll", L"d3d12.dll", L"d3d11.dll", L"dinput8.dll", L"version.dll", L"winmm.dll", L"dsound.dll", L"winhttp.dll", L"wininet.dll", L"xinput1_3.dll", L"dbghelp.dll"};
  return std::any_of(names.begin(), names.end(), [&](const auto* name) { return Equal(name, basename); });
}
Result<RuntimeInventory> Inventory(HANDLE process, std::uint32_t pid, std::uint64_t creation, const IMemory& memory) {
  const auto identity = VerifiedIdentityFactory::Revalidate(process, pid, creation);
  if (!IsOk(identity)) return Failure(identity);
  const auto package = DiscoverPackage();
  if (!IsOk(package)) return Failure(package);
  RuntimeInventory inventory;
  inventory.package_name = Value(package).name();
  inventory.package_version = Value(package).version();
  std::vector<HMODULE> modules(1024);
  DWORD needed{};
  if (!EnumProcessModulesEx(process, modules.data(), static_cast<DWORD>(modules.size() * sizeof(HMODULE)), &needed, LIST_MODULES_64BIT) || needed > modules.size() * sizeof(HMODULE) || needed % sizeof(HMODULE) != 0) return Fail(ErrorCode::AccessDenied, "module_enumeration", Stage::dependencies, GetLastError());
  if (needed == 0) return Fail(ErrorCode::ModuleNotReady, "startup_modules_missing", Stage::dependencies);
  modules.resize(needed / sizeof(HMODULE));
  wchar_t system_buffer[MAX_PATH]{};
  if (!GetSystemDirectoryW(system_buffer, MAX_PATH)) return Fail(ErrorCode::AccessDenied, "system_directory", Stage::dependencies);
  const auto system_probe = OpenFile(std::wstring(system_buffer) + L"\\kernel32.dll");
  if (!IsOk(system_probe)) return Failure(system_probe);
  const auto system_final = FinalPath(Value(system_probe).get());
  if (!IsOk(system_final)) return Failure(system_final);
  const auto system_directory = std::filesystem::path(Value(system_final)).parent_path().wstring();
  const auto local_directory = std::filesystem::path(Value(identity).executable).parent_path();
  struct LoadedDependency { std::string basename; Pin file; std::size_t dependency_index; };
  std::vector<LoadedDependency> loaded_dependencies;
  bool have_executable{}, have_interposer{};
  for (const auto module : modules) {
    std::wstring path(MaxPath, L'\0');
    const auto length = GetModuleFileNameExW(process, module, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size() - 1) return Fail(ErrorCode::AccessDenied, "module_path", Stage::dependencies);
    path.resize(length);
    const auto basename = Safe(std::filesystem::path(path).filename().wstring());
    if (basename.empty()) return Fail(ErrorCode::DllConflict, "module_basename", Stage::dependencies);
    const auto base = reinterpret_cast<Address>(module);
    const auto module_executable = CanonicalExecutable(path);
    if (!IsOk(module_executable)) return Failure(module_executable);
    if (Equal(Value(module_executable), Value(identity).executable)) {
      if (have_executable) return Fail(ErrorCode::AmbiguousProcess, "multiple_executable_images");
      const auto image = InspectPe(memory, base);
      if (!IsOk(image)) return Failure(image);
      inventory.executable = {basename, Value(image)};
      have_executable = true;
      continue;
    }
    const bool interposer = Equal(std::filesystem::path(path).filename().wstring(), L"sl.interposer.dll");
    const auto file = OpenFile(path);
    if (!IsOk(file)) return Failure(file);
    const auto canonical = FinalPath(Value(file).get());
    if (!IsOk(canonical)) return Failure(canonical);
    const bool system = Equal(std::filesystem::path(Value(canonical)).parent_path().wstring(), system_directory);
    const bool required_native = Equal(std::filesystem::path(path).filename().wstring(), L"d3d12.dll") || Equal(std::filesystem::path(path).filename().wstring(), L"dxgi.dll");
    if (required_native && !system) return Fail(ErrorCode::DllConflict, "native_module_location", Stage::dependencies);
    if (interposer && !Equal(std::filesystem::path(Value(canonical)).parent_path().wstring(), local_directory.wstring())) return Fail(ErrorCode::UntrustedInterposer, "interposer_location", Stage::dependencies);
    if (system || interposer) {
      if (!Signature(Value(file).get(), path, interposer)) return Fail(ErrorCode::UntrustedInterposer, "module_offline_trust", Stage::dependencies);
      const auto backing = process_detail::VerifyBackingImage(process, base, path, memory);
      if (!IsOk(backing)) return Failure(backing);
      const auto image = InspectPe(memory, base);
      if (!IsOk(image)) return Failure(image);
      if (interposer) {
        if (have_interposer) return Fail(ErrorCode::DllConflict, "multiple_interposers", Stage::dependencies);
        const auto evidence = Evidence(Value(file).get(), path, true);
        if (!IsOk(evidence)) return Failure(evidence);
        const auto& expected = SupportedBuild().interposer;
        if (Value(evidence).length != expected.length || Value(evidence).sha256 != expected.sha256 || Value(evidence).version != expected.version) return Fail(ErrorCode::UntrustedInterposer, "interposer_build", Stage::dependencies);
        inventory.interposer = {{basename, Value(image)}, Value(evidence).version, Value(evidence).length, Value(evidence).sha256, true, true};
        auto role = process_detail::IdentifyDependencyRole(memory, Value(image), Value(evidence), basename);
        if (role.value != DependencyRole::Interposer) return Fail(ErrorCode::UntrustedInterposer, "interposer_export_role", Stage::dependencies);
        inventory.dependencies.push_back(Dependency(basename, Value(evidence), std::move(role), "loaded_backing_image"));
        loaded_dependencies.push_back({basename, Value(file), inventory.dependencies.size() - 1});
        have_interposer = true;
      } else {
        inventory.trusted_system_modules.push_back({{basename, Value(image)}, true});
      }
    } else {
      const auto image = InspectPe(memory, base);
      if (!IsOk(image)) return Failure(image);
      const auto evidence = Evidence(Value(file).get(), path, true);
      if (IsOk(evidence)) {
        const auto backing = process_detail::VerifyBackingImage(process, base, path, memory);
        if (!IsOk(backing)) return Failure(backing);
        auto role = process_detail::IdentifyDependencyRole(memory, Value(image), Value(evidence), basename);
        inventory.dependencies.push_back(Dependency(basename, Value(evidence), std::move(role), "loaded_backing_image"));
        loaded_dependencies.push_back({basename, Value(file), inventory.dependencies.size() - 1});
      } else {
        if (Failure(evidence).code == ErrorCode::AccessDenied) return Failure(evidence);
        DependencyIdentity unknown;
        unknown.basename = basename;
        unknown.availability = Availability::NotObserved;
        unknown.source = "loaded_file_identity_unverified";
        unknown.role.source = "authenticated_loaded_exports";
        unknown.role.reason = "nvidia_file_identity_unverified";
        inventory.dependencies.push_back(std::move(unknown));
      }
    }
  }
  std::error_code filesystem_error;
  std::filesystem::directory_iterator files(local_directory, filesystem_error);
  if (filesystem_error) return Fail(ErrorCode::AccessDenied, "local_loader_enumeration", Stage::dependencies);
  unsigned count{};
  for (const auto& entry : files) {
    if (++count > 4096) return Fail(ErrorCode::DllConflict, "local_loader_limit", Stage::dependencies);
    if (!Equal(entry.path().extension().wstring(), L".dll")) continue;
    const auto basename = Safe(entry.path().filename().wstring());
    if (basename.empty()) return Fail(ErrorCode::DllConflict, "local_loader_basename", Stage::dependencies);
    const auto file = OpenFile(entry.path().wstring());
    if (!IsOk(file)) return Failure(file);
    const auto loaded = std::find_if(loaded_dependencies.begin(), loaded_dependencies.end(), [&](const LoadedDependency& identity) { return identity.basename == basename && SameFile(identity.file.get(), Value(file).get()); });
    if (loaded != loaded_dependencies.end()) {
      const auto& dependency = inventory.dependencies[loaded->dependency_index];
      const bool official = dependency.signer.value == DependencySigner::Nvidia && dependency.role.value.has_value();
      if (official || ProxyName(entry.path().filename().wstring())) inventory.application_local_loaders.push_back({basename, dependency.version.value.value_or(""), dependency.signer.value.has_value(), official});
      continue;
    }
    const auto evidence = Evidence(Value(file).get(), entry.path().wstring(), true);
    if (IsOk(evidence)) {
      auto mapping = Hold(CreateFileMappingW(Value(file).get(), nullptr, PAGE_READONLY | SEC_IMAGE_NO_EXECUTE, 0, 0, nullptr));
      const auto* view = mapping ? MapViewOfFile(mapping.get(), FILE_MAP_READ, 0, 0, 0) : nullptr;
      if (!view) return Fail(ErrorCode::DllConflict, "local_plugin_image", Stage::dependencies);
      struct Unmap { const void* view; ~Unmap() { UnmapViewOfFile(view); } } unmap{view};
      auto handle = Duplicate(GetCurrentProcess(), ReadRights);
      if (!IsOk(handle)) return Failure(handle);
      ReadMemory local(Value(handle));
      const auto image = InspectPe(local, reinterpret_cast<Address>(view));
      if (!IsOk(image)) return Failure(image);
      auto role = process_detail::IdentifyDependencyRole(local, Value(image), Value(evidence), basename);
      const bool official = role.value.has_value();
      inventory.dependencies.push_back(Dependency(basename, Value(evidence), std::move(role), "application_local_file"));
      if (official || ProxyName(entry.path().filename().wstring())) inventory.application_local_loaders.push_back({basename, Value(evidence).version, true, official});
    } else {
      if (Failure(evidence).code == ErrorCode::AccessDenied) return Failure(evidence);
      DependencyIdentity unknown;
      unknown.basename = basename;
      unknown.availability = Availability::NotObserved;
      unknown.source = "application_local_file_unverified";
      unknown.role.source = "authenticated_file_exports";
      unknown.role.reason = "nvidia_file_identity_unverified";
      inventory.dependencies.push_back(std::move(unknown));
      if (ProxyName(entry.path().filename().wstring())) inventory.application_local_loaders.push_back({basename, FileVersion(entry.path().wstring()), false, false});
    }
  }
  const auto final_identity = VerifiedIdentityFactory::Revalidate(process, pid, creation);
  if (!IsOk(final_identity)) return Failure(final_identity);
  const auto startup = process_detail::ValidateStartupInventory(inventory);
  if (!IsOk(startup)) return Failure(startup);
  return inventory;
}
}
Result<RuntimeInventory> InventoryProcess(const OwnedProcess& process, const IMemory& memory) {
  const auto verified = VerifiedIdentityFactory::Revalidate(process.native_handle(), process.pid(), process.creation_time());
  if (!IsOk(verified)) return Failure(verified);
  if (Safe(Value(verified).user) != process.user_identity() || std::to_string(Value(verified).session) != process.session_identity() || Safe(Value(verified).package) != process.package_identity()) return Fail(ErrorCode::IdentityChanged, "owned_snapshot_changed");
  return Inventory(process.native_handle(), process.pid(), process.creation_time(), memory);
}
Result<RuntimeInventory> InventoryObservedProcess(const ObservedProcess& process, const IMemory& memory) {
  return Inventory(process.native_handle(), process.pid(), process.creation_time(), memory);
}

}
