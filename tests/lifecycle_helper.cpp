#include "fixtures/lifecycle_protocol.hpp"
#include "fixtures/verified_identity.hpp"
#include "wfg/launcher.hpp"
#include "wfg/support_storage_windows.hpp"
#include "wfg/windows_process.hpp"
#include <cstdio>
#include <malloc.h>
#include <new>
#include <thread>
using namespace wfg;
using namespace lifecycle;
namespace {
Shared *shared{};
Case Which() { return shared->which; }
bool Is(Case c) { return Which() == c; }
Error Fault(const char *operation) {
  return {ErrorCode::MutationFailure, Stage::import_transaction, operation, ERROR_WRITE_FAULT};
}
void Quiet() {
  if (shared && Load(shared->unsafe))
    InterlockedIncrement(&shared->forbidden);
}
[[noreturn]] void Die(unsigned code) {
  TerminateProcess(GetCurrentProcess(), code);
  std::_Exit(static_cast<int>(code));
}
void Block(bool read, std::string_view payload = {}) {
  InterlockedExchange(&shared->blocked, 1);
  std::array<char, 65536> bytes{};
  DWORD count{};
  if (!payload.empty())
    std::memcpy(bytes.data(), payload.data(), std::min(payload.size(), bytes.size()));
  if (read)
    ReadFile(shared->blocked_read, bytes.data(), 1, &count, nullptr);
  else
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), bytes.data(), static_cast<DWORD>(bytes.size()),
              &count, nullptr);
  throw std::runtime_error("blocked_io_unexpectedly_completed");
}
struct Debug final : IDebugBackend {
  ~Debug() override { Quiet(); }
  NativeDebugBackend native;
  unsigned false_calls{};
  std::chrono::steady_clock::time_point Now() const override { return native.Now(); }
  Result<Unit> Attach(const OwnedProcess &p) override {
    Quiet();
    InterlockedIncrement(&shared->attaches);
    auto r = native.Attach(p);
    if (IsOk(r))
      InterlockedExchange(&shared->ready, 1);
    return r;
  }
  Result<Unit> SetKillOnExit(bool enabled) override {
    Quiet();
    if (!enabled && ++false_calls == 1 && Is(Case::InitialPolicyFailure))
      return Error{ErrorCode::DebugFailure, Stage::debug_attach, "fixture_false_denied",
                   ERROR_ACCESS_DENIED};
    auto r = native.SetKillOnExit(enabled);
    if (IsOk(r) && enabled)
      InterlockedExchange(&shared->armed, 1);
    return r;
  }
  Result<std::optional<DebugEvent>> Wait(std::chrono::milliseconds slice) override {
    Quiet();
    auto r = native.Wait(slice);
    if (IsOk(r) && Value(r) && Value(r)->attach_breakpoint_identity) {
      InterlockedExchange(&shared->held, 1);
      SetEvent(shared->go);
    }
    return r;
  }
  Result<Unit> Continue(const DebugEvent &e, Continuation c) override {
    Quiet();
    InterlockedIncrement(&shared->continues);
    if (e.exception_status == FixtureException && c == Continuation::NotHandled)
      InterlockedExchange(&shared->exception_passed, 1);
    return native.Continue(e, c);
  }
  Result<Unit> Detach() override {
    Quiet();
    const auto n = InterlockedIncrement(&shared->detaches);
    if ((Is(Case::DetachRetry) && n <= 2) || Is(Case::DetachDeadline))
      return Error{ErrorCode::DebugFailure, Stage::detach, "fixture_detach_failed", ERROR_BUSY};
    auto r = native.Detach();
    if (IsOk(r))
      InterlockedExchange(&shared->detached, 1);
    return r;
  }
  Result<Unit> AbortOwnedStartup() override {
    InterlockedExchange(&shared->unsafe, 1);
    InterlockedIncrement(&shared->aborts);
    auto r = native.AbortOwnedStartup();
    if (!IsOk(r)) {
      InterlockedExchange(&shared->abort_error,
                          static_cast<LONG>(Failure(r).native_code.value_or(0)));
      if (WaitForSingleObject(shared->target, 0) == WAIT_TIMEOUT) {
        InterlockedExchange(&shared->alive_before_death, 1);
        // Test-only rendezvous before returning the real native failure. The parent
        // independently observes liveness; the production terminal consumer then
        // receives the failure and must exit immediately without cleanup.
        const auto until = GetTickCount64() + 200;
        while (!Load(shared->alive_ack) && GetTickCount64() < until)
          Sleep(1);
      }
    }
    if (IsOk(r))
      InterlockedExchange(&shared->unsafe, 0);
    return r;
  }
};
struct Memory final : IWritableMemory {
  std::unique_ptr<IWritableMemory> native;
  explicit Memory(std::unique_ptr<IWritableMemory> p) : native(std::move(p)) {}
  Result<Bytes> Read(Address a, std::size_t n) const override { return native->Read(a, n); }
  Result<MemoryRegion> Query(Address a) const override { return native->Query(a); }
  Result<Unit> ReadInto(Address a, std::span<std::byte> b) const override {
    return native->ReadInto(a, b);
  }
  Result<Unit> Write(Address a, std::span<const std::byte> b) override {
    const auto n = Load(shared->writes);
    if ((Is(Case::Abort) || Is(Case::TerminationDenied)) && n >= 2)
      return Fault("fixture_rollback_write_failed");
    auto result = native->Write(a, b);
    if (!IsOk(result))
      return result;
    for (unsigned i = 0; i < 4; ++i) {
      auto r =
          native->ReadInto(shared->layout.slots[i],
                           std::as_writable_bytes(std::span(&shared->last_write_values[i], 1)));
      if (!IsOk(r))
        return Failure(r);
    }
    const auto count = InterlockedIncrement(&shared->writes);
    const int death = static_cast<int>(Which()) - static_cast<int>(Case::Die1) + 1;
    if (death >= 1 && death <= 4) {
      // Keep the real stop and TRUE policy while the independent controller
      // reads every slot through its retained target handle after every write.
      InterlockedExchange(&shared->write_waiting, count);
      const auto until = GetTickCount64() + 500;
      while (Load(shared->write_ack) < count && GetTickCount64() < until)
        Sleep(1);
      if (Load(shared->write_ack) != count) {
        InterlockedExchange(&shared->helper_error, ERROR_TIMEOUT);
        Die(71);
      }
      if (count == death)
        Die(70);
    }
    if (Is(Case::WatchdogMutation) && count == 1)
      Sleep(INFINITE);
    if ((Is(Case::Rollback) || Is(Case::Abort) || Is(Case::TerminationDenied)) && count == 2)
      return Fault("fixture_after_write_failure");
    return result;
  }
  Result<std::uint32_t> Protect(Address a, std::size_t n, std::uint32_t p) override {
    return native->Protect(a, n, p);
  }
};
OwnedProcess Owner() {
  // Ownership comes only from the controller's inherited directly-created child
  // handle; no process enumeration or PID reopen exists in this executable.
  HANDLE duplicate{};
  Must(DuplicateHandle(GetCurrentProcess(), shared->target, GetCurrentProcess(), &duplicate, 0,
                       FALSE, DUPLICATE_SAME_ACCESS),
       "owner_duplicate");
  return VerifiedIdentityTestFactory::Owned(
      duplicate, [](void *h) noexcept { CloseHandle(h); }, shared->layout.pid,
      shared->layout.creation);
}
std::unique_ptr<IWritableMemory> Open(const OwnedProcess &p) {
  auto r = process_detail::OpenLifecycleMemory(p);
  if (!IsOk(r))
    throw std::runtime_error(std::string(Failure(r).operation.view()));
  return std::make_unique<Memory>(std::move(std::get<std::unique_ptr<IWritableMemory>>(r)));
}
DebugStop Initial(DebugSession &session) {
  const Deadline limit{std::chrono::steady_clock::now() + std::chrono::seconds(3)};
  for (;;) {
    auto r = session.WaitStopped(limit);
    if (!IsOk(r))
      throw std::runtime_error(std::string(Failure(r).operation.view()));
    auto stop = std::move(std::get<DebugStop>(r));
    if (stop.event().attach_breakpoint_identity)
      return stop;
    auto continued = session.Continue(stop, stop.continuation());
    if (!IsOk(continued))
      throw std::runtime_error("initial_continue");
  }
}
void Direct() {
  auto owner = Owner();
  auto memory = Open(owner);
  Debug backend;
  DebugSession session(backend);
  auto attached = session.Attach(owner);
  if (!IsOk(attached))
    throw std::runtime_error("direct_attach");
  auto stop = Initial(session);
  std::stop_source cancellation;
  std::unique_ptr<DeadlineWatchdog> watchdog;
  if (Is(Case::WatchdogInspect) || Is(Case::WatchdogMutation))
    watchdog = std::make_unique<DeadlineWatchdog>(
        Deadline{std::chrono::steady_clock::now() + std::chrono::milliseconds(200)}, cancellation);
  if (Is(Case::InspectThreadExit)) {
    InterlockedExchange(&shared->thread_exited, 1);
    ExitThread(0);
  }
  if (Is(Case::InspectExit))
    Die(70);
  if (Is(Case::WatchdogInspect))
    Sleep(INFINITE);
  if (Is(Case::Exception)) {
    auto continued = session.Continue(stop, stop.continuation());
    if (!IsOk(continued))
      throw std::runtime_error("exception_continue");
    const Deadline limit{std::chrono::steady_clock::now() + std::chrono::seconds(2)};
    while (!Load(shared->exception_passed)) {
      auto next = session.WaitStopped(limit);
      if (!IsOk(next))
        throw std::runtime_error("exception_wait");
      auto &s = std::get<DebugStop>(next);
      auto r = session.Continue(s, s.continuation());
      if (!IsOk(r))
        throw std::runtime_error("exception_pass");
    }
    if (!IsOk(session.Detach(limit)))
      throw std::runtime_error("exception_detach");
    return;
  }
  auto applied = ApplyImports(session, stop, *memory, Build(shared->layout));
  if (!IsOk(applied))
    throw std::runtime_error("apply_outer");
  if (Value(applied).immediate_helper_exit_required ||
      session.Safety() == DebugSafety::AttachedUnverified)
    Die(3);
  InterlockedExchange(&shared->transaction, static_cast<LONG>(Value(applied).outcome));
  if (Is(Case::DetachDeadline)) {
    auto r = session.Detach({std::chrono::steady_clock::now() + std::chrono::milliseconds(200)});
    if (IsOk(r))
      throw std::runtime_error("detach_fault_not_reached");
    Die(3);
  }
  auto r = session.Detach({std::chrono::steady_clock::now() + std::chrono::seconds(2)});
  if (!IsOk(r))
    throw std::runtime_error("direct_detach");
}
struct Guard final : IRunGuard {
  DeadlineWatchdog watchdog;
  Guard(Deadline deadline, std::stop_source &cancel, std::stop_token external)
      : watchdog(deadline, cancel, external) {}
  void Disarm() override { watchdog.Disarm(); }
};
struct Sink final : ILogStorage {
  std::unique_ptr<FileLogStorage> files;
  explicit Sink(const std::filesystem::path &root)
      : files(std::make_unique<FileLogStorage>(root / "diagnostics")) {}
  Result<Unit> AppendEvent(std::string_view s, std::uint32_t a, std::uint32_t b) override {
    Quiet();
    return files->AppendEvent(s, a, b);
  }
  Result<Unit> WriteSummaryAtomically(std::string_view s) override {
    Quiet();
    if (Is(Case::BlockDiagnostic) && Load(shared->detached))
      Block(false, s);
    return files->WriteSummaryAtomically(s);
  }
  void WriteConsoleLine(std::string_view s) override {
    Quiet();
    if (Is(Case::BlockConsole) && s.starts_with("Import routing applied"))
      Block(false, s);
  }
};
struct FixtureEnvironment final : ILaunchEnvironment {
  ~FixtureEnvironment() override { Quiet(); }
  std::filesystem::path root;
  Sink sink;
  Debug debug;
  CompatibilityRecord compatibility = ::lifecycle::Compatibility();
  std::array<std::string_view, 1> args{};
  unsigned argc{};
  explicit FixtureEnvironment(std::filesystem::path p) : root(std::move(p)), sink(root) {
    if (Is(Case::ReadOnlyCancel)) {
      args[0] = "--diagnose";
      argc = 1;
    }
    if (Is(Case::Support) || Is(Case::StorageRefusal)) {
      args[0] = "--collect-support";
      argc = 1;
    }
  }
  std::span<const std::string_view> Arguments() const override { return {args.data(), argc}; }
  std::chrono::steady_clock::time_point Now() const override {
    return std::chrono::steady_clock::now();
  }
  Result<EffectiveConfig> Configuration(const CommandLine &c) override {
    auto config = LoadConfiguration(root, c);
    if (IsOk(config) && (Is(Case::BlockAcquisition) || Is(Case::BlockAcquisitionDeadline))) {
      std::get<EffectiveConfig>(config).runtime_observation = true;
      std::get<EffectiveConfig>(config).observation_seconds =
          Is(Case::BlockAcquisitionDeadline) ? 1 : 5;
    }
    return config;
  }
  ILogStorage &Diagnostics() override { return sink; }
  std::unique_ptr<IRunGuard> Guard(Deadline d, std::stop_source &c,
                                   std::stop_token external) override {
    return std::make_unique<::Guard>(d, c, external);
  }
  Result<PackageFacts> Discover() override {
    return PackageFacts{compatibility.package_name, compatibility.package_version,
                        compatibility.machine};
  }
  EnvironmentSnapshot Environment() override { return {}; }
  Result<Unit> InstallObserver(Deadline) override { return Unit{}; }
  Result<Unit> Activate() override { return Unit{}; }
  Result<OwnedProcess> PinFresh() override { return Owner(); }
  Result<std::unique_ptr<IWritableMemory>> Memory(const OwnedProcess &p) override {
    return Open(p);
  }
  IDebugBackend &Debugger() override { return debug; }
  Result<RuntimeInventory> Inventory(const OwnedProcess &, const IMemory &) override {
    if (!Load(shared->held))
      return Error{ErrorCode::ModuleNotReady, Stage::dependencies,
                   "fixture_before_attach_breakpoint"};
    return ::lifecycle::Inventory(shared->layout);
  }
  const CompatibilityRecord &Compatibility() const override { return compatibility; }
  Result<ObservationReport> Observe(const OwnedProcess *p, const EffectiveConfig &,
                                    std::stop_token) override {
    auto observed = [&]() -> Result<ObservedProcess> {
      if (p)
        return DuplicateObservedProcess(*p);
      HANDLE h{};
      if (!DuplicateHandle(GetCurrentProcess(), shared->target, GetCurrentProcess(), &h,
                           PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | SYNCHRONIZE, FALSE, 0))
        return Error{ErrorCode::AccessDenied, Stage::runtime_observation, "fixture_readonly_pin",
                     GetLastError()};
      return VerifiedIdentityTestFactory::Observed(
          h, [](void *v) noexcept { CloseHandle(v); }, shared->layout.pid, shared->layout.creation);
    }();
    if (!IsOk(observed))
      return Failure(observed);
    auto memory = process_detail::OpenLifecycleReadOnlyMemory(Value(observed));
    if (!IsOk(memory))
      return Failure(memory);
    auto value = Value(memory)->Read(shared->layout.slots[0], sizeof(Address));
    if (!IsOk(value))
      return Failure(value);
    InterlockedExchange(&shared->identity,
                        Value(observed).pid() == shared->layout.pid &&
                            Value(observed).creation_time() == shared->layout.creation);
    InterlockedExchange(&shared->readonly_acquired, 1);
    Block(true);
    return ObservationReport{};
  }
  Result<Unit> Collect(const EffectiveConfig &) override {
    sink.files.reset();
    WindowsSupportStorage storage(root);
    if (Is(Case::StorageRefusal))
      storage.drive_type_override = DRIVE_REMOTE;
    SupportInputs inputs{1, {1, "disposable-lifecycle", "", false, ""}, {}, {}};
    auto result = CollectSupport(inputs, storage);
    if (Is(Case::StorageRefusal)) {
      const bool correct = !IsOk(result) &&
                           Failure(result).operation.view() == "SupportLocalStorageRequired" &&
                           storage.root_open_attempts == 0;
      InterlockedExchange(&shared->storage_refused, correct);
      return correct ? Result<Unit>{Unit{}} : Result<Unit>{Fault("storage_refusal_missing")};
    }
    if (!IsOk(result))
      return Failure(result);
    return Unit{};
  }
  [[noreturn]] void ExitHelper() noexcept override { Die(3); }
};
void Identity() {
  auto owner = Owner();
  auto result = DuplicateObservedProcess(owner);
  if (!IsOk(result))
    throw std::runtime_error("observed_duplicate");
  auto &observed = Value(result);
  const bool identity = observed.pid() == shared->layout.pid &&
                        observed.creation_time() == shared->layout.creation &&
                        Creation(observed.native_handle()) == shared->layout.creation;
  auto memory = process_detail::OpenLifecycleReadOnlyMemory(observed);
  if (!IsOk(memory))
    throw std::runtime_error("readonly_adapter");
  auto read = Value(memory)->Read(shared->layout.slots[0], 8);
  if (!IsOk(read))
    throw std::runtime_error("readonly_read");
  // Use the independently queried writable slot page, not an RO page whose
  // page error can precede access-mask validation (Windows reported 998 there).
  const auto address = shared->layout.slots[1];
  const auto v = shared->layout.originals[1];
  MEMORY_BASIC_INFORMATION region{};
  Must(VirtualQueryEx(owner.native_handle(), reinterpret_cast<void *>(address), &region,
                      sizeof(region)) == sizeof(region) &&
           region.Protect == PAGE_READWRITE,
       "writable_probe_page");
  SIZE_T n{};
  Must(WriteProcessMemory(owner.native_handle(), reinterpret_cast<void *>(address), &v, 8, &n) &&
           n == 8,
       "owned_write_positive_control");
  SetLastError(0);
  const bool write_result =
      WriteProcessMemory(observed.native_handle(), reinterpret_cast<void *>(address), &v, 8, &n) !=
      FALSE;
  InterlockedExchange(&shared->write_denied_error, static_cast<LONG>(GetLastError()));
  bool denied = !write_result && Load(shared->write_denied_error) == ERROR_ACCESS_DENIED;
  DWORD old{};
  const bool protect_result =
      VirtualProtectEx(observed.native_handle(), reinterpret_cast<void *>(shared->layout.slots[0]),
                       4096, PAGE_READWRITE, &old) != FALSE;
  InterlockedExchange(&shared->protect_denied_error, static_cast<LONG>(GetLastError()));
  denied = denied && !protect_result && Load(shared->protect_denied_error) == ERROR_ACCESS_DENIED;
  // Do not attempt termination with the read-only handle: inspect its granted
  // access instead through an attempted DuplicateHandle rights upgrade is unsafe
  // (same-process duplication can grant more). Write/protect denial is physical.
  auto stale = VerifiedIdentityTestFactory::Owned(
      owner.native_handle(), [](void *) noexcept {}, shared->layout.pid,
      shared->layout.creation + 1);
  const auto rejected = DuplicateObservedProcess(stale);
  InterlockedExchange(&shared->identity, identity && !IsOk(rejected) &&
                                             Failure(rejected).code == ErrorCode::IdentityChanged);
  InterlockedExchange(&shared->readonly_denied, denied);
  SetEvent(shared->go);
  InterlockedExchange(&shared->ready, 1);
  if (Is(Case::ObservedExited)) {
    const auto until = GetTickCount64() + 2000;
    while (!Load(shared->target_reported) && GetTickCount64() < until)
      Sleep(1);
    Must(Load(shared->target_reported) != 0, "exit_case_execution_proof");
    SetEvent(shared->quit);
    Must(WaitForSingleObject(owner.native_handle(), 2000) == WAIT_OBJECT_0, "owned_exit_observed");
    const auto retry = DuplicateObservedProcess(owner);
    const auto read_retry = process_detail::OpenLifecycleReadOnlyMemory(observed);
    InterlockedExchange(&shared->observed_exit_rejected,
                        !IsOk(retry) && Failure(retry).code == ErrorCode::IdentityChanged &&
                            !IsOk(read_retry) &&
                            Failure(read_retry).code == ErrorCode::IdentityChanged);
  }
}
} // namespace
void *operator new(std::size_t size) {
  Quiet();
  if (auto *p = std::malloc(size ? size : 1))
    return p;
  throw std::bad_alloc();
}
void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }
void *operator new(std::size_t size, std::align_val_t alignment) {
  Quiet();
  if (auto *p = _aligned_malloc(size ? size : 1, static_cast<std::size_t>(alignment)))
    return p;
  throw std::bad_alloc();
}
void *operator new[](std::size_t size, std::align_val_t alignment) {
  return ::operator new(size, alignment);
}
void operator delete(void *p, std::align_val_t) noexcept { _aligned_free(p); }
void operator delete[](void *p, std::align_val_t) noexcept { _aligned_free(p); }
void operator delete(void *p, std::size_t, std::align_val_t) noexcept { _aligned_free(p); }
void operator delete[](void *p, std::size_t, std::align_val_t) noexcept { _aligned_free(p); }
int wmain(int argc, wchar_t **argv) {
  if (argc != 3)
    return 2;
  try {
    shared = static_cast<Shared *>(
        MapViewOfFile(Arg(argv[1]), FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
    Must(shared != nullptr, "helper_map");
    Must(shared->layout.magic == Magic && GetProcessId(shared->target) == shared->layout.pid &&
             Creation(shared->target) == shared->layout.creation,
         "helper_identity");
    Must(SetStdHandle(STD_OUTPUT_HANDLE, shared->blocked_write), "stdout_pipe");
    // Global helper bound also covers a broken fixture path; production watchdog
    // cases use their own actual production watchdog below.
    std::stop_source overall_cancel;
    DeadlineWatchdog outer({std::chrono::steady_clock::now() + std::chrono::seconds(8)},
                           overall_cancel);
    std::stop_source external;
    std::jthread canceller([&](std::stop_token stop) {
      while (!stop.stop_requested()) {
        if (WaitForSingleObject(shared->cancel, 10) == WAIT_OBJECT_0) {
          InterlockedExchange64(&shared->requested_at, static_cast<LONG64>(GetTickCount64()));
          external.request_stop();
          return;
        }
      }
    });
    if (Is(Case::ObservedIdentity) || Is(Case::ObservedExited))
      Identity();
    else if (Is(Case::InspectThreadExit)) {
      Handle thread(CreateThread(
          nullptr, 0,
          [](void *) -> DWORD {
            try {
              Direct();
            } catch (...) {
              InterlockedExchange(&shared->helper_error, 1);
            }
            return 0;
          },
          nullptr, 0, nullptr));
      Must(thread.h != nullptr, "debug_thread");
      Must(WaitForSingleObject(thread.h, 4000) == WAIT_OBJECT_0, "debug_thread_exit");
      const auto until = GetTickCount64() + 2000;
      while (!Load(shared->thread_resumed_ack) && GetTickCount64() < until)
        Sleep(1);
      Must(Load(shared->thread_resumed_ack) != 0, "thread_exit_resume_proof");
    } else if (Is(Case::Commit) || Is(Case::Rollback) || Is(Case::Abort) ||
               Is(Case::TerminationDenied) || Is(Case::InitialPolicyFailure) ||
               Is(Case::BlockAcquisition) || Is(Case::BlockAcquisitionDeadline) ||
               Is(Case::BlockDiagnostic) || Is(Case::BlockConsole) || Is(Case::ReadOnlyCancel) ||
               Is(Case::Support) || Is(Case::StorageRefusal)) {
      if (Is(Case::ReadOnlyCancel) || Is(Case::Support) || Is(Case::StorageRefusal)) {
        SetEvent(shared->go);
        InterlockedExchange(&shared->ready, 1);
      }
      FixtureEnvironment environment(argv[2]);
      const auto code = RunLauncherToConsole(environment, external.get_token());
      InterlockedExchange(&shared->returned, 1);
      InterlockedExchange(&shared->status, code);
      if (code != 0 && !Is(Case::Rollback) && !Is(Case::Abort))
        throw std::runtime_error("coordinator_returned_failure");
    } else
      Direct();
    canceller.request_stop();
    outer.Disarm();
    return Load(shared->helper_error) ? 6 : 0;
  } catch (const std::exception &e) {
    if (shared)
      InterlockedExchange(&shared->helper_error, 1);
    std::fprintf(stderr, "lifecycle helper: %s\n", e.what());
    return 6;
  }
}
