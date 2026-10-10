#pragma once

#include "wfg/windows_process.hpp"

class ProcessBackendFixture final : public wfg::IProcessBackend {
public:
  std::chrono::steady_clock::time_point Now() const override { return std::chrono::steady_clock::time_point{} + elapsed; }
  void Sleep(std::chrono::milliseconds duration) override {
    elapsed += duration;
    ++sleeps;
    max_sleep = std::max(max_sleep, duration);
  }
  wfg::Result<wfg::Unit> Install() override { events.push_back("install"); return wfg::Unit{}; }
  wfg::Result<wfg::Unit> CheckRegistration() override {
    if (!registered) return wfg::Error{registration_error, wfg::Stage::Discover, "registration_changed"};
    return wfg::Unit{};
  }
  wfg::Result<wfg::Unit> Activate() override { activated = true; events.push_back("activate"); return wfg::Unit{}; }
  bool ActivationRequested() const override { return activated; }
  wfg::Result<std::vector<wfg::Candidate>> Enumerate() override { elapsed += enumeration_time; return candidates; }
  std::chrono::milliseconds elapsed{};
  std::chrono::milliseconds max_sleep{};
  std::chrono::milliseconds enumeration_time{};
  unsigned sleeps{};
  unsigned attach_count{};
  bool registered{true};
  bool activated{};
  wfg::ErrorCode registration_error{wfg::ErrorCode::IdentityChanged};
  std::vector<std::string> events;
  std::vector<wfg::Candidate> candidates;
};

struct ProcessIdentityFixture {
  wfg::LaunchIdentity launch{100, 7, L"current_user", L"registered_package", L"D:\\XboxGames\\Witcher\\bin\\witcher3.exe", true};
  wfg::Candidate candidate{42, 101, 7, L"current_user", L"registered_package", L"D:\\XboxGames\\Witcher\\bin\\witcher3.exe", 42, 101};
  ProcessBackendFixture backend;
};
