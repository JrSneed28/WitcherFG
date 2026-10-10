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

class EnvironmentBackendFixture final : public wfg::process_detail::IEnvironmentBackend {
public:
  wfg::Result<wfg::process_detail::OsBuild> QueryOsBuild() override { return os; }
  wfg::Result<std::optional<wfg::process_detail::AdapterFacts>> QueryAdapter(std::uint32_t index) override {
    ++adapter_queries;
    if (adapter_error) return *adapter_error;
    if (repeat_adapter || index < adapters.size()) return std::optional{adapters[repeat_adapter ? 0 : index]};
    return std::optional<wfg::process_detail::AdapterFacts>{};
  }
  wfg::Result<std::optional<std::uint32_t>> QueryHagsMode() override { return hags; }
  wfg::Result<wfg::process_detail::OsBuild> os{wfg::process_detail::OsBuild{10, 0, 26200}};
  std::vector<wfg::process_detail::AdapterFacts> adapters{{"NVIDIA Test GPU", 0x10de, 0x1234, std::string{"32.0.15.1234"}}, {"Intel Test GPU", 0x8086, 0x5678, std::string{"31.0.101.1234"}}};
  wfg::Result<std::optional<std::uint32_t>> hags{std::optional<std::uint32_t>{2}};
  std::optional<wfg::Error> adapter_error;
  bool repeat_adapter{};
  unsigned adapter_queries{};
};

class ActivationOperationsFixture final : public wfg::process_detail::IActivationOperations {
public:
  std::chrono::steady_clock::time_point Now() const override { return std::chrono::steady_clock::time_point{} + elapsed; }
  wfg::Result<wfg::Unit> Prepare() override { elapsed += preparation_time; return preparation; }
  wfg::Result<wfg::Unit> Launch() override { ++launch_calls; return wfg::Unit{}; }
  std::chrono::milliseconds elapsed{};
  std::chrono::milliseconds preparation_time{};
  wfg::Result<wfg::Unit> preparation{wfg::Unit{}};
  unsigned launch_calls{};
};

class LocalDirectoryFixture final : public wfg::process_detail::ILocalDirectory {
public:
  bool AtEnd() const override { return index >= paths.size(); }
  std::filesystem::path Current() const override { return paths[index]; }
  void Increment(std::error_code& error) override {
    ++index;
    if (fail_increment) error = std::make_error_code(std::errc::permission_denied);
  }
  std::vector<std::filesystem::path> paths{L"first.dll", L"preserved.dll"};
  std::size_t index{};
  bool fail_increment{};
};
