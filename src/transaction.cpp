#include "wfg/transaction.hpp"

#include <limits>

namespace wfg {
namespace {
constexpr std::size_t PageSize = 4096;
constexpr std::uint32_t ReadWrite = 4;

Error TransactionError(std::string_view operation, std::optional<std::size_t> slot = {},
  ErrorCode code = ErrorCode::MutationFailure) {
  return {code, Stage::import_transaction, operation, {}, slot};
}
Error AtSlot(Error error, std::optional<std::size_t> slot = {}) {
  error.stage = Stage::import_transaction;
  error.affected_slot = slot;
  return error;
}
bool Covers(const MemoryRegion& region, Address address, std::size_t size) {
  return region.committed && region.readable && !region.executable && address >= region.base &&
    size <= region.size && address - region.base <= region.size - size;
}
Result<Unit> CheckPage(const IWritableMemory& memory, const ProtectionJournal& page,
  std::uint32_t protection) {
  const auto queried = memory.Query(page.page);
  if (!IsOk(queried)) return AtSlot(Failure(queried));
  if (!Covers(Value(queried), page.page, PageSize) || Value(queried).protection != protection)
    return TransactionError("protection_verification");
  return Unit{};
}
Result<Unit> CheckPointer(const IWritableMemory& memory, const PointerMapping& mapping,
  Address expected, std::size_t slot) {
  Address actual{};
  const auto read = memory.ReadInto(mapping.slot, std::as_writable_bytes(std::span(&actual, 1)));
  if (!IsOk(read)) return AtSlot(Failure(read), slot);
  if (actual != expected) return TransactionError("pointer_verification", slot);
  return Unit{};
}

class Transaction {
public:
  Transaction(DebugSession& session, IWritableMemory& memory, const ValidatedBuild& build)
      : session_(session), memory_(memory) { result_.journal = build.mappings; }

  TransactionResult Refuse(Error error) {
    result_.original_error = AtSlot(error, error.affected_slot);
    return result_;
  }

  Result<Unit> Capture() {
    for (std::size_t slot = 0; slot < result_.journal.size(); ++slot) {
      const auto& mapping = result_.journal[slot];
      if (mapping.slot == 0 || mapping.slot % sizeof(Address) != 0 || mapping.original == 0 ||
          mapping.replacement == 0 || mapping.original == mapping.replacement ||
          mapping.slot > std::numeric_limits<Address>::max() - (sizeof(Address) - 1))
        return TransactionError("invalid_mapping", slot, ErrorCode::UnexpectedImports);
      for (std::size_t earlier = 0; earlier < slot; ++earlier)
        if (result_.journal[earlier].slot == mapping.slot)
          return TransactionError("duplicate_slot", slot, ErrorCode::UnexpectedImports);
      const Address page = mapping.slot & ~static_cast<Address>(PageSize - 1);
      const auto region = memory_.Query(mapping.slot);
      if (!IsOk(region)) return AtSlot(Failure(region), slot);
      if (!Covers(Value(region), page, PageSize) ||
          (Value(region).protection != 2 && Value(region).protection != 4 && Value(region).protection != 8))
        return TransactionError("invalid_iat_page", slot);
      std::size_t index{};
      while (index < result_.page_count && result_.pages[index].page != page) ++index;
      if (index == result_.page_count) {
        result_.pages[result_.page_count++] = {page, Value(region).protection};
      } else if (result_.pages[index].original != Value(region).protection) {
        return TransactionError("page_changed_at_capture", slot);
      }
      const auto pointer = CheckPointer(memory_, mapping, mapping.original, slot);
      if (!IsOk(pointer)) {
        auto error = Failure(pointer);
        if (error.operation.view() == "pointer_verification") {
          error.code = ErrorCode::UnexpectedImports;
          error.operation = "original_changed_at_capture";
        }
        return error;
      }
    }
    for (std::size_t slot = 0; slot < result_.journal.size(); ++slot) {
      const auto pointer = CheckPointer(memory_, result_.journal[slot], result_.journal[slot].original, slot);
      if (!IsOk(pointer)) return AtSlot(Failure(pointer), slot);
    }
    for (std::size_t page = 0; page < result_.page_count; ++page) {
      const auto checked = CheckPage(memory_, result_.pages[page], result_.pages[page].original);
      if (!IsOk(checked)) return Failure(checked);
    }
    return Unit{};
  }

  TransactionResult Commit() {
    for (std::size_t page = 0; page < result_.page_count; ++page) {
      const auto& entry = result_.pages[page];
      const auto changed = memory_.Protect(entry.page, PageSize, ReadWrite);
      if (!IsOk(changed)) return Recover(AtSlot(Failure(changed)));
      if (Value(changed) != entry.original) return Recover(TransactionError("protection_changed"));
      const auto checked = CheckPage(memory_, entry, ReadWrite);
      if (!IsOk(checked)) return Recover(Failure(checked));
    }
    for (std::size_t slot = 0; slot < result_.journal.size(); ++slot) {
      const auto& mapping = result_.journal[slot];
      const auto written = memory_.Write(mapping.slot, std::as_bytes(std::span(&mapping.replacement, 1)));
      if (!IsOk(written)) return Recover(AtSlot(Failure(written), slot));
      const auto checked = CheckPointer(memory_, mapping, mapping.replacement, slot);
      if (!IsOk(checked)) return Recover(Failure(checked));
    }
    if (!RestorePages()) return Recover(*result_.recovery_error);
    const auto verified = VerifyPointers(true);
    if (!IsOk(verified)) return Recover(Failure(verified));
    result_.pointers_verified = true;
    const auto policy = session_.SetKillOnExit(false);
    if (!IsOk(policy)) return Recover(AtSlot(Failure(policy)));
    result_.exit_policy_restored = true;
    result_.outcome = TransactionOutcome::Committed;
    return result_;
  }

private:
  void RecoveryError(Error error) {
    if (!result_.recovery_error) result_.recovery_error = error;
  }
  Result<Unit> VerifyPointers(bool replacement) {
    for (std::size_t slot = 0; slot < result_.journal.size(); ++slot) {
      const auto& mapping = result_.journal[slot];
      const auto checked = CheckPointer(memory_, mapping, replacement ? mapping.replacement : mapping.original, slot);
      if (!IsOk(checked)) return Failure(checked);
    }
    return Unit{};
  }
  bool RestorePages() {
    bool restored = true;
    for (std::size_t page = 0; page < result_.page_count; ++page) {
      const auto& entry = result_.pages[page];
      const auto changed = memory_.Protect(entry.page, PageSize, entry.original);
      if (!IsOk(changed)) { RecoveryError(AtSlot(Failure(changed))); restored = false; }
      else if (Value(changed) != ReadWrite) {
        RecoveryError(TransactionError("restore_previous_protection"));
        restored = false;
      }
      const auto checked = CheckPage(memory_, entry, entry.original);
      if (!IsOk(checked)) { RecoveryError(Failure(checked)); restored = false; }
    }
    for (std::size_t page = 0; page < result_.page_count; ++page) {
      const auto checked = CheckPage(memory_, result_.pages[page], result_.pages[page].original);
      if (!IsOk(checked)) { RecoveryError(Failure(checked)); restored = false; }
    }
    result_.protections_restored = restored;
    return restored;
  }
  TransactionResult Recover(Error initiating) {
    result_.original_error = initiating;
    result_.recovery_error.reset();
    result_.pointers_verified = false;
    result_.protections_restored = false;
    bool recovered = true;
    for (std::size_t page = 0; page < result_.page_count; ++page) {
      const auto& entry = result_.pages[page];
      const auto changed = memory_.Protect(entry.page, PageSize, ReadWrite);
      if (!IsOk(changed)) { RecoveryError(AtSlot(Failure(changed))); recovered = false; }
      const auto checked = CheckPage(memory_, entry, ReadWrite);
      if (!IsOk(checked)) { RecoveryError(Failure(checked)); recovered = false; }
    }
    for (std::size_t slot = 0; slot < result_.journal.size(); ++slot) {
      const auto& mapping = result_.journal[slot];
      const auto written = memory_.Write(mapping.slot, std::as_bytes(std::span(&mapping.original, 1)));
      if (!IsOk(written)) { RecoveryError(AtSlot(Failure(written), slot)); recovered = false; }
      const auto checked = CheckPointer(memory_, mapping, mapping.original, slot);
      if (!IsOk(checked)) { RecoveryError(Failure(checked)); recovered = false; }
    }
    const auto pointers = VerifyPointers(false);
    result_.pointers_verified = IsOk(pointers);
    if (!IsOk(pointers)) { RecoveryError(Failure(pointers)); recovered = false; }
    if (!RestorePages()) recovered = false;
    if (recovered) {
      const auto policy = session_.SetKillOnExit(false);
      if (IsOk(policy)) {
        result_.exit_policy_restored = true;
        result_.outcome = TransactionOutcome::RolledBack;
        return result_;
      }
      RecoveryError(AtSlot(Failure(policy)));
    }
    const auto aborted = session_.AbortOwnedStartup();
    result_.outcome = IsOk(aborted) ? TransactionOutcome::Aborted : TransactionOutcome::Failed;
    result_.immediate_helper_exit_required = !IsOk(aborted);
    if (!IsOk(aborted)) result_.abort_error = AtSlot(Failure(aborted));
    return result_;
  }
  DebugSession& session_;
  IWritableMemory& memory_;
  TransactionResult result_;
};
}

Result<TransactionResult> ApplyImports(DebugSession& session, DebugStop& stop,
  IWritableMemory& memory, const ValidatedBuild& build) {
  Transaction transaction(session, memory, build);
  if (!session.Holds(stop)) return transaction.Refuse(TransactionError("transaction_stop", {}, ErrorCode::DebugFailure));
  if (build.routing != Routing::Native)
    return transaction.Refuse(TransactionError("transaction_routing", {}, ErrorCode::UnexpectedImports));
  const auto captured = transaction.Capture();
  if (!IsOk(captured)) return transaction.Refuse(Failure(captured));
  const auto factory = CheckFactory(memory, build);
  if (!IsOk(factory)) return transaction.Refuse(Failure(factory));
  if (Value(factory) != FactoryState::NotCreated)
    return transaction.Refuse(TransactionError("factory_created", {}, ErrorCode::StartupTooLate));
  if (!session.Holds(stop)) return transaction.Refuse(TransactionError("transaction_stop", {}, ErrorCode::DebugFailure));
  const auto armed = session.SetKillOnExit(true);
  if (!IsOk(armed)) return transaction.Refuse(Failure(armed));
  return transaction.Commit();
}

}
