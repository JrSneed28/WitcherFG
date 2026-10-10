#pragma once

#include "wfg/core.hpp"

#include <memory>
#include <stop_token>
#include <thread>

namespace wfg {

enum class Continuation { Handled, NotHandled };
enum class DebugEventKind { ProcessCreated, ThreadCreated, DllLoaded, Other, Exception, ProcessExited };
struct DebugEvent {
  DebugEventKind kind{DebugEventKind::Other};
  std::uint32_t pid{};
  std::uint32_t tid{};
  std::uint32_t exception_status{};
  Address exception_address{};
  bool first_chance{};
  bool attach_breakpoint_identity{};
};

class IDebugBackend {
public:
  virtual ~IDebugBackend() = default;
  virtual std::chrono::steady_clock::time_point Now() const = 0;
  virtual Result<Unit> Attach(const OwnedProcess&) = 0;
  virtual Result<Unit> SetKillOnExit(bool) = 0;
  virtual Result<std::optional<DebugEvent>> Wait(std::chrono::milliseconds) = 0;
  virtual Result<Unit> Continue(const DebugEvent&, Continuation) = 0;
  virtual Result<Unit> Detach() = 0;
  virtual Result<Unit> AbortOwnedStartup() = 0;
};

struct DebugSessionState;
class DebugStop {
public:
  DebugStop(const DebugStop&) = delete;
  DebugStop& operator=(const DebugStop&) = delete;
  DebugStop(DebugStop&&) noexcept;
  DebugStop& operator=(DebugStop&&) noexcept;
  bool active() const noexcept;
  const DebugEvent& event() const noexcept { return event_; }
  Continuation continuation() const noexcept { return continuation_; }
private:
  DebugStop(std::shared_ptr<DebugSessionState>, std::uint64_t, DebugEvent, Continuation);
  std::weak_ptr<DebugSessionState> state_;
  std::uint64_t sequence_{};
  DebugEvent event_;
  Continuation continuation_{};
  friend class DebugSession;
};

enum class DebugSafety { NotAttached, AttachedSafe, AttachedUnverified, Exited };

class DebugSession {
public:
  explicit DebugSession(IDebugBackend&, std::stop_token = {});
  ~DebugSession();
  DebugSession(const DebugSession&) = delete;
  DebugSession& operator=(const DebugSession&) = delete;
  Result<Unit> Attach(const OwnedProcess&);
  DebugSafety Safety() const noexcept;
  Result<DebugStop> WaitStopped(Deadline);
  Result<Unit> SetKillOnExit(bool);
  bool Holds(const DebugStop&) const noexcept;
  Result<Unit> Continue(DebugStop&, Continuation);
  Result<Unit> Detach();
  Result<Unit> Detach(Deadline);
  Result<Unit> AbortOwnedStartup();
private:
  bool Owner() const;
  Result<Unit> ReleaseHeld();
  IDebugBackend& backend_;
  std::shared_ptr<DebugSessionState> state_;
  std::thread::id owner_;
  std::stop_token cancellation_;
};

class NativeDebugBackend final : public IDebugBackend {
public:
  NativeDebugBackend();
  ~NativeDebugBackend() override;
  NativeDebugBackend(const NativeDebugBackend&) = delete;
  NativeDebugBackend& operator=(const NativeDebugBackend&) = delete;
  std::chrono::steady_clock::time_point Now() const override;
  Result<Unit> Attach(const OwnedProcess&) override;
  Result<Unit> SetKillOnExit(bool) override;
  Result<std::optional<DebugEvent>> Wait(std::chrono::milliseconds) override;
  Result<Unit> Continue(const DebugEvent&, Continuation) override;
  Result<Unit> Detach() override;
  Result<Unit> AbortOwnedStartup() override;
private:
  struct State;
  std::unique_ptr<State> state_;
};

class IWatchdogBackend {
public:
  virtual ~IWatchdogBackend() = default;
  virtual bool WaitUntil(Deadline, std::stop_token disarmed) = 0;
  virtual void ExitHelper() noexcept = 0;
};

class DeadlineWatchdog {
public:
  DeadlineWatchdog(Deadline, std::stop_source&);
  DeadlineWatchdog(Deadline, std::stop_source&, std::stop_token external);
  DeadlineWatchdog(Deadline, std::stop_source&, IWatchdogBackend&);
  DeadlineWatchdog(Deadline, std::stop_source&, IWatchdogBackend&, std::stop_token external);
  ~DeadlineWatchdog();
  DeadlineWatchdog(const DeadlineWatchdog&) = delete;
  DeadlineWatchdog& operator=(const DeadlineWatchdog&) = delete;
  void Disarm();
private:
  std::jthread thread_;
};

}
