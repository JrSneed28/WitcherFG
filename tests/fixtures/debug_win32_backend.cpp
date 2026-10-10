#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>

#include "support.hpp"
#include "fixtures/verified_identity.hpp"
#include "fixtures/pe_images.hpp"
#include "wfg/debug_session.hpp"

#include <deque>

namespace DebugWin32Fixture {
inline HANDLE Handle(std::uintptr_t value) { return reinterpret_cast<HANDLE>(value); }
struct Model {
  std::deque<DEBUG_EVENT> events;
  std::vector<HANDLE> closed;
  std::vector<std::string> calls;
  bool denied{};
  bool fail_policy{};
  wfg::Bytes ntdll;
  bool system_path{true};
  wfg::Address thread_start{};
  DWORD last_event{};
  bool exited{};
  bool fail_exit_signal{};
  bool kill{true};
  bool fail_terminate{};
  bool signal_on_terminate{};
  unsigned positive_pin_waits{};
};
Model model;
BOOL Close(HANDLE handle) { model.closed.push_back(handle); return TRUE; }
DWORD Pid(HANDLE) { return 42; }
BOOL Times(HANDLE, FILETIME* created, FILETIME*, FILETIME*, FILETIME*) { *created = FILETIME{100, 0}; return TRUE; }
DWORD Signal(HANDLE handle, DWORD duration) {
  CHECK(duration <= 50);
  if (handle == Handle(101) && duration > 0) ++model.positive_pin_waits;
  if (handle == Handle(101) && model.exited) {
    if (model.fail_exit_signal) { SetLastError(ERROR_INVALID_HANDLE); return WAIT_FAILED; }
    return WAIT_OBJECT_0;
  }
  if (handle == Handle(101) && duration > 0) std::this_thread::sleep_for(std::chrono::milliseconds(duration));
  return WAIT_TIMEOUT;
}
BOOL Duplicate(HANDLE, HANDLE, HANDLE, HANDLE* pin, DWORD, BOOL, DWORD) { *pin = Handle(101); return TRUE; }
BOOL Attach(DWORD pid) {
  CHECK(pid == 42); model.calls.push_back("attach");
  if (model.denied) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; } return TRUE;
}
BOOL Policy(BOOL kill) { model.calls.push_back(kill ? "true" : "false"); if (model.fail_policy) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; } model.kill = kill != FALSE; return TRUE; }
BOOL Wait(DEBUG_EVENT* event, DWORD duration) {
  CHECK(duration <= 50); model.calls.push_back("wait");
  if (model.events.empty()) { SetLastError(ERROR_SEM_TIMEOUT); return FALSE; }
  *event = model.events.front(); model.events.pop_front(); model.last_event = event->dwDebugEventCode; return TRUE;
}
BOOL Continue(DWORD pid, DWORD, DWORD) { CHECK(pid == 42); model.calls.push_back("continue"); if (model.last_event == EXIT_PROCESS_DEBUG_EVENT) model.exited = true; return TRUE; }
BOOL Detach(DWORD pid) { CHECK(pid == 42); model.calls.push_back("detach"); return TRUE; }
BOOL Terminate(HANDLE handle, UINT) {
  CHECK(handle == Handle(101)); model.calls.push_back("abort_pin");
  if (model.fail_terminate) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
  if (model.signal_on_terminate) model.exited = true;
  return TRUE;
}
constexpr wfg::Address ntdll_base = 0x00007FF700000000ULL;
DWORD FinalPath(HANDLE, wchar_t* buffer, DWORD capacity, DWORD) {
  const std::wstring path = model.system_path ? L"\\\\?\\C:\\Windows\\System32\\ntdll.dll" : L"\\\\?\\C:\\Other\\ntdll.dll";
  if (model.ntdll.empty() || capacity <= path.size()) return 0;
  std::copy(path.begin(), path.end(), buffer); buffer[path.size()] = L'\0'; return static_cast<DWORD>(path.size());
}
UINT SystemPath(wchar_t* buffer, UINT) {
  constexpr wchar_t path[] = L"C:\\Windows\\System32";
  std::copy(std::begin(path), std::end(path), buffer); return static_cast<UINT>(std::size(path) - 1);
}
DWORD ModulePath(HANDLE, HMODULE, wchar_t* buffer, DWORD capacity) {
  const std::wstring path = model.system_path ? L"C:\\Windows\\System32\\ntdll.dll" : L"C:\\Other\\ntdll.dll";
  if (capacity <= path.size()) return 0;
  std::copy(path.begin(), path.end(), buffer); buffer[path.size()] = L'\0'; return static_cast<DWORD>(path.size());
}
BOOL Read(HANDLE, const void* address, void* buffer, SIZE_T size, SIZE_T* received) {
  const auto location = reinterpret_cast<wfg::Address>(address);
  if (location < ntdll_base || size > model.ntdll.size() || location - ntdll_base > model.ntdll.size() - size) return FALSE;
  std::memcpy(buffer, model.ntdll.data() + static_cast<std::size_t>(location - ntdll_base), size); *received = size; return TRUE;
}
SIZE_T Query(HANDLE, const void*, MEMORY_BASIC_INFORMATION* region, SIZE_T) {
  region->BaseAddress = reinterpret_cast<void*>(ntdll_base); region->RegionSize = 0x1000;
  region->State = MEM_COMMIT; region->Protect = PAGE_EXECUTE_READ; return sizeof(*region);
}
HANDLE Thread(DWORD, BOOL, DWORD) { return Handle(301); }
LONG WINAPI QueryThread(HANDLE, ULONG, void* buffer, ULONG, ULONG*) {
  *static_cast<void**>(buffer) = reinterpret_cast<void*>(model.thread_start); return 0;
}
FARPROC Procedure(HMODULE, const char*) { return reinterpret_cast<FARPROC>(QueryThread); }
void Ntdll() {
  auto image = FixturePe::ExportImage("DbgBreakPoint", 0x600, {}, 1, ntdll_base);
  model.ntdll = wfg::Value(image.memory.Read(ntdll_base, 0x1000));
  FixturePe::Put<std::uint32_t>(model.ntdll, 0x214, 2);
  FixturePe::Put<std::uint32_t>(model.ntdll, 0x218, 2);
  FixturePe::Put<std::uint32_t>(model.ntdll, 0x284, 0x700);
  FixturePe::Put<std::uint32_t>(model.ntdll, 0x294, 0x2D0);
  FixturePe::Put<std::uint16_t>(model.ntdll, 0x2A2, 1);
  FixturePe::Text(model.ntdll, 0x2D0, "DbgUiRemoteBreakin");
  model.ntdll[0x600] = std::byte{0xCC};
}
void Ignore(void*) noexcept {}
wfg::OwnedProcess Owned() { return wfg::VerifiedIdentityTestFactory::Owned(Handle(100), Ignore); }
DEBUG_EVENT Event(DWORD kind) { DEBUG_EVENT event{}; event.dwDebugEventCode = kind; event.dwProcessId = 42; event.dwThreadId = 7; return event; }
}

#define CloseHandle DebugWin32Fixture::Close
#define GetProcessId DebugWin32Fixture::Pid
#define GetProcessTimes DebugWin32Fixture::Times
#define WaitForSingleObject DebugWin32Fixture::Signal
#define DuplicateHandle DebugWin32Fixture::Duplicate
#define DebugActiveProcess DebugWin32Fixture::Attach
#define DebugSetProcessKillOnExit DebugWin32Fixture::Policy
#define WaitForDebugEvent DebugWin32Fixture::Wait
#define ContinueDebugEvent DebugWin32Fixture::Continue
#define DebugActiveProcessStop DebugWin32Fixture::Detach
#define TerminateProcess DebugWin32Fixture::Terminate
#define GetFinalPathNameByHandleW DebugWin32Fixture::FinalPath
#define GetSystemDirectoryW DebugWin32Fixture::SystemPath
#define K32GetModuleFileNameExW DebugWin32Fixture::ModulePath
#define ReadProcessMemory DebugWin32Fixture::Read
#define VirtualQueryEx DebugWin32Fixture::Query
#define OpenThread DebugWin32Fixture::Thread
#define GetProcAddress DebugWin32Fixture::Procedure
#include "../../src/debug_session.cpp"
#undef CloseHandle
#undef GetProcessId
#undef GetProcessTimes
#undef WaitForSingleObject
#undef DuplicateHandle
#undef DebugActiveProcess
#undef DebugSetProcessKillOnExit
#undef WaitForDebugEvent
#undef ContinueDebugEvent
#undef DebugActiveProcessStop
#undef TerminateProcess
#undef GetFinalPathNameByHandleW
#undef GetSystemDirectoryW
#undef K32GetModuleFileNameExW
#undef ReadProcessMemory
#undef VirtualQueryEx
#undef OpenThread
#undef GetProcAddress

namespace {
using namespace DebugWin32Fixture;
using namespace wfg;
DebugEvent Waited(NativeDebugBackend& backend) {
  auto result = backend.Wait(std::chrono::milliseconds(50)); CHECK(IsOk(result)); CHECK(Value(result).has_value()); return *Value(result);
}
}
TEST("debug.native_event_handles_files_only_and_duplicate_exactly_once") {
  model = {};
  {
    NativeDebugBackend backend; auto owned = Owned(); REQUIRE_OK(backend.Attach(owned)); REQUIRE_OK(backend.SetKillOnExit(false));
    auto created = Event(CREATE_PROCESS_DEBUG_EVENT); created.u.CreateProcessInfo.hProcess = Handle(201);
    created.u.CreateProcessInfo.hThread = Handle(202); created.u.CreateProcessInfo.hFile = Handle(203); model.events.push_back(created);
    auto stopped = Waited(backend); CHECK((model.closed == std::vector<HANDLE>{Handle(203)})); REQUIRE_OK(backend.Continue(stopped, Continuation::Handled));
    auto thread = Event(CREATE_THREAD_DEBUG_EVENT); thread.u.CreateThread.hThread = Handle(204); model.events.push_back(thread);
    stopped = Waited(backend); REQUIRE_OK(backend.Continue(stopped, Continuation::Handled));
    auto dll = Event(LOAD_DLL_DEBUG_EVENT); dll.u.LoadDll.hFile = Handle(205); model.events.push_back(dll);
    stopped = Waited(backend); REQUIRE_OK(backend.Continue(stopped, Continuation::Handled));
    CHECK((model.closed == std::vector<HANDLE>{Handle(203), Handle(205)}));
    model.events.push_back(Event(EXIT_THREAD_DEBUG_EVENT)); stopped = Waited(backend); REQUIRE_OK(backend.Continue(stopped, Continuation::Handled));
    model.events.push_back(Event(EXIT_PROCESS_DEBUG_EVENT)); stopped = Waited(backend); REQUIRE_OK(backend.Continue(stopped, Continuation::Handled));
    CHECK((model.closed == std::vector<HANDLE>{Handle(203), Handle(205)}));
  }
  CHECK((model.closed == std::vector<HANDLE>{Handle(203), Handle(205), Handle(101)}));
}
TEST("debug.native_attach_denial_closes_only_independent_pin") {
  model = {}; model.denied = true;
  NativeDebugBackend backend; auto owned = Owned(); REQUIRE_ERROR(backend.Attach(owned), ErrorCode::AccessDenied);
  CHECK((model.closed == std::vector<HANDLE>{Handle(101)})); CHECK((model.calls == std::vector<std::string>{"attach"}));
}
TEST("debug.native_direct_wait_requires_first_false_policy") {
  model = {}; NativeDebugBackend backend; auto owned = Owned(); REQUIRE_OK(backend.Attach(owned));
  REQUIRE_ERROR(backend.Wait(std::chrono::milliseconds(50)), ErrorCode::DebugFailure);
  CHECK((model.calls == std::vector<std::string>{"attach"}));
  REQUIRE_OK(backend.SetKillOnExit(false)); REQUIRE_OK(backend.Detach());
}
TEST("debug.native_direct_resume_refuses_unsafe_mutation_policy") {
  model = {}; NativeDebugBackend backend; auto owned = Owned(); REQUIRE_OK(backend.Attach(owned)); REQUIRE_OK(backend.SetKillOnExit(false));
  model.events.push_back(Event(CREATE_PROCESS_DEBUG_EVENT)); auto stopped = Waited(backend); REQUIRE_OK(backend.SetKillOnExit(true));
  REQUIRE_ERROR(backend.Continue(stopped, Continuation::Handled), ErrorCode::DebugFailure);
  REQUIRE_OK(backend.SetKillOnExit(false)); REQUIRE_OK(backend.Continue(stopped, Continuation::Handled)); REQUIRE_OK(backend.Detach());
}
namespace {
void OnOwnerThread(auto action) {
  std::exception_ptr failure;
  std::thread owner([&] { try { action(); } catch (...) { failure = std::current_exception(); } });
  owner.join(); if (failure) std::rethrow_exception(failure);
}
DebugEvent BeginUnsafeAbort(NativeDebugBackend& backend) {
  auto owned = Owned(); REQUIRE_OK(backend.Attach(owned)); REQUIRE_OK(backend.SetKillOnExit(false));
  model.events.push_back(Event(CREATE_PROCESS_DEBUG_EVENT)); auto stopped = Waited(backend);
  REQUIRE_OK(backend.SetKillOnExit(true)); model.events.push_back(Event(EXIT_PROCESS_DEBUG_EVENT)); return stopped;
}
void CheckTerminalNoContinuation(NativeDebugBackend& backend, const DebugEvent& stopped) {
  CHECK(model.kill); CHECK(std::count(model.calls.begin(), model.calls.end(), "continue") == 0);
  CHECK(std::count(model.calls.begin(), model.calls.end(), "false") == 1); CHECK(model.events.size() == 1);
  REQUIRE_ERROR(backend.Continue(stopped, Continuation::Handled), ErrorCode::DebugFailure);
  REQUIRE_ERROR(backend.SetKillOnExit(false), ErrorCode::DebugFailure);
  REQUIRE_ERROR(backend.Detach(), ErrorCode::DebugFailure);
  NativeDebugBackend second; auto owned = Owned(); REQUIRE_ERROR(second.Attach(owned), ErrorCode::DebugFailure);
}
}
TEST("debug.native_verified_abort_observes_pin_without_continuing_or_closing_os_handles") {
  model = {};
  model.signal_on_terminate = true;
  OnOwnerThread([&] {
    NativeDebugBackend backend; const auto stopped = BeginUnsafeAbort(backend); REQUIRE_OK(backend.AbortOwnedStartup());
    CHECK(model.closed.empty()); CHECK(std::find(model.calls.begin(), model.calls.end(), "abort_pin") != model.calls.end());
    CheckTerminalNoContinuation(backend, stopped);
  });
  CHECK((model.closed == std::vector<HANDLE>{Handle(101)}));
}
namespace {
void InitialEvents(NativeDebugBackend& backend, bool file = true) {
  model.events.push_back(Event(CREATE_PROCESS_DEBUG_EVENT)); auto created = Waited(backend); REQUIRE_OK(backend.Continue(created, Continuation::Handled));
  auto dll = Event(LOAD_DLL_DEBUG_EVENT); dll.u.LoadDll.hFile = file ? Handle(205) : nullptr;
  dll.u.LoadDll.lpBaseOfDll = reinterpret_cast<void*>(ntdll_base); model.events.push_back(dll);
  auto loaded = Waited(backend); REQUIRE_OK(backend.Continue(loaded, Continuation::Handled));
}
DEBUG_EVENT Breakpoint(wfg::Address address = ntdll_base + 0x600, DWORD tid = 7) {
  auto event = Event(EXCEPTION_DEBUG_EVENT); event.dwThreadId = tid;
  event.u.Exception.ExceptionRecord.ExceptionCode = EXCEPTION_BREAKPOINT;
  event.u.Exception.ExceptionRecord.ExceptionAddress = reinterpret_cast<void*>(address);
  event.u.Exception.dwFirstChance = 1; return event;
}
}
TEST("debug.native_identifies_target_export_not_local_address_and_only_once") {
  model = {}; Ntdll(); NativeDebugBackend backend; auto owned = Owned();
  REQUIRE_OK(backend.Attach(owned)); REQUIRE_OK(backend.SetKillOnExit(false)); InitialEvents(backend);
  model.events.push_back(Breakpoint()); auto initial = Waited(backend); CHECK(initial.attach_breakpoint_identity);
  REQUIRE_OK(backend.Continue(initial, Continuation::Handled));
  model.events.push_back(Breakpoint()); auto later = Waited(backend); CHECK(!later.attach_breakpoint_identity);
  REQUIRE_ERROR(backend.Continue(later, Continuation::Handled), ErrorCode::DebugFailure);
  REQUIRE_OK(backend.Continue(later, Continuation::NotHandled)); REQUIRE_OK(backend.Detach());
}
TEST("debug.native_missing_dll_file_uses_verified_remote_module_path") {
  model = {}; Ntdll(); NativeDebugBackend backend; auto owned = Owned();
  REQUIRE_OK(backend.Attach(owned)); REQUIRE_OK(backend.SetKillOnExit(false)); InitialEvents(backend, false);
  model.events.push_back(Breakpoint()); auto initial = Waited(backend); CHECK(initial.attach_breakpoint_identity);
  REQUIRE_OK(backend.Continue(initial, Continuation::Handled)); REQUIRE_OK(backend.Detach());
}
TEST("debug.native_rejects_wrong_export_thread_chance_path_and_late_breakpoint") {
  for (int variant = 0; variant < 5; ++variant) {
    model = {}; Ntdll(); if (variant == 3) model.system_path = false;
    NativeDebugBackend backend; auto owned = Owned(); REQUIRE_OK(backend.Attach(owned)); REQUIRE_OK(backend.SetKillOnExit(false)); InitialEvents(backend);
    auto event = Breakpoint(variant == 0 ? ntdll_base + 0x601 : ntdll_base + 0x600, variant == 1 ? 8U : 7U);
    if (variant == 2) event.u.Exception.dwFirstChance = 0;
    if (variant == 4) { model.events.push_back(Breakpoint(ntdll_base + 0x601)); auto earlier = Waited(backend); REQUIRE_OK(backend.Continue(earlier, Continuation::NotHandled)); }
    model.events.push_back(event); auto stop = Waited(backend); CHECK(!stop.attach_breakpoint_identity);
    REQUIRE_ERROR(backend.Continue(stop, Continuation::Handled), ErrorCode::DebugFailure);
    REQUIRE_OK(backend.Continue(stop, Continuation::NotHandled)); REQUIRE_OK(backend.Detach());
  }
}
TEST("debug.native_identifies_remote_breakin_thread_using_target_export") {
  model = {}; Ntdll(); model.thread_start = ntdll_base + 0x700;
  NativeDebugBackend backend; auto owned = Owned(); REQUIRE_OK(backend.Attach(owned)); REQUIRE_OK(backend.SetKillOnExit(false)); InitialEvents(backend);
  model.events.push_back(Breakpoint(ntdll_base + 0x600, 8)); auto initial = Waited(backend); CHECK(initial.attach_breakpoint_identity);
  CHECK(std::find(model.closed.begin(), model.closed.end(), Handle(301)) != model.closed.end());
  REQUIRE_OK(backend.Continue(initial, Continuation::Handled)); REQUIRE_OK(backend.Detach());
}
TEST("debug.native_abort_requires_pinned_exit_signal_not_terminate_return") {
  model = {}; model.fail_exit_signal = true;
  model.signal_on_terminate = true;
  OnOwnerThread([&] {
    NativeDebugBackend backend; const auto stopped = BeginUnsafeAbort(backend);
    REQUIRE_ERROR(backend.AbortOwnedStartup(), ErrorCode::DebugFailure);
    CheckTerminalNoContinuation(backend, stopped);
  });
}
TEST("debug.native_only_one_target_per_debug_thread_even_with_two_backends") {
  model = {}; NativeDebugBackend first, second; auto owned = Owned();
  REQUIRE_OK(first.Attach(owned)); REQUIRE_OK(first.SetKillOnExit(false));
  const auto before = model.calls.size(); REQUIRE_ERROR(second.Attach(owned), ErrorCode::DebugFailure);
  CHECK(model.calls.size() == before); REQUIRE_OK(first.Detach());
  REQUIRE_OK(second.Attach(owned)); REQUIRE_OK(second.SetKillOnExit(false)); REQUIRE_OK(second.Detach());
}
TEST("debug.native_failed_termination_nonsignaled_pin_keeps_original_stop_true_and_guard") {
  model = {}; model.fail_terminate = true;
  OnOwnerThread([&] {
    NativeDebugBackend backend; const auto stopped = BeginUnsafeAbort(backend);
    REQUIRE_ERROR(backend.AbortOwnedStartup(), ErrorCode::AccessDenied);
    CHECK(model.positive_pin_waits == 0); CheckTerminalNoContinuation(backend, stopped);
  });
}
TEST("debug.native_failed_termination_already_signaled_pin_is_verified_without_continue") {
  model = {}; model.fail_terminate = true;
  OnOwnerThread([&] {
    NativeDebugBackend backend; const auto stopped = BeginUnsafeAbort(backend); model.exited = true;
    REQUIRE_OK(backend.AbortOwnedStartup()); CheckTerminalNoContinuation(backend, stopped);
  });
}
TEST("debug.native_accepted_termination_nonsignaled_pin_times_out_without_releasing_event") {
  model = {};
  OnOwnerThread([&] {
    NativeDebugBackend backend; const auto stopped = BeginUnsafeAbort(backend); const auto began = backend.Now();
    REQUIRE_ERROR(backend.AbortOwnedStartup(), ErrorCode::TimedOut);
    CHECK(model.positive_pin_waits > 0); CheckTerminalNoContinuation(backend, stopped);
    CHECK(backend.Now() - began < std::chrono::seconds(3));
    const auto waits = model.positive_pin_waits; REQUIRE_ERROR(backend.AbortOwnedStartup(), ErrorCode::TimedOut);
    CHECK(model.positive_pin_waits == waits);
  });
}
