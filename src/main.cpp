#include "wfg/launcher.hpp"
#include <cstdio>
#include <string>
#include <vector>
#define NOMINMAX
#include <windows.h>

namespace {
SRWLOCK callback_lock=SRWLOCK_INIT;
std::stop_source* cancellation{};
BOOL WINAPI Cancel(DWORD event) {
  if(event!=CTRL_C_EVENT&&event!=CTRL_BREAK_EVENT&&event!=CTRL_CLOSE_EVENT)return FALSE;
  AcquireSRWLockShared(&callback_lock);
  if(cancellation)cancellation->request_stop();
  ReleaseSRWLockShared(&callback_lock);
  return TRUE;
}
}
int wmain(int argc,wchar_t** argv) {
  std::stop_source stop;
  cancellation=&stop;
  if(!SetConsoleCtrlHandler(Cancel,TRUE)) {
    std::fputs("WitcherFG could not install cancellation handling. Retry from a console.\n",stderr);return 3;
  }
  int code=3;
  try {
    std::vector<std::string> storage;
    for(int i=1;i<argc;++i) {
      const auto length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, nullptr, 0, nullptr, nullptr);
      std::string argument;
      if (length <= 0) argument = "--invalid-unicode-argument";
      else {
        argument.resize(static_cast<std::size_t>(length));
        if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, argument.data(), length, nullptr, nullptr))
          argument = "--invalid-unicode-argument";
        else argument.pop_back();
      }
      storage.push_back(std::move(argument));
    }
    std::vector<std::string_view> arguments;
    for(const auto& argument:storage)arguments.push_back(argument);
    code=wfg::RunNativeLauncher(arguments,stop.get_token());
  } catch(...) {std::fputs("WitcherFG could not initialize. Retain this console result and retry.\n",stderr);}
  // Drain any in-flight callback before the stack-owned source is destroyed.
  AcquireSRWLockExclusive(&callback_lock);
  cancellation=nullptr;
  ReleaseSRWLockExclusive(&callback_lock);
  SetConsoleCtrlHandler(Cancel,FALSE);
  return code;
}
