#include "wfg/support_storage_windows.hpp"
#include "local_storage_windows.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <utility>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <winternl.h>
#endif

namespace wfg {
namespace {
Error StorageError(std::string_view operation) {
#ifdef _WIN32
  return {ErrorCode::SupportBundleFailure, Stage::Support, operation, GetLastError(), {}};
#else
  return {ErrorCode::SupportBundleFailure, Stage::Support, operation, {}, {}};
#endif
}
#ifdef _WIN32
using local_storage::NativeHandle;
using local_storage::RelativeOpen;
using local_storage::Same;
struct SourceFile {
  NativeHandle handle;
  BY_HANDLE_FILE_INFORMATION identity{};
  std::filesystem::path path;
  HANDLE directory{};
};

#endif
}

struct WindowsSupportStorage::State {
  std::filesystem::path root, completed;
#ifdef WFG_SUPPORT_TESTING
  WindowsSupportStorage* test_owner{};
#endif
#ifdef _WIN32
  std::vector<NativeHandle> directories;
  std::map<std::uint64_t, std::vector<SourceFile>> sources;
  std::uint64_t next_token{1};
  bool pinned{};
  local_storage::DirectoryLease diagnostics_lease;
  bool locality_refused{};
  std::wstring local_volume_root;
  HANDLE root_directory{INVALID_HANDLE_VALUE}, diagnostics_directory{INVALID_HANDLE_VALUE}, support_directory{INVALID_HANDLE_VALUE};
  Error RootStorageError(std::string_view operation) const {
    return StorageError(locality_refused ? "SupportLocalStorageRequired" : operation);
  }
  std::optional<std::wstring> LocalVolumeRoot() {
    const auto refuse = [&]() -> std::optional<std::wstring> {
      locality_refused = true; SetLastError(ERROR_NOT_SUPPORTED); return std::nullopt;
    };
    locality_refused = false;
    const auto drive = root.root_name().native();
    if (!root.is_absolute() || drive.size() != 2 || drive[1] != L':' ||
        !((drive[0] >= L'A' && drive[0] <= L'Z') || (drive[0] >= L'a' && drive[0] <= L'z'))) return refuse();
    for (const auto& part : root.relative_path()) if (part == L".." || part == L"." || part.empty()) return refuse();
    const auto drive_root = drive + L"\\";
#ifdef WFG_SUPPORT_TESTING
    ++test_owner->locality_probe_count;
    const auto type = test_owner->drive_type_override ? *test_owner->drive_type_override : GetDriveTypeW(drive_root.c_str());
#else
    const auto type = GetDriveTypeW(drive_root.c_str());
#endif
    if (type != DRIVE_FIXED) return refuse();
    std::wstring device;
#ifdef WFG_SUPPORT_TESTING
    if (test_owner->device_target_override) device = *test_owner->device_target_override;
    else
#endif
    {
      std::array<wchar_t,512> targets{};
      const auto count = QueryDosDeviceW(drive.c_str(),targets.data(),static_cast<DWORD>(targets.size()));
      if (count == 0 || count >= targets.size()) return refuse();
      device = targets.data();
    }
    const auto volume = local_storage::ClassifyLocalVolume(root,type,device);
    if(!volume)return refuse();
    return volume;
  }

  bool DirectoriesValid() {
    if (!pinned || !PinRoot()) return false;
    for (const auto& handle : directories) {
      BY_HANDLE_FILE_INFORMATION info{};
      if (!GetFileInformationByHandle(handle.get(),&info) || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
          (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) return false;
    }
    return true;
  }
  bool PinRoot() {
    const auto volume = LocalVolumeRoot();
    if (!volume) return false;
    if (pinned) {
      if (*volume != local_volume_root) { locality_refused = true; SetLastError(ERROR_NOT_SUPPORTED); return false; }
      return true;
    }
    local_volume_root = *volume;
#ifdef WFG_SUPPORT_TESTING
    ++test_owner->root_open_attempts;
    if(test_owner->fail_before_root_open)return false;
#endif
    if(!local_storage::PinDirectories(root,local_volume_root,1,directories))return false;
    root_directory = directories.back().get();
    pinned = true; return true;
  }
  bool PinChild(std::wstring_view name, bool create, bool& missing) {
    missing = false;
    if (!PinRoot()) return false;
    NativeHandle handle = RelativeOpen(root_directory,name,FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY | (create ? FILE_ADD_FILE : 0U),FILE_SHARE_READ | FILE_SHARE_WRITE,create ? 3U : 1U,true);
    if (!handle) { missing = GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND; return missing && !create; }
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle.get(), &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) return false;
    if (name == L"diagnostics") {
      if(!diagnostics_lease.Acquire(handle.get()))return false;
      diagnostics_directory = handle.get();
    }
    if (name == L"support") support_directory = handle.get();
    directories.push_back(std::move(handle)); return DirectoriesValid();
  }
  bool ExpectedFiles() const {
    alignas(FILE_ID_BOTH_DIR_INFO) std::array<std::byte,4096> buffer{};
    bool restart = true;
    for (unsigned page = 0; page < 32; ++page) {
      if (!GetFileInformationByHandleEx(diagnostics_directory,restart ? FileIdBothDirectoryRestartInfo : FileIdBothDirectoryInfo,buffer.data(),static_cast<DWORD>(buffer.size()))) return GetLastError() == ERROR_NO_MORE_FILES;
      restart = false;
      std::size_t offset{};
      for (;;) {
        if (offset + offsetof(FILE_ID_BOTH_DIR_INFO,FileName) > buffer.size()) return false;
        const auto* entry = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(buffer.data() + offset);
        if ((entry->FileNameLength % sizeof(wchar_t)) != 0 || entry->FileNameLength > buffer.size() - offset - offsetof(FILE_ID_BOTH_DIR_INFO,FileName)) return false;
        const std::wstring_view name(entry->FileName,entry->FileNameLength / sizeof(wchar_t));
        if (name != L"." && name != L"..") {
          bool allowed = name == L"last-run.json" || name == L"last-run.json.tmp" || name == L"events.jsonl";
          for (unsigned index = 1; index < 10; ++index) if (name == L"events." + std::to_wstring(index) + L".jsonl") allowed = true;
          if (!allowed || (entry->FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) return false;
        }
        if (entry->NextEntryOffset == 0) break;
        if (entry->NextEntryOffset > buffer.size() - offset) return false;
        offset += entry->NextEntryOffset;
      }
    }
    return false;
  }
  Result<SupportReadHandle> Open(bool events) {
    bool missing{};
    if (!PinChild(L"diagnostics", false, missing)) return RootStorageError("PinDiagnosticsDirectory");
    if (missing) return SupportReadHandle{};
    if (!ExpectedFiles()) return StorageError("UnexpectedDiagnosticFile");
    std::vector<SourceFile> files;
    for (unsigned index = 0; index < (events ? 10U : 1U); ++index) {
      const auto name = events ? (index ? L"events." + std::to_wstring(index) + L".jsonl" : L"events.jsonl") : L"last-run.json";
      const auto path = root / L"diagnostics" / name;
      NativeHandle handle = RelativeOpen(diagnostics_directory,name,GENERIC_READ,FILE_SHARE_READ,1);
      if (!handle) { if (GetLastError() == ERROR_FILE_NOT_FOUND) continue; return StorageError("OpenSupportSource"); }
      BY_HANDLE_FILE_INFORMATION info{};
      if (!GetFileInformationByHandle(handle.get(), &info) || info.nNumberOfLinks != 1 ||
          (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) return StorageError("ValidateSupportSource");
      files.push_back({std::move(handle), info, path,diagnostics_directory});
    }
    if (files.empty()) return SupportReadHandle{};
    const auto token = next_token++;
    sources.emplace(token, std::move(files)); return SupportReadHandle{token, true, false};
  }
  Result<SupportRead> Read(const SupportReadHandle& handle, std::size_t maximum, bool events) {
    if (!DirectoriesValid() || !handle.regular || handle.reparse || maximum > 10U * 1024U * 1024U) return RootStorageError("ReadSupportSource");
    auto found = sources.find(handle.identity);
    if (found == sources.end()) return StorageError("ReadSupportSource");
    auto files = std::move(found->second); sources.erase(found);
    SupportRead result{{}, handle.identity, true, false, false};
    std::size_t complete_lines{};
    for (auto& file : files) {
      BY_HANDLE_FILE_INFORMATION before{}, after{};
      if (!GetFileInformationByHandle(file.handle.get(), &before) || !Same(before, file.identity)) return StorageError("SupportIdentityChanged");
      const auto size = (static_cast<std::uint64_t>(before.nFileSizeHigh) << 32U) | before.nFileSizeLow;
      if (!events && size > maximum) return StorageError("SupportSummaryTooLarge");
      const auto available = maximum - result.bytes.size();
      if (available == 0 || (events && complete_lines >= 5)) { result.truncated = true; break; }
      const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(size, available));
      LARGE_INTEGER offset{}; offset.QuadPart = static_cast<LONGLONG>(size - count);
      if (!SetFilePointerEx(file.handle.get(), offset, nullptr, FILE_BEGIN)) return StorageError("SeekSupportSource");
      std::vector<std::byte> bytes(count); DWORD read{};
      if ((count && !ReadFile(file.handle.get(), bytes.data(), static_cast<DWORD>(count), &read, nullptr)) || read != count ||
          !GetFileInformationByHandle(file.handle.get(), &after) || !Same(before, after) || after.nNumberOfLinks != 1) return StorageError("ReadSupportSource");
      NativeHandle path_check = RelativeOpen(file.directory,file.path.filename().wstring(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ,1);
      BY_HANDLE_FILE_INFORMATION named{};
      if (!path_check || !GetFileInformationByHandle(path_check.get(), &named) || !Same(after, named)) return StorageError("SupportIdentityChanged");
      if (events && size > count) {
        result.truncated = true;
        const auto newline = std::find(bytes.begin(), bytes.end(), std::byte{'\n'});
        bytes.erase(bytes.begin(), newline == bytes.end() ? bytes.end() : newline + 1);
      }
      if (events && !bytes.empty() && bytes.back() != std::byte{'\n'}) {
        result.truncated = true;
        const auto newline = std::find(bytes.rbegin(), bytes.rend(), std::byte{'\n'});
        bytes.erase(newline == bytes.rend() ? bytes.begin() : newline.base(), bytes.end());
      }
      complete_lines += static_cast<std::size_t>(std::count(bytes.begin(), bytes.end(), std::byte{'\n'}));
      result.bytes.insert(result.bytes.begin(), bytes.begin(), bytes.end());
    }
    if (!DirectoriesValid()) return RootStorageError("SupportDirectoryChanged");
    return result;
  }
#endif
};

WindowsSupportStorage::WindowsSupportStorage() : state_(std::make_unique<State>()) {
#ifdef WFG_SUPPORT_TESTING
  state_->test_owner = this;
#endif
#ifdef _WIN32
  PWSTR path{};
  constexpr auto folder_flags = KF_FLAG_DONT_VERIFY;
#ifdef WFG_SUPPORT_TESTING
  known_folder_flags = folder_flags;
#endif
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, folder_flags, nullptr, &path))) {
    state_->root = std::filesystem::path(path) / L"WitcherFG";
    CoTaskMemFree(path);
  }
#endif
}
#ifdef WFG_SUPPORT_TESTING
WindowsSupportStorage::WindowsSupportStorage(std::filesystem::path root) : state_(std::make_unique<State>()) { state_->root = std::move(root); state_->test_owner = this; }
Result<Unit> WindowsSupportStorage::ValidateLocalRootForTest() {
#ifdef _WIN32
  if (!state_->LocalVolumeRoot()) return state_->RootStorageError("SupportLocalStorageRequired");
  return Unit{};
#else
  return StorageError("WindowsSupportUnavailable");
#endif
}
#endif
WindowsSupportStorage::~WindowsSupportStorage() = default;
Result<SupportReadHandle> WindowsSupportStorage::OpenSanitizedSummary() {
#ifdef _WIN32
  return state_->Open(false);
#else
  return StorageError("WindowsSupportUnavailable");
#endif
}
Result<SupportReadHandle> WindowsSupportStorage::OpenRecentEvents() {
#ifdef _WIN32
  return state_->Open(true);
#else
  return StorageError("WindowsSupportUnavailable");
#endif
}
Result<SupportRead> WindowsSupportStorage::ReadSanitizedSummary(const SupportReadHandle& handle, std::size_t maximum) {
#ifdef _WIN32
  return state_->Read(handle, maximum, false);
#else
  static_cast<void>(handle); static_cast<void>(maximum); return StorageError("WindowsSupportUnavailable");
#endif
}
Result<SupportRead> WindowsSupportStorage::ReadRecentEvents(const SupportReadHandle& handle, std::size_t maximum) {
#ifdef _WIN32
  return state_->Read(handle, maximum, true);
#else
  static_cast<void>(handle); static_cast<void>(maximum); return StorageError("WindowsSupportUnavailable");
#endif
}
Result<std::filesystem::path> WindowsSupportStorage::CommitArchiveAtomically(std::span<const std::byte> archive) {
#ifdef _WIN32
  state_->completed.clear();
  bool missing{};
  if (archive.empty() || archive.size() > 11U * 1024U * 1024U || !state_->PinChild(L"support", true, missing)) return state_->RootStorageError("PinSupportDirectory");
  GUID id{};
  if (FAILED(CoCreateGuid(&id))) return StorageError("NameSupportArchive");
  wchar_t identifier[40]{};
  if (!StringFromGUID2(id, identifier, 40)) return StorageError("NameSupportArchive");
  const auto stem = std::wstring(L"WitcherFG-support-") + identifier;
  const auto temporary = state_->root / L"support" / (stem + L".partial");
  const auto destination = state_->root / L"support" / (stem + L".zip");
  NativeHandle file = RelativeOpen(state_->support_directory,temporary.filename().wstring(),GENERIC_WRITE | DELETE | FILE_READ_ATTRIBUTES,0,2);
  if (!file) return StorageError("CreateSupportArchive");
  auto fail = [&]() -> Result<std::filesystem::path> {
    FILE_DISPOSITION_INFO disposition{TRUE};
    SetFileInformationByHandle(file.get(), FileDispositionInfo, &disposition, sizeof(disposition));
    return StorageError("CommitSupportArchive");
  };
#ifdef WFG_SUPPORT_TESTING
  if (fail_before_write) return fail();
#endif
  DWORD written{};
  if (!WriteFile(file.get(), archive.data(), static_cast<DWORD>(archive.size()), &written, nullptr) ||
      written != archive.size() || !FlushFileBuffers(file.get())) return fail();
#ifdef WFG_SUPPORT_TESTING
  if (fail_before_rename) return fail();
#endif
  if (!state_->DirectoriesValid()) return fail();
  if(!local_storage::RenameWithin(file.get(),state_->support_directory,destination.filename().wstring(),false))return fail();
  state_->completed = destination;
  return destination;
#else
  static_cast<void>(archive); return StorageError("WindowsSupportUnavailable");
#endif
}
std::filesystem::path WindowsSupportStorage::CompletedArchiveLocation() const { return state_->completed; }
}
