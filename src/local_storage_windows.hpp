#pragma once
// Internal Windows storage primitives. Names are single components; callers retain
// the ancestor pins for every operation. No path-based reopen after provenance.
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>
#define NOMINMAX
#include <windows.h>
#include <utility>
namespace wfg::local_storage {
class NativeHandle {
public:
  NativeHandle() : value_(INVALID_HANDLE_VALUE) {}
  explicit NativeHandle(HANDLE value) : value_(value) {}
  ~NativeHandle() { if (value_ != INVALID_HANDLE_VALUE) CloseHandle(value_); }
  NativeHandle(NativeHandle&& other) noexcept : value_(std::exchange(other.value_, INVALID_HANDLE_VALUE)) {}
  NativeHandle& operator=(NativeHandle&& other) noexcept {
    if (this != &other) { if (value_ != INVALID_HANDLE_VALUE) CloseHandle(value_); value_ = std::exchange(other.value_, INVALID_HANDLE_VALUE); } return *this;
  }
  HANDLE get() const noexcept { return value_; }
  explicit operator bool() const noexcept { return value_ != INVALID_HANDLE_VALUE; }
private: HANDLE value_;
};
class DirectoryLease {
public:
  ~DirectoryLease();
  bool Acquire(HANDLE directory);
private:
  NativeHandle mutex_;
};
bool LocalPathSyntax(const std::filesystem::path& path);
std::optional<std::wstring> ClassifyLocalVolume(const std::filesystem::path&, UINT drive_type, std::wstring_view device);
std::optional<std::wstring> LocalVolumeRoot(const std::filesystem::path&);
std::optional<std::filesystem::path> LocalAppDataCandidate();
NativeHandle RelativeOpen(HANDLE directory, std::wstring_view name, ACCESS_MASK access, ULONG sharing, ULONG disposition, bool is_directory = false);
bool PinDirectories(const std::filesystem::path&, std::wstring_view physical_volume, unsigned create_tail,
                    std::vector<NativeHandle>& pins, bool writer = false);
bool DirectoriesValid(const std::vector<NativeHandle>& pins);
bool RegularSingleLink(HANDLE handle, BY_HANDLE_FILE_INFORMATION* information = nullptr);
bool Same(const BY_HANDLE_FILE_INFORMATION&, const BY_HANDLE_FILE_INFORMATION&);
bool RenameWithin(HANDLE file, HANDLE directory, std::wstring_view name, bool replace);
bool DeleteFileHandle(HANDLE file);
bool WriteBytes(HANDLE file, std::string_view bytes);
}
