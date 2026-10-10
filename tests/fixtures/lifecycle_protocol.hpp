#pragma once
#include "lifecycle.hpp"
#include "wfg/compatibility.hpp"
#define NOMINMAX
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>
#include <windows.h>
namespace lifecycle {
constexpr std::uint32_t Magic = 0x57464731;
constexpr DWORD FixtureException = 0xe0424746;
constexpr unsigned ImageSize = 0x6000;
inline constexpr const char *Names[]{"FixtureOne", "FixtureTwo", "FixtureThree", "FixtureFour"};
struct Layout {
  std::uint32_t magic{Magic}, pid{};
  std::uint64_t creation{}, image{}, native{}, replacement{};
  std::array<std::uint64_t, 4> slots{}, originals{}, replacements{};
  std::array<std::uint32_t, 4> protections{};
};
struct Report {
  std::uint32_t magic{Magic}, pid{}, exception_handled{}, debugged{};
  std::array<std::uint64_t, 4> pointers{};
  std::array<std::uint32_t, 4> protections{}, calls{};
};
// Fixed-size cross-process breadcrumbs: no logging, allocation, or locks in an
// armed interval. Publication uses Windows interlocked operations.
struct Shared {
  Layout layout;
  Case which{};
  HANDLE target{}, go{}, quit{}, cancel{}, blocked_read{}, blocked_write{};
  volatile LONG ready{}, held{}, armed{}, writes{}, attaches{}, continues{}, detaches{}, aborts{},
      detached{};
  volatile LONG unsafe{}, forbidden{}, abort_error{}, alive_before_death{}, alive_ack{}, blocked{},
      returned{};
  volatile LONG identity{}, readonly_denied{}, write_denied_error{}, protect_denied_error{},
      storage_refused{}, thread_exited{}, thread_resumed_ack{}, target_reported{},
      observed_exit_rejected{}, readonly_acquired{}, exception_passed{};
  volatile LONG helper_error{}, transaction{}, status{}, write_waiting{}, write_ack{};
  alignas(8) volatile LONG64 requested_at{};
  std::array<std::uint64_t, 4> last_write_values{};
};
inline LONG Load(volatile LONG &v) { return InterlockedCompareExchange(&v, 0, 0); }
inline std::uint64_t Creation(HANDLE h) {
  FILETIME c{}, e{}, k{}, u{};
  if (!GetProcessTimes(h, &c, &e, &k, &u))
    throw std::runtime_error("GetProcessTimes");
  return (std::uint64_t(c.dwHighDateTime) << 32) | c.dwLowDateTime;
}
class Handle {
public:
  HANDLE h{};
  Handle() = default;
  explicit Handle(HANDLE value) : h(value) {}
  ~Handle() {
    if (h && h != INVALID_HANDLE_VALUE)
      CloseHandle(h);
  }
  Handle(const Handle &) = delete;
  Handle &operator=(const Handle &) = delete;
  Handle(Handle &&other) noexcept : h(std::exchange(other.h, nullptr)) {}
  Handle &operator=(Handle &&other) noexcept {
    if (h)
      CloseHandle(h);
    h = std::exchange(other.h, nullptr);
    return *this;
  }
};
inline void Must(bool ok, const char *what) {
  if (!ok)
    throw std::runtime_error(std::string(what) + " native=" + std::to_string(GetLastError()));
}
inline HANDLE Arg(const wchar_t *text) {
  return reinterpret_cast<HANDLE>(_wcstoui64(text, nullptr, 10));
}
inline std::wstring Number(HANDLE h) {
  return std::to_wstring(reinterpret_cast<std::uintptr_t>(h));
}
inline std::filesystem::path Sibling(const wchar_t *name) {
  wchar_t p[32768]{};
  Must(GetModuleFileNameW(nullptr, p, 32768) != 0, "module_path");
  return std::filesystem::path(p).parent_path() / name;
}
inline void Send(HANDLE pipe, const auto &data) {
  DWORD n{};
  Must(WriteFile(pipe, &data, sizeof(data), &n, nullptr) && n == sizeof(data), "pipe_send");
}
inline wfg::Hash256 Hash(const char *text) {
  wfg::Hash256 h{};
  auto nib = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
  for (unsigned i = 0; i < 32; ++i)
    h[i] = std::byte((nib(text[2 * i]) << 4) | nib(text[2 * i + 1]));
  return h;
}
inline wfg::CompatibilityRecord Compatibility() {
  wfg::CompatibilityRecord c{"WfgDisposableFixture", "1", 0x8664, 42, ImageSize,
                             {"fixture", 8192, {}}};
  c.fingerprints = {
      {{0x800, 32, Hash("861009ec4d599fab1f40abc76e6f89880cff5833c79c548c99f9045f191cd90b")},
       {0x840, 32, Hash("cfd2f1fad75a1978da0a444883db7251414b139f31f5a04704c291fdb0e175e6")}}};
  for (unsigned i = 0; i < 4; ++i)
    c.mappings[i] = {"fixture-native.dll", Names[i], "fixture-native.dll", 0x2000 + i * 0x1000};
  c.factory_manager_rva = 0x900;
  c.factory_member_offset = 8;
  return c;
}
inline wfg::RuntimeInventory Inventory(const Layout &l) {
  wfg::RuntimeInventory i;
  i.package_name = "WfgDisposableFixture";
  i.package_version = "1";
  i.executable = {"disposable-fixture.exe", {l.image, 0x8664, 42, ImageSize}};
  i.trusted_system_modules = {{{"fixture-native.dll", {l.native, 0x8664, 42, 0x2000}}, true}};
  // Trust facts are synthetic test inputs; these are allocated fixture pages,
  // never NVIDIA files or a compatibility assertion about the game.
  i.interposer = {{"fixture-replacement.dll", {l.replacement, 0x8664, 42, 0x2000}},
                  "fixture",
                  8192,
                  {},
                  true,
                  true};
  return i;
}
inline wfg::ValidatedBuild Build(const Layout &l) {
  wfg::ValidatedBuild b;
  b.executable_base = l.image;
  b.factory_manager_rva = 0x900;
  b.factory_member_offset = 8;
  for (unsigned i = 0; i < 4; ++i)
    b.mappings[i] = {l.slots[i], l.originals[i], l.replacements[i], 0x2000 + i * 0x1000};
  return b;
}
} // namespace lifecycle
