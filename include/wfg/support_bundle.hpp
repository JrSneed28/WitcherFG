#pragma once

#include "wfg/core.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace wfg {

enum class SupportRunOutcome { NotStarted, Applied, RoutingAlreadyPresent, Refused, RolledBack, StartupAborted, Failed };

struct SupportRunRecord {
  std::uint32_t schema_version{};
  SupportRunOutcome outcome{SupportRunOutcome::NotStarted};
  Stage stage{Stage::Support};
  ErrorCode error{ErrorCode::SupportBundleFailure};
  OperationIdentifier operation;
  std::uint64_t elapsed_ms{};
  std::string untrusted_detail;
};

struct SupportEventRecord {
  std::uint32_t schema_version{};
  Stage stage{Stage::Support};
  ErrorCode error{ErrorCode::SupportBundleFailure};
  OperationIdentifier operation;
  std::uint64_t sequence{};
  std::string untrusted_detail;
};

struct SupportMetadata {
  std::uint32_t schema_version{};
  std::string build_id;
  std::string package_version;
  bool runtime_observation_enabled{};
  std::string untrusted_environment;
};

struct SupportInputs {
  std::uint32_t schema_version{};
  SupportMetadata metadata;
  std::span<const SupportRunRecord> runs;
  std::span<const SupportEventRecord> events;
};

struct BundleEntry {
  std::string name;
  std::vector<std::byte> bytes;
};

struct SupportReadHandle {
  std::uint64_t identity{};
  bool regular{};
  bool reparse{};
};

struct SupportRead {
  std::vector<std::byte> bytes;
  std::uint64_t identity{};
  bool regular{};
  bool reparse{};
  bool truncated{};
};

class ISupportStorage {
public:
  virtual ~ISupportStorage() = default;
  virtual Result<SupportReadHandle> OpenSanitizedSummary() = 0;
  virtual Result<SupportRead> ReadSanitizedSummary(const SupportReadHandle& handle, std::size_t maximum_bytes) = 0;
  virtual Result<SupportReadHandle> OpenRecentEvents() { return SupportReadHandle{}; }
  virtual Result<SupportRead> ReadRecentEvents(const SupportReadHandle&, std::size_t) { return Error{ErrorCode::SupportBundleFailure, Stage::Support, "ReadRecentEvents", std::nullopt, std::nullopt}; }
  virtual Result<std::filesystem::path> CommitArchiveAtomically(std::span<const std::byte> archive) = 0;
  virtual std::filesystem::path CompletedArchiveLocation() const = 0;
};

struct SupportInventoryItem {
  std::string name;
  std::string sha256;
  std::size_t bytes{};
};

struct SupportBundle {
  std::filesystem::path local_output;
  std::string payload_sha256;
  std::vector<SupportInventoryItem> entries;
  std::vector<std::string> exclusions;
  bool truncated{};
};

[[nodiscard]] Result<std::vector<BundleEntry>> BuildSupportPayload(const SupportInputs& inputs);
[[nodiscard]] Result<SupportBundle> CollectSupport(const SupportInputs& inputs, ISupportStorage& storage);

#ifdef WFG_SUPPORT_TESTING
[[nodiscard]] Result<Unit> ValidateSupportArchiveEntriesForTest(std::span<const BundleEntry> entries);
#endif

}  // namespace wfg
