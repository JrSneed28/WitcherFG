#include "wfg/debug_session.hpp"
#include "wfg/pe.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <limits>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

namespace wfg {
namespace {
Error DebugFailure(std::string_view operation, ErrorCode code = ErrorCode::DebugFailure) {
  return {code, Stage::debug_attach, operation};
}
std::chrono::milliseconds Slice(Deadline deadline, std::chrono::steady_clock::time_point now) {
  return std::clamp(std::chrono::ceil<std::chrono::milliseconds>(deadline.value - now),
    std::chrono::milliseconds(0), std::chrono::milliseconds(50));
}
}

struct DebugSessionState {
  std::atomic<std::uint64_t> held_sequence{};
  std::uint64_t sequence{};
  std::optional<DebugEvent> held;
  Continuation continuation{Continuation::NotHandled};
  std::uint32_t pid{};
  bool attached{};
  bool safe_policy{};
  bool abort_requested{};
  bool process_created{};
  bool attach_breakpoint_consumed{};
  bool observed_exit{};
};

DebugStop::DebugStop(std::shared_ptr<DebugSessionState> state, std::uint64_t sequence, DebugEvent event, Continuation continuation)
    : state_(state), sequence_(sequence), event_(event), continuation_(continuation) {}
DebugStop::DebugStop(DebugStop&& other) noexcept
    : state_(std::move(other.state_)), sequence_(std::exchange(other.sequence_, 0)), event_(other.event_), continuation_(other.continuation_) {}
DebugStop& DebugStop::operator=(DebugStop&& other) noexcept {
  if (this != &other) {
    state_ = std::move(other.state_);
    sequence_ = std::exchange(other.sequence_, 0);
    event_ = other.event_;
    continuation_ = other.continuation_;
  }
  return *this;
}
bool DebugStop::active() const noexcept {
  const auto state = state_.lock();
  return state && sequence_ != 0 && state->held_sequence.load() == sequence_;
}

DebugSession::DebugSession(IDebugBackend& backend, std::stop_token cancellation)
    : backend_(backend), state_(std::make_shared<DebugSessionState>()), owner_(std::this_thread::get_id()), cancellation_(cancellation) {}
DebugSafety DebugSession::Safety() const noexcept {
  if(state_->observed_exit)return DebugSafety::Exited;
  if(!state_->attached)return DebugSafety::NotAttached;
  return state_->safe_policy && !state_->abort_requested ? DebugSafety::AttachedSafe : DebugSafety::AttachedUnverified;
}
bool DebugSession::Owner() const { return owner_ == std::this_thread::get_id(); }
DebugSession::~DebugSession() {
  if (Owner() && state_->attached) {
    try {
      if (state_->safe_policy && !state_->abort_requested) static_cast<void>(Detach());
      else static_cast<void>(AbortOwnedStartup());
    } catch (...) {}
  }
  state_->held_sequence.store(0);
}
Result<Unit> DebugSession::Attach(const OwnedProcess& process) {
  if (!Owner() || state_->attached || state_->abort_requested || !process.native_handle() || process.pid() == 0) return DebugFailure("attach_state");
  if (cancellation_.stop_requested()) return DebugFailure("attach_cancelled", ErrorCode::TimedOut);
  auto attached = backend_.Attach(process);
  if (!IsOk(attached)) return Failure(attached);
  state_->attached = true;
  state_->observed_exit = false;
  state_->pid = process.pid();
  state_->safe_policy = false;
  state_->process_created = false;
  state_->attach_breakpoint_consumed = false;
  auto policy = backend_.SetKillOnExit(false);
  if (!IsOk(policy)) {
    static_cast<void>(AbortOwnedStartup());
    return Failure(policy);
  }
  state_->safe_policy = true;
  return Unit{};
}
Result<DebugStop> DebugSession::WaitStopped(Deadline deadline) {
  if (!Owner() || !state_->attached || state_->abort_requested || state_->held || !state_->safe_policy) return DebugFailure("wait_state");
  while (backend_.Now() < deadline.value && !cancellation_.stop_requested()) {
    auto waited = backend_.Wait(Slice(deadline, backend_.Now()));
    if (!IsOk(waited)) return Failure(waited);
    if (!Value(waited)) continue;
    const auto event = *Value(waited);
    if (event.pid != state_->pid) return DebugFailure("unexpected_debug_target", ErrorCode::IdentityChanged);
    if (event.kind == DebugEventKind::ProcessCreated) state_->process_created = true;
    const bool initial = event.kind == DebugEventKind::Exception && event.exception_status == 0x80000003U &&
      event.first_chance && event.attach_breakpoint_identity && state_->process_created && !state_->attach_breakpoint_consumed;
    // Latch on the handshake itself. Latching on any exception let a live game thread's
    // first-chance exception burn the one-shot before the real attach breakpoint arrived.
    if (initial) state_->attach_breakpoint_consumed = true;
    state_->continuation = event.kind != DebugEventKind::Exception || initial ? Continuation::Handled : Continuation::NotHandled;
    state_->held = event;
    state_->held_sequence.store(++state_->sequence);
    if (event.kind == DebugEventKind::ProcessExited) {
      auto released = ReleaseHeld();
      if (!IsOk(released)) return Failure(released);
      state_->attached = false;
      state_->observed_exit = true;
      return DebugFailure("debug_target_exited", ErrorCode::NoMatchingProcess);
    }
    return DebugStop(state_, state_->sequence, event, state_->continuation);
  }
  return DebugFailure("debug_wait_deadline", ErrorCode::TimedOut);
}
Result<Unit> DebugSession::SetKillOnExit(bool enabled) {
  if (!Owner() || !state_->attached || state_->abort_requested || (enabled && !state_->held)) return DebugFailure("policy_state");
  if (enabled && cancellation_.stop_requested()) return DebugFailure("mutation_cancelled", ErrorCode::TimedOut);
  const auto result = backend_.SetKillOnExit(enabled);
  if (IsOk(result)) state_->safe_policy = !enabled;
  return result;
}
bool DebugSession::Holds(const DebugStop& stop) const noexcept {
  return Owner() && state_->attached && state_->held.has_value() && stop.active() && stop.state_.lock() == state_;
}
Result<Unit> DebugSession::Continue(DebugStop& stop, Continuation continuation) {
  if (!Holds(stop) || state_->abort_requested || !state_->safe_policy || continuation != state_->continuation) return DebugFailure("continue_state");
  return ReleaseHeld();
}
Result<Unit> DebugSession::ReleaseHeld() {
  if (!state_->held) return Unit{};
  const auto result = backend_.Continue(*state_->held, state_->continuation);
  if (IsOk(result)) {
    state_->held.reset();
    state_->held_sequence.store(0);
  }
  return result;
}
Result<Unit> DebugSession::Detach() {
  if (!Owner()) return DebugFailure("detach_owner");
  return Detach({backend_.Now() + std::chrono::seconds(2)});
}
Result<Unit> DebugSession::Detach(Deadline deadline) {
  if (!Owner() || !state_->attached || state_->abort_requested || !state_->safe_policy) return DebugFailure("detach_state");
  if (backend_.Now() >= deadline.value) return Error{ErrorCode::TimedOut, Stage::detach, "debug_detach_deadline"};
  auto released = ReleaseHeld();
  if (!IsOk(released)) return Failure(released);
  while (backend_.Now() < deadline.value) {
    auto detached = backend_.Detach();
    if (IsOk(detached)) { state_->attached = false; return Unit{}; }
    auto waited = backend_.Wait(Slice(deadline, backend_.Now()));
    if (!IsOk(waited)) return Failure(waited);
    if (Value(waited)) {
      const auto event = *Value(waited);
      if (event.pid != state_->pid) return DebugFailure("unexpected_debug_target", ErrorCode::IdentityChanged);
      const bool initial = event.kind == DebugEventKind::Exception && event.exception_status == 0x80000003U && event.first_chance &&
        event.attach_breakpoint_identity && state_->process_created && !state_->attach_breakpoint_consumed;
      if (event.kind == DebugEventKind::ProcessCreated) state_->process_created = true;
      if (initial) state_->attach_breakpoint_consumed = true;
      state_->held = event;
      state_->held_sequence.store(++state_->sequence);
      state_->continuation = event.kind != DebugEventKind::Exception || initial ? Continuation::Handled : Continuation::NotHandled;
      auto continued = ReleaseHeld();
      if (!IsOk(continued)) return Failure(continued);
      if (event.kind == DebugEventKind::ProcessExited) { state_->attached = false; state_->observed_exit = true; return Unit{}; }
    }
  }
  return Error{ErrorCode::TimedOut, Stage::detach, "debug_detach_deadline"};
}
Result<Unit> DebugSession::AbortOwnedStartup() {
  if (!Owner() || !state_->attached) return DebugFailure("abort_state");
  state_->abort_requested = true;
  const auto result = backend_.AbortOwnedStartup();
  if (IsOk(result)) {
    state_->attached = false;
    state_->observed_exit = true;
    state_->held.reset();
    state_->held_sequence.store(0);
  }
  return result;
}

struct NativeDebugBackend::State {
  std::thread::id owner{std::this_thread::get_id()};
  std::uint32_t pid{};
  bool attached{};
  bool safe_policy{};
  bool process_created{};
  bool attach_breakpoint_consumed{};
  bool abort_requested{};
  std::optional<Deadline> abort_deadline;
  std::uint32_t initial_tid{};
  std::optional<DebugEvent> held;
  Address breakpoint{};
  Address remote_breakin{};
#ifdef _WIN32
  HANDLE pin{};
  ~State() { if (pin) CloseHandle(pin); }
#endif
  bool Owner() const { return owner == std::this_thread::get_id(); }
};

#ifdef _WIN32
namespace {
// Claims the calling thread for one debug target. Detach() and a ProcessExited Continue() clear
// it; AbortOwnedStartup() deliberately does NOT. An aborted startup is terminal for the thread:
// after it, Continue, SetKillOnExit, Detach and any fresh Attach all refuse, which is asserted
// by CheckTerminalNoContinuation in tests/fixtures/debug_win32_backend.cpp. The launcher attaches
// once per process, so retaining the claim strands no reusable resource. Do not "fix" this into
// a release without replacing the terminal guard it provides.
thread_local const NativeDebugBackend* active_debugger{};
class DebugFileHandle {
public:
  explicit DebugFileHandle(HANDLE handle) : handle_(handle) {}
  ~DebugFileHandle() { if (handle_ && handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_); }
  DebugFileHandle(const DebugFileHandle&) = delete;
  DebugFileHandle& operator=(const DebugFileHandle&) = delete;
private:
  HANDLE handle_;
};
Error NativeFailure(std::string_view operation, Stage stage = Stage::debug_attach) {
  const auto native = GetLastError();
  return {native == ERROR_ACCESS_DENIED ? ErrorCode::AccessDenied : ErrorCode::DebugFailure, stage, operation, native};
}
class RemoteMemory final : public IMemory {
public:
  explicit RemoteMemory(HANDLE process) : process_(process) {}
  Result<Bytes> Read(Address address, std::size_t size) const override {
    Bytes bytes(size); SIZE_T received{};
    if (!ReadProcessMemory(process_, reinterpret_cast<const void*>(address), bytes.data(), size, &received) || received != size)
      return DebugFailure("debug_read_memory", ErrorCode::UnreadableMemory);
    return bytes;
  }
  Result<MemoryRegion> Query(Address address) const override {
    MEMORY_BASIC_INFORMATION region{};
    if (!VirtualQueryEx(process_, reinterpret_cast<const void*>(address), &region, sizeof(region))) return DebugFailure("debug_query_memory");
    const auto access = region.Protect & 0xFFU;
    const bool executable = access == PAGE_EXECUTE || access == PAGE_EXECUTE_READ || access == PAGE_EXECUTE_READWRITE || access == PAGE_EXECUTE_WRITECOPY;
    const bool readable = access != PAGE_NOACCESS && access != PAGE_EXECUTE && (region.Protect & PAGE_GUARD) == 0;
    return MemoryRegion{reinterpret_cast<Address>(region.BaseAddress), region.RegionSize, region.State == MEM_COMMIT, readable, false, executable, region.Protect};
  }
private:
  HANDLE process_;
};
bool IsSystemNtdll(HANDLE process, Address base, HANDLE file) {
  std::wstring path(32768, L'\0');
  const auto length = file && file != INVALID_HANDLE_VALUE
    ? GetFinalPathNameByHandleW(file, path.data(), static_cast<DWORD>(path.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS)
    : K32GetModuleFileNameExW(process, reinterpret_cast<HMODULE>(base), path.data(), static_cast<DWORD>(path.size()));
  if (length == 0 || length >= path.size()) return false;
  path.resize(length);
  if (path.starts_with(L"\\\\?\\")) path.erase(0, 4);
  std::wstring system(32768, L'\0');
  const auto system_length = GetSystemDirectoryW(system.data(), static_cast<UINT>(system.size()));
  if (system_length == 0 || system_length >= system.size()) return false;
  system.resize(system_length); system += L"\\ntdll.dll";
  return CompareStringOrdinal(path.c_str(), -1, system.c_str(), -1, TRUE) == CSTR_EQUAL;
}
Address TargetExport(const IMemory& memory, Address base, std::string_view name) {
  const auto image = InspectPe(memory, base);
  if (!IsOk(image) || Value(image).machine != 0x8664) return 0;
  const auto entry = FindExport(memory, Value(image), name);
  if (!IsOk(entry) || Value(entry).forwarder) return 0;
  const auto address = base + Value(entry).rva;
  const auto region = memory.Query(address);
  return IsOk(region) && Value(region).committed && Value(region).executable ? address : 0;
}
bool RemoteBreakinThread(DWORD tid, Address expected) {
  if (expected == 0) return false;
  const auto thread = OpenThread(THREAD_QUERY_INFORMATION, FALSE, tid);
  if (!thread) return false;
  using QueryThread = LONG (WINAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
  const auto ntdll = GetModuleHandleW(L"ntdll.dll");
  const auto query = reinterpret_cast<QueryThread>(GetProcAddress(ntdll, "NtQueryInformationThread"));
  void* start{};
  const bool matches = query && query(thread, 9, &start, sizeof(start), nullptr) >= 0 && reinterpret_cast<Address>(start) == expected;
  CloseHandle(thread);
  return matches;
}
}
#endif

NativeDebugBackend::NativeDebugBackend() : state_(std::make_unique<State>()) {}
NativeDebugBackend::~NativeDebugBackend() = default;
std::chrono::steady_clock::time_point NativeDebugBackend::Now() const { return std::chrono::steady_clock::now(); }
Result<Unit> NativeDebugBackend::Attach(const OwnedProcess& process) {
  if (!state_->Owner() || state_->attached || !process.native_handle() || process.pid() == 0) return DebugFailure("native_attach_state");
#ifdef _WIN32
  if (active_debugger) return DebugFailure("native_debug_thread_in_use");
  const auto handle = static_cast<HANDLE>(process.native_handle());
  FILETIME created{}, exited{}, kernel{}, user{};
  if (GetProcessId(handle) != process.pid() || !GetProcessTimes(handle, &created, &exited, &kernel, &user) ||
      ((static_cast<std::uint64_t>(created.dwHighDateTime) << 32U) | created.dwLowDateTime) != process.creation_time() ||
      WaitForSingleObject(handle, 0) != WAIT_TIMEOUT) return DebugFailure("native_attach_identity", ErrorCode::IdentityChanged);
  HANDLE pin{};
  if (!DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(), &pin, 0, FALSE, DUPLICATE_SAME_ACCESS)) return NativeFailure("debug_duplicate_pin");
  if (!DebugActiveProcess(process.pid())) { auto error = NativeFailure("DebugActiveProcess"); CloseHandle(pin); return error; }
  active_debugger = this;
  if (state_->pin) CloseHandle(state_->pin);
  state_->pin = pin;
  state_->pid = process.pid(); state_->attached = true; state_->safe_policy = false;
  state_->process_created = false; state_->attach_breakpoint_consumed = false; state_->abort_requested = false;
  state_->abort_deadline.reset();
  state_->initial_tid = 0; state_->breakpoint = 0; state_->remote_breakin = 0;
  return Unit{};
#else
  return DebugFailure("native_debug_unavailable");
#endif
}
Result<Unit> NativeDebugBackend::SetKillOnExit(bool enabled) {
  if (!state_->Owner() || !state_->attached || state_->abort_requested || (enabled && !state_->held)) return DebugFailure("native_policy_state");
#ifdef _WIN32
  if (!DebugSetProcessKillOnExit(enabled ? TRUE : FALSE)) return NativeFailure("DebugSetProcessKillOnExit");
  state_->safe_policy = !enabled;
  return Unit{};
#else
  return DebugFailure("native_debug_unavailable");
#endif
}
Result<std::optional<DebugEvent>> NativeDebugBackend::Wait(std::chrono::milliseconds slice) {
  if (!state_->Owner() || !state_->attached || state_->abort_requested || state_->held || !state_->safe_policy ||
      slice.count() < 0 || slice > std::chrono::milliseconds(50)) return DebugFailure("native_wait_state");
#ifdef _WIN32
  DEBUG_EVENT native{};
  if (!WaitForDebugEvent(&native, static_cast<DWORD>(slice.count()))) {
    if (GetLastError() == ERROR_SEM_TIMEOUT) return std::optional<DebugEvent>{};
    return NativeFailure("WaitForDebugEvent");
  }
  DebugEvent event{DebugEventKind::Other, native.dwProcessId, native.dwThreadId};
  if (native.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT) {
    event.kind = DebugEventKind::ProcessCreated;
    if (event.pid == state_->pid) { state_->process_created = true; state_->initial_tid = event.tid; }
    if (native.u.CreateProcessInfo.hFile) CloseHandle(native.u.CreateProcessInfo.hFile);
  } else if (native.dwDebugEventCode == CREATE_THREAD_DEBUG_EVENT) {
    event.kind = DebugEventKind::ThreadCreated;
  } else if (native.dwDebugEventCode == LOAD_DLL_DEBUG_EVENT) {
    event.kind = DebugEventKind::DllLoaded;
    const auto file = native.u.LoadDll.hFile;
    const DebugFileHandle file_owner(file);
    const auto base = reinterpret_cast<Address>(native.u.LoadDll.lpBaseOfDll);
    if (native.dwProcessId == state_->pid && IsSystemNtdll(state_->pin, base, file)) {
      RemoteMemory memory(state_->pin);
      state_->breakpoint = TargetExport(memory, base, "DbgBreakPoint");
      state_->remote_breakin = TargetExport(memory, base, "DbgUiRemoteBreakin");
    }
  } else if (native.dwDebugEventCode == EXCEPTION_DEBUG_EVENT) {
    event.kind = DebugEventKind::Exception;
    event.exception_status = native.u.Exception.ExceptionRecord.ExceptionCode;
    event.exception_address = reinterpret_cast<Address>(native.u.Exception.ExceptionRecord.ExceptionAddress);
    event.first_chance = native.u.Exception.dwFirstChance != 0;
    event.attach_breakpoint_identity = event.pid == state_->pid && state_->process_created && !state_->attach_breakpoint_consumed &&
      event.exception_status == EXCEPTION_BREAKPOINT && event.first_chance && state_->breakpoint != 0 &&
      event.exception_address == state_->breakpoint && (event.tid == state_->initial_tid || RemoteBreakinThread(event.tid, state_->remote_breakin));
    // One-shot, but consumed by the handshake rather than by whichever exception arrives first.
    if (event.attach_breakpoint_identity) state_->attach_breakpoint_consumed = true;
  } else if (native.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT) {
    event.kind = DebugEventKind::ProcessExited;
  }
  state_->held = event;
  return std::optional<DebugEvent>{event};
#else
  return DebugFailure("native_debug_unavailable");
#endif
}
Result<Unit> NativeDebugBackend::Continue(const DebugEvent& event, Continuation continuation) {
  if (!state_->Owner() || !state_->attached || state_->abort_requested || !state_->held || !state_->safe_policy ||
      event.pid != state_->pid || event.tid != state_->held->tid ||
      event.kind != state_->held->kind || event.exception_status != state_->held->exception_status ||
      event.exception_address != state_->held->exception_address ||
      (event.kind == DebugEventKind::Exception && continuation == Continuation::Handled && !state_->held->attach_breakpoint_identity))
    return DebugFailure("native_continue_state");
#ifdef _WIN32
  if (!ContinueDebugEvent(event.pid, event.tid, continuation == Continuation::Handled ? DBG_CONTINUE : DBG_EXCEPTION_NOT_HANDLED)) return NativeFailure("ContinueDebugEvent");
  state_->held.reset();
  if (event.kind == DebugEventKind::ProcessExited) {
    state_->attached = false;
    active_debugger = nullptr;
  }
  return Unit{};
#else
  return DebugFailure("native_debug_unavailable");
#endif
}
Result<Unit> NativeDebugBackend::Detach() {
  if (!state_->Owner() || !state_->attached || state_->abort_requested || state_->held || !state_->safe_policy) return DebugFailure("native_detach_state");
#ifdef _WIN32
  if (!DebugActiveProcessStop(state_->pid)) return NativeFailure("DebugActiveProcessStop", Stage::detach);
  state_->attached = false;
  active_debugger = nullptr;
  return Unit{};
#else
  return DebugFailure("native_debug_unavailable");
#endif
}
Result<Unit> NativeDebugBackend::AbortOwnedStartup() {
  if (!state_->Owner() || !state_->attached) return DebugFailure("native_abort_state");
#ifdef _WIN32
  state_->abort_requested = true;
  if (!state_->abort_deadline) state_->abort_deadline = Deadline{Now() + std::chrono::seconds(2)};
  const auto cleanup = *state_->abort_deadline;
  if (Now() >= cleanup.value) return DebugFailure("abort_owned_exit_deadline", ErrorCode::TimedOut);
  const bool accepted = TerminateProcess(state_->pin, ERROR_PROCESS_ABORTED) != FALSE;
  const auto termination_error = accepted ? Error{} : NativeFailure("abort_owned_startup");
  const auto observed = WaitForSingleObject(state_->pin, 0);
  if (observed == WAIT_OBJECT_0) { state_->attached = false; return Unit{}; }
  if (!accepted) return termination_error;
  if (observed != WAIT_TIMEOUT) return NativeFailure("abort_owned_exit_wait");
  while (Now() < cleanup.value) {
    const auto signaled = WaitForSingleObject(state_->pin, static_cast<DWORD>(Slice(cleanup, Now()).count()));
    if (signaled == WAIT_OBJECT_0) { state_->attached = false; return Unit{}; }
    if (signaled != WAIT_TIMEOUT) return NativeFailure("abort_owned_exit_wait");
  }
  return DebugFailure("abort_owned_exit_deadline", ErrorCode::TimedOut);
#else
  return DebugFailure("native_debug_unavailable");
#endif
}

namespace {
class NativeWatchdogBackend final : public IWatchdogBackend {
public:
  bool WaitUntil(Deadline deadline, std::stop_token disarmed) override {
    std::unique_lock lock(mutex_);
    condition_.wait_until(lock, disarmed, deadline.value, [] { return false; });
    return !disarmed.stop_requested();
  }
  void ExitHelper() noexcept override {
#ifdef _WIN32
    TerminateProcess(GetCurrentProcess(), ERROR_TIMEOUT);
#endif
    std::_Exit(124);
  }
private:
  std::mutex mutex_;
  std::condition_variable_any condition_;
};
IWatchdogBackend& SystemWatchdog() {
  static NativeWatchdogBackend backend;
  return backend;
}
}
struct DeadlineWatchdog::State {
  using Clock=std::chrono::steady_clock;
  static constexpr auto Unset=std::numeric_limits<Clock::rep>::max();
  std::stop_source wake;
  std::atomic<Clock::rep> requested_at{Unset};
  struct Cancel {
    State* state;
    void operator()()const noexcept {
      state->requested_at.store(Clock::now().time_since_epoch().count());
      state->wake.request_stop();
    }
  };
  std::stop_callback<Cancel> on_cancel;
  explicit State(std::stop_token external):on_cancel(external,Cancel{this}){}
};
DeadlineWatchdog::DeadlineWatchdog(Deadline deadline, std::stop_source& cancellation)
    : DeadlineWatchdog(deadline,cancellation,SystemWatchdog(),{}) {}
DeadlineWatchdog::DeadlineWatchdog(Deadline deadline, std::stop_source& cancellation,IWatchdogBackend& backend)
    : DeadlineWatchdog(deadline,cancellation,backend,{}) {}
DeadlineWatchdog::DeadlineWatchdog(Deadline deadline, std::stop_source& cancellation, std::stop_token external)
    : DeadlineWatchdog(deadline, cancellation, SystemWatchdog(), external) {}
DeadlineWatchdog::DeadlineWatchdog(Deadline deadline, std::stop_source& cancellation, IWatchdogBackend& backend, std::stop_token external)
    : state_(std::make_unique<State>(external)),
      thread_([deadline, &cancellation, &backend, state=state_.get()](std::stop_token disarmed) {
        // The stop state and external callback are fully constructed on the
        // caller before this worker can overlap an unsafe transaction interval.
        std::stop_callback on_disarm(disarmed,[&] { state->wake.request_stop(); });
        const bool expired=backend.WaitUntil(deadline,state->wake.get_token());
        const auto request=state->requested_at.load();
        if(disarmed.stop_requested() || (!expired && request==State::Unset))return;
        auto cleanup=deadline.value+std::chrono::seconds(2);
        if(request!=State::Unset)cleanup=std::min(cleanup,State::Clock::time_point(State::Clock::duration(request))+std::chrono::seconds(2));
        cancellation.request_stop();
        if(backend.WaitUntil({cleanup},disarmed) && !disarmed.stop_requested())backend.ExitHelper();
      }) {}
DeadlineWatchdog::~DeadlineWatchdog() { Disarm(); }
void DeadlineWatchdog::Disarm() {
  thread_.request_stop();
  if (thread_.joinable()) thread_.join();
}

}
