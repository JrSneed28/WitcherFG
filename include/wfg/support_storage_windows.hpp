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
