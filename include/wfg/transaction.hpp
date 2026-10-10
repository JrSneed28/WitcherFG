#pragma once

#include "wfg/compatibility.hpp"
#include "wfg/debug_session.hpp"

namespace wfg {

struct ProtectionJournal {
  Address page{};
  std::uint32_t original{};
};

struct TransactionResult {
  TransactionOutcome outcome{TransactionOutcome::NotStarted};
  std::optional<Error> original_error;
  std::optional<Error> recovery_error;
  std::optional<Error> abort_error;
  std::array<PointerMapping, 4> journal{};
  std::array<ProtectionJournal, 4> pages{};
  std::size_t page_count{};
  bool pointers_verified{};
  bool protections_restored{};
  bool exit_policy_restored{};
  bool immediate_helper_exit_required{};
};

Result<TransactionResult> ApplyImports(DebugSession& session, DebugStop& stop,
  IWritableMemory& memory, const ValidatedBuild& build);

}
