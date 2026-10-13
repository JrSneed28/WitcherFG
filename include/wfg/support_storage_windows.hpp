#pragma once

#include "wfg/support_bundle.hpp"
#include <memory>

namespace wfg {
class WindowsSupportStorage final : public ISupportStorage {
public:
  WindowsSupportStorage();
  ~WindowsSupportStorage() override;
  WindowsSupportStorage(const WindowsSupportStorage&) = delete;
  WindowsSupportStorage& operator=(const WindowsSupportStorage&) = delete;
#ifdef WFG_SUPPORT_TESTING
  explicit WindowsSupportStorage(std::filesystem::path disposable_root);
  bool fail_before_write{};
  bool fail_before_rename{};
  std::optional<std::uint32_t> drive_type_override;
  std::optional<std::wstring> device_target_override;
  bool fail_before_root_open{};
  std::size_t root_open_attempts{};
  std::size_t locality_probe_count{};
  std::uint32_t known_folder_flags{};
  Result<Unit> ValidateLocalRootForTest();
#endif
  Result<SupportReadHandle> OpenSanitizedSummary() override;
  Result<SupportRead> ReadSanitizedSummary(const SupportReadHandle&, std::size_t) override;
  Result<SupportReadHandle> OpenRecentEvents() override;
  Result<SupportRead> ReadRecentEvents(const SupportReadHandle&, std::size_t) override;
  Result<std::filesystem::path> CommitArchiveAtomically(std::span<const std::byte>) override;
  std::filesystem::path CompletedArchiveLocation() const override;
private:
  struct State;
  std::unique_ptr<State> state_;
};
}
