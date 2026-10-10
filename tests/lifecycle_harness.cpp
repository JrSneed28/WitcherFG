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
struct Child {
  Pipe pipe;
  Handle go = Event(), quit = Event(), process;
  Layout layout{};
  Report report{};
  bool reported{}, sentinel{}, stopped{};
  explicit Child(bool unrelated, bool exception = false) : sentinel(unrelated) {
    process = Spawn(Sibling(L"wfg_fixture_target.exe"),
                    Number(pipe.write.h) + L" " + Number(go.h) + L" " + Number(quit.h) +
                        (exception ? L" exception" : L" normal"),
                    {pipe.write.h, go.h, quit.h});
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
      SetEvent(go.h);
      SetEvent(quit.h);
      if (WaitForSingleObject(process.h, 300) != WAIT_OBJECT_0 && !sentinel) {
        TerminateProcess(process.h, 99);
        WaitForSingleObject(process.h, 300);
      }
    }
    stopped = true;
  }
  ~Child() { Shutdown(); }
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
    void Stop() {
      if (WaitForSingleObject(h, 0) == WAIT_TIMEOUT) {
        TerminateProcess(h, 98);
        WaitForSingleObject(h, 300);
      }
    }
    ~HelperCleanup() { Stop(); }
  } cleanup{helper.h};
  bool exited{}, cancel_sent{}, independent_alive{}, thread_resumed{};
  ULONGLONG blocked_at{}, helper_exit_at{};
  while (GetTickCount64() < deadline) {
    target.Poll();
    sentinel.Poll();
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
              "readonly=%d thread_resume=%d\n",
              Load(shared->write_denied_error), Load(shared->protect_denied_error),
              static_cast<unsigned long long>(o.blocked_ms),
              static_cast<unsigned long long>(o.elapsed_ms), o.readonly_acquired,
              o.thread_exit_resumed);
  if (!exited || Load(shared->helper_error))
    throw std::runtime_error("fixture_helper_failed_or_timed_out");
  return o;
}
} // namespace lifecycle
