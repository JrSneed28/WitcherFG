#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifdef _WIN32
#include <windows.h>
#endif

#include "wfg/debug_session.hpp"
#include "wfg/windows_process.hpp"

#include <type_traits>

static_assert(!std::is_copy_constructible_v<wfg::DebugStop>);
static_assert(!std::is_default_constructible_v<wfg::DebugStop>);
static_assert(!std::is_invocable_v<decltype(&wfg::DebugSession::Attach), wfg::DebugSession&, const wfg::ObservedProcess&>);
static_assert(std::is_same_v<decltype(wfg::OpenMemory(std::declval<const wfg::OwnedProcess&>())), wfg::Result<std::unique_ptr<wfg::IWritableMemory>>>);

int main() {
  wfg::NativeDebugBackend backend;
  wfg::DebugSession session(backend);
  std::stop_source cancellation;
  wfg::DeadlineWatchdog watchdog({std::chrono::steady_clock::now() + std::chrono::seconds(60)}, cancellation);
  watchdog.Disarm();
  return wfg::IsOk(session.SetKillOnExit(false)) || cancellation.stop_requested() ? 1 : 0;
}
