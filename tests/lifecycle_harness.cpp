#include "fixtures/lifecycle_protocol.hpp"
#include <cstdio>
#include <thread>
namespace lifecycle {
namespace {
Handle Event() { return Handle(CreateEventW(nullptr, TRUE, FALSE, nullptr)); }
struct Pipe {
  Handle read, write;
  Pipe() {
    HANDLE r{}, w{};
    Must(CreatePipe(&r, &w, nullptr, 4096), "CreatePipe");
    read = Handle(r);
    write = Handle(w);
  }
};
Handle Spawn(const std::filesystem::path &path, std::wstring arguments,
             std::vector<HANDLE> inherited) {
  SIZE_T size{};
  InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
  std::vector<std::byte> storage(size);
  auto *attributes = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
  Must(InitializeProcThreadAttributeList(attributes, 1, 0, &size) != FALSE, "attribute_list");
  struct Clean {
    PPROC_THREAD_ATTRIBUTE_LIST p;
    ~Clean() { DeleteProcThreadAttributeList(p); }
  } clean{attributes};
  for (auto h : inherited)
    Must(SetHandleInformation(h, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT) != FALSE,
         "inherit_handle");
  Must(UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited.data(),
                                 inherited.size() * sizeof(HANDLE), nullptr, nullptr) != FALSE,
       "handle_list");
  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  startup.lpAttributeList = attributes;
  PROCESS_INFORMATION process{};
  auto command = L"\"" + path.wstring() + L"\" " + arguments;
  const BOOL ok = CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, TRUE,
                                 EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr,
                                 path.parent_path().c_str(), &startup.StartupInfo, &process);
  const DWORD error = GetLastError();
  for (auto h : inherited)
    SetHandleInformation(h, HANDLE_FLAG_INHERIT, 0);
  SetLastError(error);
  Must(ok != FALSE, "CreateProcess_fixture");
  CloseHandle(process.hThread);
  return Handle(process.hProcess);
}
bool Available(HANDLE pipe, unsigned size) {
  DWORD n{};
  return PeekNamedPipe(pipe, nullptr, 0, nullptr, &n, nullptr) && n >= size;
}
void Receive(HANDLE pipe, auto &value) {
  DWORD n{};
  Must(ReadFile(pipe, &value, sizeof(value), &n, nullptr) && n == sizeof(value), "pipe_receive");
}
// A directly-created process pin is the only authority used for termination.
// Report native failure and require observed exit; a timeout is never "stopped".
void ForceExit(HANDLE process, unsigned code) {
  const auto state = WaitForSingleObject(process, 0);
  if (state == WAIT_OBJECT_0)
    return;
  Must(state == WAIT_TIMEOUT, "owned_cleanup_initial_wait");
  if (!TerminateProcess(process, code)) {
    const auto error = GetLastError();
    if (WaitForSingleObject(process, 0) != WAIT_OBJECT_0) {
      SetLastError(error);
      Must(false, "owned_cleanup_terminate");
    }
  }
  Must(WaitForSingleObject(process, 300) == WAIT_OBJECT_0, "owned_cleanup_exit_unverified");
}
struct Child {
  Pipe pipe;
  Handle go = Event(), quit = Event(), process;
  Layout layout{};
  Report report{};
  bool reported{}, sentinel{}, stopped{};
  explicit Child(bool unrelated, bool exception = false, const wchar_t *mode = nullptr,
                 Handle *observer = nullptr)
      : sentinel(unrelated) {
    process =
        Spawn(Sibling(L"wfg_fixture_target.exe"),
              Number(pipe.write.h) + L" " + Number(go.h) + L" " + Number(quit.h) +
                  (mode ? std::wstring(L" ") + mode : (exception ? L" exception" : L" normal")),
              {pipe.write.h, go.h, quit.h});
    // This local guard unwinds before members are destroyed if any layout or
    // observer setup throws. ~Child itself does not run on constructor failure.
    struct ConstructionCleanup {
      Child &child;
      bool active{true};
      ~ConstructionCleanup() {
        if (active)
          child.CleanupNoThrow();
      }
    } construction{*this};
    if (observer) {
      HANDLE duplicate{};
      Must(DuplicateHandle(GetCurrentProcess(), process.h, GetCurrentProcess(), &duplicate, 0,
                           FALSE, DUPLICATE_SAME_ACCESS),
           "regression_owned_pin");
      *observer = Handle(duplicate);
    }
    pipe.write = Handle();
    const auto until = GetTickCount64() + 2000;
    while (!Available(pipe.read.h, sizeof(layout)) && GetTickCount64() < until &&
           WaitForSingleObject(process.h, 0) == WAIT_TIMEOUT)
      Sleep(1);
    Must(Available(pipe.read.h, sizeof(layout)), "fixture_layout_timeout");
    Receive(pipe.read.h, layout);
    Must(layout.magic == Magic && layout.pid == GetProcessId(process.h) &&
             layout.creation == Creation(process.h),
         "fixture_layout_identity");
    construction.active = false;
  }
  void Poll() {
    if (!reported && Available(pipe.read.h, sizeof(report))) {
      Receive(pipe.read.h, report);
      Must(report.magic == Magic && report.pid == layout.pid, "report_identity");
      reported = true;
    }
  }
  void Shutdown() {
    if (stopped)
      return;
    if (process.h) {
      Must(SetEvent(go.h) != FALSE && SetEvent(quit.h) != FALSE, "owned_cleanup_signal");
      const auto state = WaitForSingleObject(process.h, 300);
      if (state != WAIT_OBJECT_0) {
        Must(state == WAIT_TIMEOUT, "owned_cleanup_wait");
        Must(!sentinel, "sentinel_cooperative_exit_unverified");
        ForceExit(process.h, 99);
      }
      Must(WaitForSingleObject(process.h, 0) == WAIT_OBJECT_0, "owned_cleanup_exit_unverified");
    }
    stopped = true;
  }
  void CleanupNoThrow() noexcept {
    try {
      Shutdown();
    } catch (const std::exception &e) {
      // Explicit cleanup propagates this error before any test can pass. During
      // exception unwinding retain the failed-test diagnostic without throwing.
      std::fprintf(stderr, "fixture cleanup failed: %s\n", e.what());
    }
  }
  ~Child() { CleanupNoThrow(); }
};
} // namespace
Observation RunFixtureCase(Case which) {
  const auto started = GetTickCount64();
  // Reserve 1.5 seconds of the ten-second case budget for helper, target,
  // and separate sentinel cleanup (bounded waits total at most 1.2 seconds).
  const auto deadline = started + 8500;
  Child sentinel(true);
  SetEvent(sentinel.go.h);
  Child target(false, which == Case::Exception || which == Case::DetachRetry ||
                          which == Case::DetachDeadline);
  Handle mapping(CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                    sizeof(Shared), nullptr));
  Must(mapping.h != nullptr, "fixture_mapping");
  auto *shared =
      static_cast<Shared *>(MapViewOfFile(mapping.h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
  Must(shared != nullptr, "controller_map");
  struct Unmap {
    void *p;
    ~Unmap() { UnmapViewOfFile(p); }
  } unmap{shared};
  new (shared) Shared{};
  shared->layout = target.layout;
  shared->which = which;
  Pipe blocked;
  Handle cancel = Event();
  Handle inherited_target;
  const bool readonly =
      which == Case::ReadOnlyCancel || which == Case::Support || which == Case::StorageRefusal;
  const DWORD rights =
      PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | SYNCHRONIZE |
      (readonly ? 0 : (PROCESS_VM_WRITE | PROCESS_VM_OPERATION)) |
      ((readonly || which == Case::TerminationDenied || which == Case::InitialPolicyFailure)
           ? 0
           : PROCESS_TERMINATE);
  HANDLE duplicate{};
  Must(DuplicateHandle(GetCurrentProcess(), target.process.h, GetCurrentProcess(), &duplicate,
                       rights, FALSE, 0),
       "target_capability");
  inherited_target = Handle(duplicate);
  shared->target = inherited_target.h;
  shared->go = target.go.h;
  shared->quit = target.quit.h;
  shared->cancel = cancel.h;
  shared->blocked_read = blocked.read.h;
  shared->blocked_write = blocked.write.h;
  const auto root = Sibling(L"lifecycle-artifacts") / std::to_wstring(GetCurrentProcessId()) /
                    std::to_wstring(static_cast<unsigned>(which));
  std::filesystem::create_directories(root);
  Handle helper = Spawn(Sibling(L"wfg_lifecycle_helper.exe"),
                        Number(mapping.h) + L" \"" + root.wstring() + L"\"",
                        {mapping.h, inherited_target.h, target.go.h, target.quit.h, cancel.h,
                         blocked.read.h, blocked.write.h});
  struct HelperCleanup {
    HANDLE h;
    bool stopped{};
    void Stop() {
      if (!stopped) {
        ForceExit(h, 98);
        stopped = true;
      }
    }
    ~HelperCleanup() {
      try {
        Stop();
      } catch (const std::exception &e) {
        std::fprintf(stderr, "helper cleanup failed: %s\n", e.what());
      }
    }
  } cleanup{helper.h};
  bool exited{}, cancel_sent{}, independent_alive{}, thread_resumed{};
  ULONGLONG blocked_at{}, helper_exit_at{};
  unsigned controller_writes{};
  while (GetTickCount64() < deadline) {
    target.Poll();
    sentinel.Poll();
    const auto pending_write = static_cast<unsigned>(Load(shared->write_waiting));
    if (pending_write > controller_writes) {
      Must(pending_write == controller_writes + 1 && pending_write <= 4 && Load(shared->held) &&
               Load(shared->armed) && !target.reported &&
               WaitForSingleObject(helper.h, 0) == WAIT_TIMEOUT &&
               WaitForSingleObject(target.process.h, 0) == WAIT_TIMEOUT,
           "controller_write_rendezvous_state");
      std::array<std::uint64_t, 4> actual{};
      for (unsigned i = 0; i < 4; ++i) {
        SIZE_T read{};
        Must(ReadProcessMemory(target.process.h,
                               reinterpret_cast<const void *>(target.layout.slots[i]), &actual[i],
                               sizeof(actual[i]), &read) &&
                 read == sizeof(actual[i]),
             "controller_native_slot_read");
        Must(actual[i] ==
                 (i < pending_write ? target.layout.replacements[i] : target.layout.originals[i]),
             "controller_native_slot_mismatch");
      }
      Must(WaitForSingleObject(helper.h, 0) == WAIT_TIMEOUT &&
               WaitForSingleObject(target.process.h, 0) == WAIT_TIMEOUT,
           "controller_write_rendezvous_liveness");
      ++controller_writes;
      std::printf(
          "CONTROLLER_WRITE case=%u sequence=%u slots=%016llx,%016llx,%016llx,%016llx\n",
          static_cast<unsigned>(which), controller_writes,
          static_cast<unsigned long long>(actual[0]), static_cast<unsigned long long>(actual[1]),
          static_cast<unsigned long long>(actual[2]), static_cast<unsigned long long>(actual[3]));
      InterlockedExchange(&shared->write_ack, static_cast<LONG>(controller_writes));
    }
    if (target.reported)
      InterlockedExchange(&shared->target_reported, 1);
    if (target.reported && Load(shared->thread_exited) &&
        WaitForSingleObject(helper.h, 0) == WAIT_TIMEOUT) {
      thread_resumed = true;
      InterlockedExchange(&shared->thread_resumed_ack, 1);
    }
    if (Load(shared->alive_before_death) &&
        WaitForSingleObject(target.process.h, 0) == WAIT_TIMEOUT) {
      independent_alive = true;
      InterlockedExchange(&shared->alive_ack, 1);
    }
    if (Load(shared->blocked) && !blocked_at)
      blocked_at = GetTickCount64();
    if (blocked_at && !cancel_sent && which != Case::BlockAcquisitionDeadline &&
        GetTickCount64() - blocked_at >= 100) {
      Must(WaitForSingleObject(helper.h, 0) == WAIT_TIMEOUT, "io_not_actually_blocked");
      SetEvent(cancel.h);
      cancel_sent = true;
    }
    if (!exited && WaitForSingleObject(helper.h, 0) == WAIT_OBJECT_0) {
      exited = true;
      helper_exit_at = GetTickCount64();
    }
    if (exited && (target.reported || WaitForSingleObject(target.process.h, 0) == WAIT_OBJECT_0) &&
        sentinel.reported)
      break;
    Sleep(1);
  }
  target.Poll();
  sentinel.Poll();
  Observation o;
  o.controller_write_checks = controller_writes;
  o.ready = Load(shared->ready) && sentinel.reported;
  o.held = Load(shared->held) != 0;
  o.armed = Load(shared->armed) != 0;
  o.helper_exited = exited;
  o.target_exited = WaitForSingleObject(target.process.h, 0) == WAIT_OBJECT_0;
  o.target_alive = WaitForSingleObject(target.process.h, 0) == WAIT_TIMEOUT;
  o.sentinel_alive = WaitForSingleObject(sentinel.process.h, 0) == WAIT_TIMEOUT &&
                     sentinel.reported && sentinel.report.debugged == 0 &&
                     sentinel.report.pointers == sentinel.layout.originals &&
                     sentinel.report.protections == sentinel.layout.protections;
  o.ran_originals = target.reported && target.report.pointers == target.layout.originals;
  o.ran_replacements = target.reported && target.report.pointers == target.layout.replacements;
  for (unsigned i = 0; i < 4; ++i) {
    o.ran_originals = o.ran_originals && target.report.calls[i] == 10 + i;
    o.ran_replacements = o.ran_replacements && target.report.calls[i] == 20 + i;
  }
  o.mixed = target.reported && !o.ran_originals && !o.ran_replacements;
  o.protections_restored =
      target.reported && target.report.protections == target.layout.protections;
  o.exception_handled =
      target.reported && target.report.exception_handled && Load(shared->exception_passed);
  o.abort_denied = Load(shared->abort_error) == ERROR_ACCESS_DENIED;
  o.alive_before_death = independent_alive;
  o.unsafe_quiet = Load(shared->forbidden) == 0;
  o.detached = Load(shared->detached) != 0;
  o.identity_verified = Load(shared->identity) != 0;
  o.readonly_denied = Load(shared->readonly_denied) != 0;
  o.blocked = blocked_at != 0;
  o.thread_exit_resumed = thread_resumed;
  o.observed_exit_rejected = Load(shared->observed_exit_rejected) != 0;
  o.readonly_acquired = Load(shared->readonly_acquired) != 0;
  o.elapsed_ms = helper_exit_at - started;
  if (blocked_at && helper_exit_at >= blocked_at)
    o.blocked_ms = helper_exit_at - blocked_at;
  const auto acknowledged = static_cast<unsigned>(Load(shared->writes));
  o.write_values_verified = acknowledged > 0 && acknowledged <= 4;
  for (unsigned i = 0; i < 4; ++i)
    o.write_values_verified =
        o.write_values_verified &&
        shared->last_write_values[i] ==
            (i < acknowledged ? target.layout.replacements[i] : target.layout.originals[i]);
  o.storage_refused_before_io = Load(shared->storage_refused) != 0;
  o.writes = Load(shared->writes);
  o.detaches = Load(shared->detaches);
  o.continues = Load(shared->continues);
  o.attach_calls = Load(shared->attaches);
  o.abort_calls = Load(shared->aborts);
  DWORD code{};
  GetExitCodeProcess(helper.h, &code);
  o.helper_code = code;
  const auto request =
      static_cast<ULONGLONG>(InterlockedCompareExchange64(&shared->requested_at, 0, 0));
  if (request && helper_exit_at >= request)
    o.cancellation_ms = helper_exit_at - request;
  // Preserve the target's pre-cleanup observations, then prove teardown did
  // not touch the unrelated sentinel before its separate cooperative exit.
  cleanup.Stop();
  target.Shutdown();
  sentinel.Poll();
  o.sentinel_alive = o.sentinel_alive && WaitForSingleObject(sentinel.process.h, 0) == WAIT_TIMEOUT;
  sentinel.Shutdown();
  Must(WaitForSingleObject(sentinel.process.h, 0) == WAIT_OBJECT_0, "sentinel_cooperative_exit");
  o.total_ms = GetTickCount64() - started;
  Must(o.total_ms <= 10000, "fixture_total_budget_including_cleanup");
  std::printf("CASE %u helper=%u ready=%d held=%d armed=%d writes=%u target_exit=%d "
              "target_alive=%d report=%d original=%d replacement=%d protections=%d sentinel=%d "
              "cancel_ms=%llu abort_error=%ld quiet=%d status=%ld helper_error=%ld\n",
              static_cast<unsigned>(which), code, o.ready, o.held, o.armed, o.writes,
              o.target_exited, o.target_alive, target.reported, o.ran_originals, o.ran_replacements,
              o.protections_restored, o.sentinel_alive,
              static_cast<unsigned long long>(o.cancellation_ms), Load(shared->abort_error),
              o.unsafe_quiet, Load(shared->status), Load(shared->helper_error));
  for (unsigned i = 0; i < 4; ++i)
    std::printf(" SLOT %u original=%016llx replacement=%016llx acknowledged=%016llx "
                "executed=%016llx protection=%lu expected=%lu\n",
                i, static_cast<unsigned long long>(target.layout.originals[i]),
                static_cast<unsigned long long>(target.layout.replacements[i]),
                static_cast<unsigned long long>(shared->last_write_values[i]),
                static_cast<unsigned long long>(target.report.pointers[i]),
                static_cast<unsigned long>(target.report.protections[i]),
                static_cast<unsigned long>(target.layout.protections[i]));
  std::printf(" RIGHTS write_error=%ld protect_error=%ld blocked_ms=%llu elapsed_ms=%llu "
              "readonly=%d thread_resume=%d total_ms=%llu controller_reads=%u\n",
              Load(shared->write_denied_error), Load(shared->protect_denied_error),
              static_cast<unsigned long long>(o.blocked_ms),
              static_cast<unsigned long long>(o.elapsed_ms), o.readonly_acquired,
              o.thread_exit_resumed, static_cast<unsigned long long>(o.total_ms),
              o.controller_write_checks);
  if (!exited || Load(shared->helper_error))
    throw std::runtime_error("fixture_helper_failed_or_timed_out");
  return o;
}
CleanupObservation RunCleanupRegression() {
  const auto started = GetTickCount64();
  Child sentinel(true);
  SetEvent(sentinel.go.h);
  CleanupObservation o;
  Handle observer;
  try {
    Child malformed(false, false, L"malformed", &observer);
  } catch (const std::exception &e) {
    o.setup_failed = std::string(e.what()).find("fixture_layout_identity") != std::string::npos;
  }
  Must(observer.h != nullptr, "regression_observer_missing");
  o.constructor_exit = WaitForSingleObject(observer.h, 0) == WAIT_OBJECT_0;
  // Independent retained full-right pin bounds the RED regression too.
  if (!o.constructor_exit) {
    Must(TerminateProcess(observer.h, 97) != FALSE, "regression_fallback_terminate");
    Must(WaitForSingleObject(observer.h, 300) == WAIT_OBJECT_0, "regression_fallback_exit");
  }
  Child stalled(false, false, L"stalled");
  HANDLE reduced{};
  Must(DuplicateHandle(GetCurrentProcess(), stalled.process.h, GetCurrentProcess(), &reduced,
                       PROCESS_QUERY_INFORMATION | SYNCHRONIZE, FALSE, 0),
       "regression_reduced_pin");
  Handle full = std::move(stalled.process);
  stalled.process = Handle(reduced);
  try {
    stalled.Shutdown();
  } catch (const std::exception &e) {
    o.denial_reported = std::string(e.what()).find("native=5") != std::string::npos;
  }
  o.denied_alive = WaitForSingleObject(full.h, 0) == WAIT_TIMEOUT;
  stalled.process = std::move(full);
  stalled.Shutdown();
  o.recovered_exit = WaitForSingleObject(stalled.process.h, 0) == WAIT_OBJECT_0;
  // Bound old unverified-stopped behavior during the compiled RED run.
  if (!o.recovered_exit) {
    Must(TerminateProcess(stalled.process.h, 96) != FALSE, "regression_retry_terminate");
    Must(WaitForSingleObject(stalled.process.h, 300) == WAIT_OBJECT_0, "regression_retry_exit");
  }
  sentinel.Poll();
  o.sentinel_survived = WaitForSingleObject(sentinel.process.h, 0) == WAIT_TIMEOUT;
  sentinel.Shutdown();
  Must(WaitForSingleObject(sentinel.process.h, 0) == WAIT_OBJECT_0, "regression_sentinel_exit");
  std::printf("CLEANUP setup_failed=%d constructor_exit=%d denial_reported=%d denied_alive=%d "
              "recovered_exit=%d sentinel=%d total_ms=%llu\n",
              o.setup_failed, o.constructor_exit, o.denial_reported, o.denied_alive,
              o.recovered_exit, o.sentinel_survived,
              static_cast<unsigned long long>(GetTickCount64() - started));
  Must(GetTickCount64() - started <= 10000, "cleanup_regression_total_budget");
  return o;
}
} // namespace lifecycle
