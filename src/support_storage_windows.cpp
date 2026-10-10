#include "wfg/support_storage_windows.hpp"

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
class NativeHandle {
public:
  explicit NativeHandle(HANDLE value = INVALID_HANDLE_VALUE) : value_(value) {}
  ~NativeHandle() { if (value_ != INVALID_HANDLE_VALUE) CloseHandle(value_); }
  NativeHandle(NativeHandle&& other) noexcept : value_(std::exchange(other.value_, INVALID_HANDLE_VALUE)) {}
  NativeHandle& operator=(NativeHandle&& other) noexcept {
    if (this != &other) { if (value_ != INVALID_HANDLE_VALUE) CloseHandle(value_); value_ = std::exchange(other.value_, INVALID_HANDLE_VALUE); }
    return *this;
  }
  HANDLE get() const { return value_; }
  explicit operator bool() const { return value_ != INVALID_HANDLE_VALUE; }
private:
  HANDLE value_;
};
bool Same(const BY_HANDLE_FILE_INFORMATION& first, const BY_HANDLE_FILE_INFORMATION& second) {
  return first.dwVolumeSerialNumber == second.dwVolumeSerialNumber && first.nFileIndexHigh == second.nFileIndexHigh &&
         first.nFileIndexLow == second.nFileIndexLow && first.nNumberOfLinks == second.nNumberOfLinks &&
         first.nFileSizeHigh == second.nFileSizeHigh && first.nFileSizeLow == second.nFileSizeLow &&
         first.ftLastWriteTime.dwHighDateTime == second.ftLastWriteTime.dwHighDateTime &&
         first.ftLastWriteTime.dwLowDateTime == second.ftLastWriteTime.dwLowDateTime;
}
struct SourceFile {
  NativeHandle handle;
  BY_HANDLE_FILE_INFORMATION identity{};
  std::filesystem::path path;
  HANDLE directory{};
};
NativeHandle RelativeOpen(HANDLE directory, std::wstring_view name, ACCESS_MASK access, ULONG sharing, ULONG disposition, bool is_directory = false) {
  using OpenFunction = NTSTATUS (NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
  using ErrorFunction = ULONG (WINAPI*)(NTSTATUS);
  const auto module = GetModuleHandleW(L"ntdll.dll");
  const auto open = reinterpret_cast<OpenFunction>(GetProcAddress(module,"NtCreateFile"));
  const auto error = reinterpret_cast<ErrorFunction>(GetProcAddress(module,"RtlNtStatusToDosError"));
  if (!open || !error || name.empty() || name.size() > 120) { SetLastError(ERROR_INVALID_PARAMETER); return NativeHandle{}; }
  UNICODE_STRING text{};
  text.Buffer = const_cast<PWSTR>(name.data());
  text.Length = static_cast<USHORT>(name.size() * sizeof(wchar_t)); text.MaximumLength = text.Length;
  OBJECT_ATTRIBUTES attributes{};
  attributes.Length = sizeof(attributes); attributes.RootDirectory = directory; attributes.ObjectName = &text;
  attributes.Attributes = 0x40U | 0x1000U;
  IO_STATUS_BLOCK status{}; HANDLE handle{};
  const auto result = open(&handle,access | SYNCHRONIZE,&attributes,&status,nullptr,FILE_ATTRIBUTE_NORMAL,sharing,disposition,
      (is_directory ? 0x1U : 0x40U) | 0x20U | 0x00200000U,nullptr,0);
  if (result < 0) { SetLastError(error(result)); return NativeHandle{}; }
  return NativeHandle(handle);
}
#endif
}

struct WindowsSupportStorage::State {
  std::filesystem::path root, completed;
#ifdef _WIN32
  std::vector<NativeHandle> directories;
  std::map<std::uint64_t, std::vector<SourceFile>> sources;
  std::uint64_t next_token{1};
  bool pinned{};
  HANDLE root_directory{INVALID_HANDLE_VALUE}, diagnostics_directory{INVALID_HANDLE_VALUE}, support_directory{INVALID_HANDLE_VALUE};
  bool DirectoriesValid() const {
    for (const auto& handle : directories) {
      BY_HANDLE_FILE_INFORMATION info{};
      if (!GetFileInformationByHandle(handle.get(),&info) || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
          (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) return false;
    }
    return true;
  }
  bool PinRoot() {
    if (pinned) return true;
    if (root.empty() || !root.is_absolute() || root.has_root_name() == false) return false;
    auto current = root.root_path();
    auto pin = [&](const std::filesystem::path& path, bool create) {
      NativeHandle handle = directories.empty() ?
          NativeHandle(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE,
              nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)) :
          RelativeOpen(directories.back().get(),path.filename().wstring(),FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY |
              ((path == root.parent_path() || path == root) ? FILE_ADD_SUBDIRECTORY : 0U),FILE_SHARE_READ | FILE_SHARE_WRITE,create ? 3U : 1U,true);
      BY_HANDLE_FILE_INFORMATION info{};
      if (!handle || !GetFileInformationByHandle(handle.get(), &info) ||
          (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) return false;
      directories.push_back(std::move(handle)); return true;
    };
    if (!pin(current, false)) return false;
    for (const auto& part : root.relative_path()) {
      if (part == L".." || part == L"." || part.empty()) return false;
      current /= part;
      if (!pin(current, current == root)) return false;
    }
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
    if (name == L"diagnostics") diagnostics_directory = handle.get();
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
    if (!PinChild(L"diagnostics", false, missing)) return StorageError("PinDiagnosticsDirectory");
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
    if (!DirectoriesValid() || !handle.regular || handle.reparse || maximum > 10U * 1024U * 1024U) return StorageError("ReadSupportSource");
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
    if (!DirectoriesValid()) return StorageError("SupportDirectoryChanged");
    return result;
  }
#endif
};

WindowsSupportStorage::WindowsSupportStorage() : state_(std::make_unique<State>()) {
#ifdef _WIN32
  PWSTR path{};
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &path))) {
    state_->root = std::filesystem::path(path) / L"WitcherFG";
    CoTaskMemFree(path);
  }
#endif
}
#ifdef WFG_SUPPORT_TESTING
WindowsSupportStorage::WindowsSupportStorage(std::filesystem::path root) : state_(std::make_unique<State>()) { state_->root = std::move(root); }
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
  if (archive.empty() || archive.size() > 11U * 1024U * 1024U || !state_->PinChild(L"support", true, missing)) return StorageError("PinSupportDirectory");
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
  const auto name = destination.filename().wstring();
  std::vector<std::byte> buffer(sizeof(FILE_RENAME_INFO) + name.size() * sizeof(wchar_t));
  auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
  rename->ReplaceIfExists = FALSE; rename->RootDirectory = state_->support_directory;
  rename->FileNameLength = static_cast<DWORD>(name.size() * sizeof(wchar_t));
  std::memcpy(rename->FileName, name.data(), rename->FileNameLength);
  using RenameFunction = NTSTATUS (NTAPI*)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, FILE_INFORMATION_CLASS);
  using ErrorFunction = ULONG (WINAPI*)(NTSTATUS);
  const auto module = GetModuleHandleW(L"ntdll.dll");
  const auto rename_file = reinterpret_cast<RenameFunction>(GetProcAddress(module,"NtSetInformationFile"));
  const auto native_error = reinterpret_cast<ErrorFunction>(GetProcAddress(module,"RtlNtStatusToDosError"));
  if (!rename_file || !native_error) return fail();
  IO_STATUS_BLOCK status{};
  const auto renamed = rename_file(file.get(),&status,rename,static_cast<ULONG>(buffer.size()),static_cast<FILE_INFORMATION_CLASS>(10));
  if (renamed < 0) { SetLastError(native_error(renamed)); return fail(); }
  state_->completed = destination;
  return destination;
#else
  static_cast<void>(archive); return StorageError("WindowsSupportUnavailable");
#endif
}
std::filesystem::path WindowsSupportStorage::CompletedArchiveLocation() const { return state_->completed; }
}
