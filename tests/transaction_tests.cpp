#include "support.hpp"
#include "fixtures/transaction_memory.hpp"

#include <cstdlib>
#include <new>

void* operator new(std::size_t size) {
  if (transaction_test::allocation_window) ++transaction_test::allocation_count;
  if (auto* memory = std::malloc(size == 0 ? 1 : size)) return memory;
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace {
using namespace transaction_test;
void HeldAndQuiet(Fixture& fixture) {
  CHECK(fixture.backend.continues == 0);
  CHECK(fixture.backend.detaches == 0);
  CHECK(!fixture.memory.ordering_violation);
  CHECK(!fixture.backend.unsafe_false);
  CHECK(fixture.memory.allocating_reads_under_true == 0);
  CHECK(allocation_count == 0);
}
void Restored(Fixture& fixture, const TransactionResult& result) {
  CHECK(result.outcome == TransactionOutcome::RolledBack);
  CHECK(result.original_error.has_value());
  CHECK(result.pointers_verified);
  CHECK(result.protections_restored);
  CHECK(result.exit_policy_restored);
  CHECK(!result.immediate_helper_exit_required);
  CHECK(fixture.memory.pointers == fixture.originals);
  CHECK((fixture.memory.protections == std::array<std::uint32_t, 4>{2U, 2U, 2U, 2U}));
  CHECK(!fixture.backend.kill);
  CHECK(fixture.session.Holds(*fixture.stop));
  HeldAndQuiet(fixture);
}
}

TEST("transaction.all_four_commit_shared_and_distinct_pages") {
  for (const bool shared : {false, true}) {
    Fixture fixture(shared);
    const auto result = fixture.Apply();
    CHECK(result.outcome == TransactionOutcome::Committed);
    CHECK(!result.original_error);
    CHECK(result.pointers_verified && result.protections_restored && result.exit_policy_restored);
    CHECK(result.page_count == (shared ? 1U : 4U));
    CHECK(fixture.memory.protects == (shared ? 2 : 8));
    CHECK(fixture.memory.writes == 4);
    for (std::size_t index = 0; index < 4; ++index) {
      CHECK(fixture.memory.pointers[index] == 0x60000ULL + index * 8);
      CHECK(result.journal[index].original == fixture.originals[index]);
    }
    CHECK(!fixture.backend.kill);
    CHECK(fixture.session.Holds(*fixture.stop));
    HeldAndQuiet(fixture);
  }
}

TEST("transaction.partial_write_never_resumes") {
  for (int fault = 1; fault <= 4; ++fault) {
    Fixture fixture;
    fixture.memory.fail_write = fault;
    const auto result = fixture.Apply();
    Restored(fixture, result);
    CHECK(result.original_error->operation.view() == "write_fault");
    CHECK(result.original_error->affected_slot == static_cast<std::size_t>(fault - 1));
    CHECK(result.original_error->native_code == 123U);
  }
}

TEST("transaction.every_protect_and_restore_fault_rolls_back") {
  for (const bool shared : {false, true}) {
    for (int fault = 1; fault <= (shared ? 2 : 8); ++fault) {
      Fixture fixture(shared);
      fixture.memory.fail_protect = fault;
      Restored(fixture, fixture.Apply());
    }
  }
}

TEST("transaction.every_readback_fault_and_mismatch_rolls_back") {
  for (bool corrupt : {false, true}) {
    for (int slot = 0; slot < 4; ++slot) {
      Fixture fixture;
      if (corrupt) fixture.memory.corrupt_read = 9 + slot;
      else fixture.memory.fail_read = 9 + slot;
      const auto result = fixture.Apply();
      Restored(fixture, result);
      CHECK(result.original_error->affected_slot == static_cast<std::size_t>(slot));
    }
  }
}

TEST("transaction.every_query_fault_refuses_or_rolls_back") {
  Fixture baseline;
  baseline.Apply();
  const int count = baseline.memory.queries;
  CHECK(count >= 16);
  for (int fault = 1; fault <= count; ++fault) {
    Fixture fixture;
    fixture.memory.fail_query = fault;
    const auto result = fixture.Apply();
    if (result.outcome == TransactionOutcome::NotStarted) {
      CHECK(fixture.memory.writes == 0 && fixture.memory.protects == 0);
      CHECK(fixture.backend.policies == 1);
      HeldAndQuiet(fixture);
    } else Restored(fixture, result);
  }
}

TEST("transaction.changed_original_and_late_factory_refuse_before_true") {
  for (std::size_t slot = 0; slot < 4; ++slot) {
    Fixture fixture;
    fixture.memory.pointers[slot] ^= 8;
    const auto result = fixture.Apply();
    CHECK(result.outcome == TransactionOutcome::NotStarted);
    CHECK(result.original_error->code == ErrorCode::UnexpectedImports);
    CHECK(result.original_error->affected_slot == slot);
    CHECK(fixture.backend.policies == 1 && fixture.memory.protects == 0);
  }
  Fixture fixture;
  fixture.memory.manager = 0xA0000ULL;
  fixture.memory.factory = 0xB0000ULL;
  const auto result = fixture.Apply();
  CHECK(result.outcome == TransactionOutcome::NotStarted);
  CHECK(result.original_error->code == ErrorCode::StartupTooLate);
  CHECK(fixture.backend.policies == 1 && fixture.memory.writes == 0);
}

TEST("transaction.foreign_and_inactive_tokens_cannot_arm") {
  Fixture fixture;
  Fixture foreign;
  const auto result = REQUIRE_OK(ApplyImports(fixture.session, *foreign.stop, fixture.memory, fixture.build));
  CHECK(result.outcome == TransactionOutcome::NotStarted);
  CHECK(fixture.backend.policies == 1);
  REQUIRE_OK(fixture.session.Continue(*fixture.stop, fixture.stop->continuation()));
  const auto inactive = fixture.Apply();
  CHECK(inactive.outcome == TransactionOutcome::NotStarted);
  CHECK(fixture.backend.policies == 1);
}

TEST("transaction.both_policy_changes_are_fail_closed") {
  Fixture arm;
  arm.backend.fail_policy = 2;
  const auto refused = arm.Apply();
  CHECK(refused.outcome == TransactionOutcome::NotStarted);
  CHECK(arm.memory.protects == 0 && arm.memory.writes == 0);
  CHECK(!arm.backend.kill);
  Fixture finish;
  finish.backend.fail_policy = 3;
  const auto result = finish.Apply();
  Restored(finish, result);
  CHECK(result.original_error->operation.view() == "policy_fault");
}

TEST("transaction.failed_restore_aborts_owned_startup") {
  Fixture fixture;
  fixture.memory.fail_write = 2;
  fixture.memory.fail_rollback = true;
  const auto result = fixture.Apply();
  CHECK(result.outcome == TransactionOutcome::Aborted);
  CHECK(result.original_error->operation.view() == "write_fault");
  CHECK(result.recovery_error.has_value());
  CHECK(fixture.backend.aborts == 1 && fixture.backend.exited);
  CHECK(fixture.backend.owned == fixture.owned.native_handle());
  CHECK(!fixture.stop->active());
  CHECK(!result.exit_policy_restored && fixture.backend.kill);
  HeldAndQuiet(fixture);
}

TEST("transaction.unverified_abort_requires_immediate_helper_exit_under_true") {
  Fixture fixture;
  fixture.memory.fail_write = 2;
  fixture.memory.fail_rollback = true;
  fixture.backend.fail_abort = true;
  const auto unsafe = fixture.Apply();
  CHECK(unsafe.outcome == TransactionOutcome::Failed);
  CHECK(unsafe.immediate_helper_exit_required);
  CHECK(!unsafe.exit_policy_restored && fixture.backend.kill);
  CHECK(unsafe.original_error->operation.view() == "write_fault");
  CHECK(unsafe.abort_error.has_value());
  CHECK(unsafe.abort_error->operation.view() == "abort_unverified");
  CHECK(fixture.session.Holds(*fixture.stop));
  REQUIRE_ERROR(fixture.session.SetKillOnExit(false), ErrorCode::DebugFailure);
  REQUIRE_ERROR(fixture.session.Continue(*fixture.stop, fixture.stop->continuation()), ErrorCode::DebugFailure);
  HeldAndQuiet(fixture);
}

TEST("transaction.persistent_final_false_failure_aborts_without_resuming") {
  Fixture fixture;
  fixture.backend.fail_all_false = true;
  const auto result = fixture.Apply();
  CHECK(result.outcome == TransactionOutcome::Aborted);
  CHECK(result.original_error->operation.view() == "policy_fault");
  CHECK(fixture.memory.pointers == fixture.originals);
  CHECK(fixture.backend.kill);
  HeldAndQuiet(fixture);
}

TEST("transaction.helper_death_after_each_write_remains_guarded") {
  for (int fault = 1; fault <= 4; ++fault) {
    Fixture fixture;
    fixture.memory.death_after_write = fault;
    const auto result = fixture.Apply();
    CHECK(fixture.memory.death_seen);
    CHECK(result.outcome == TransactionOutcome::Aborted);
    CHECK(fixture.backend.kill && fixture.backend.policies == 2);
    HeldAndQuiet(fixture);
  }
}

TEST("transaction.executable_pages_and_duplicate_slots_refuse") {
  Fixture executable;
  executable.memory.executable = true;
  CHECK(executable.Apply().outcome == TransactionOutcome::NotStarted);
  CHECK(executable.memory.protects == 0);
  Fixture duplicate;
  duplicate.build.mappings[1] = duplicate.build.mappings[0];
  CHECK(duplicate.Apply().outcome == TransactionOutcome::NotStarted);
  CHECK(duplicate.memory.protects == 0);
}

TEST("transaction.every_prior_protection_mismatch_rolls_back") {
  for (const bool shared : {false, true}) {
    for (int fault = 1; fault <= (shared ? 2 : 8); ++fault) {
      Fixture fixture(shared);
      fixture.memory.wrong_previous = fault;
      Restored(fixture, fixture.Apply());
    }
  }
}

TEST("transaction.every_pointer_read_fault_refuses_or_rolls_back") {
  for (int fault = 1; fault <= 16; ++fault) {
    Fixture fixture;
    fixture.memory.fail_read = fault;
    const auto result = fixture.Apply();
    if (fault <= 8) {
      CHECK(result.outcome == TransactionOutcome::NotStarted);
      CHECK(result.original_error.has_value());
      CHECK(fixture.backend.policies == 1 && fixture.memory.protects == 0);
      HeldAndQuiet(fixture);
    } else Restored(fixture, result);
  }
}

TEST("transaction.every_protection_query_mismatch_refuses_or_rolls_back") {
  Fixture baseline;
  baseline.Apply();
  const int count = baseline.memory.queries;
  CHECK(count >= 16);
  for (int fault = 1; fault <= count; ++fault) {
    if (fault == 9) {
      // Query 9 is CheckFactory's readability probe on the factory manager address. The
      // fixture answers that address from a dedicated branch that returns before
      // corrupt_query is applied, so corrupting this index is a no-op by construction and
      // the transaction commits normally. Assert that instead of skipping silently.
      Fixture unaffected;
      unaffected.memory.corrupt_query = fault;
      CHECK(unaffected.Apply().outcome == TransactionOutcome::Committed);
      continue;
    }
    Fixture fixture;
    fixture.memory.corrupt_query = fault;
    const auto result = fixture.Apply();
    if (result.outcome == TransactionOutcome::NotStarted) {
      CHECK(fixture.backend.policies == 1 && fixture.memory.protects == 0);
      HeldAndQuiet(fixture);
    } else Restored(fixture, result);
  }
}

TEST("transaction.copy_on_write_iat_page_refuses_before_any_write") {
  // An IAT page still reporting PAGE_WRITECOPY has not been written by the loader, so its
  // slots cannot be snapped yet -- that is the mid-snap state ClassifyImports retries, not a
  // state the transaction may enter. Entering it is unrecoverable: Commit protects the page to
  // PAGE_READWRITE (which really succeeds and returns previous=8) but VirtualQuery keeps
  // reporting 8 until the first write, so CheckPage's exact-equality test fails before any
  // write and the run lands in Recover with protections_restored false. Refuse by name instead.
  for (const bool shared : {false, true}) {
    Fixture fixture(shared, 8U);
    const auto result = fixture.Apply();
    CHECK(result.outcome == TransactionOutcome::NotStarted);
    CHECK(result.original_error.has_value());
    CHECK(result.original_error->operation.view() == "invalid_iat_page");
    CHECK(result.original_error->stage == Stage::import_transaction);
    CHECK(fixture.memory.writes == 0 && fixture.memory.protects == 0);
    CHECK(fixture.memory.pointers == fixture.originals);
    CHECK(!fixture.backend.kill);
    HeldAndQuiet(fixture);
  }
}

TEST("transaction.every_rollback_fault_retains_guard_and_initiating_error") {
  for (const bool shared : {false, true}) {
    Fixture baseline(shared);
    baseline.memory.fail_write = 2;
    const auto restored = baseline.Apply();
    Restored(baseline, restored);
    const int page_count = shared ? 1 : 4;
    // Prologue queries: Capture issues one per mapping (4) plus one per page, CheckFactory
    // issues one (ReadValue's Readable probe on the factory manager address), and Commit
    // issues one per page before the injected write fault -- 5 + 2 * page_count in total,
    // so the rollback phase begins at the next index. The previous constant 9 + page_count
    // + 1 was fitted to the four-page layout and coincidentally correct only there; for the
    // shared-page layout it exceeded the total query count and silently emptied the query
    // loops (kinds 3 and 5). The CHECK(last >= first) below keeps that from recurring.
    const int first_query = 5 + 2 * page_count + 1;
    for (int kind = 0; kind < 6; ++kind) {
      const int first = kind == 0 ? 3 : kind == 1 ? 10 : kind == 2 ? page_count + 1 :
        kind == 3 ? first_query : kind == 4 ? 10 : first_query;
      const int last = kind == 0 ? baseline.memory.writes : kind == 1 || kind == 4 ? baseline.memory.reads :
        kind == 2 ? baseline.memory.protects : baseline.memory.queries;
      CHECK(last >= first);
      for (int fault = first; fault <= last; ++fault) {
        Fixture fixture(shared);
        fixture.memory.fail_write = 2;
        if (kind == 0) fixture.memory.fail_second_write = fault;
        if (kind == 1) fixture.memory.fail_read = fault;
        if (kind == 2) fixture.memory.fail_protect = fault;
        if (kind == 3) fixture.memory.fail_query = fault;
        if (kind == 4) fixture.memory.corrupt_read = fault;
        if (kind == 5) fixture.memory.corrupt_query = fault;
        const auto result = fixture.Apply();
        CHECK(result.outcome == TransactionOutcome::Aborted);
        CHECK(result.original_error.has_value());
        CHECK(result.original_error->operation.view() == "write_fault");
        CHECK(result.original_error->affected_slot == 1U);
        CHECK(result.recovery_error.has_value());
        CHECK(!result.exit_policy_restored && fixture.backend.kill);
        HeldAndQuiet(fixture);
      }
    }
  }
}

TEST("transaction.verified_original_rollback_with_failed_false_aborts") {
  Fixture fixture;
  fixture.memory.fail_write = 2;
  fixture.backend.fail_policy = 3;
  const auto result = fixture.Apply();
  CHECK(result.outcome == TransactionOutcome::Aborted);
  CHECK(result.original_error->operation.view() == "write_fault");
  CHECK(result.recovery_error->operation.view() == "policy_fault");
  CHECK(fixture.memory.pointers == fixture.originals);
  CHECK(!result.exit_policy_restored && fixture.backend.kill);
  HeldAndQuiet(fixture);
}

namespace {
template <class Factory> std::optional<ObservedProcess> ParameterizedObserved(void* handle) {
  if constexpr (requires { Factory::Observed(handle, IgnoreHandle, 77U, 900ULL); })
    return Factory::Observed(handle, IgnoreHandle, 77U, 900ULL);
  else return {};
}
}

TEST("transaction.shared_identity_factory_preserves_defaults_and_explicit_pin_facts") {
  int handle{};
  auto default_observed = VerifiedIdentityTestFactory::Observed(&handle, IgnoreHandle);
  CHECK(default_observed.pid() == 42U && default_observed.creation_time() == 100ULL);
  auto owned = VerifiedIdentityTestFactory::Owned(&handle, IgnoreHandle);
  CHECK(owned.pid() == 42U && owned.creation_time() == 100ULL);
  auto explicit_observed = ParameterizedObserved<VerifiedIdentityTestFactory>(&handle);
  CHECK(explicit_observed.has_value());
  CHECK(explicit_observed->pid() == 77U && explicit_observed->creation_time() == 900ULL);
  CHECK(explicit_observed->native_handle() == &handle);
}

TEST("transaction.factory_appearing_during_journal_capture_is_rechecked") {
  Fixture fixture;
  fixture.memory.factory_after_read = 8;
  const auto result = fixture.Apply();
  CHECK(result.outcome == TransactionOutcome::NotStarted);
  CHECK(result.original_error.has_value());
  CHECK(result.original_error->code == ErrorCode::StartupTooLate);
  CHECK(fixture.memory.writes == 0 && fixture.memory.protects == 0);
  CHECK(fixture.backend.policies == 1);
  HeldAndQuiet(fixture);
}

TEST("transaction.changed_original_after_initial_capture_is_reverified") {
  Fixture fixture;
  fixture.memory.change_original_after_read = 4;
  const auto result = fixture.Apply();
  CHECK(result.outcome == TransactionOutcome::NotStarted);
  CHECK(result.original_error.has_value());
  CHECK(result.original_error->affected_slot == 0U);
  CHECK(fixture.memory.writes == 0 && fixture.memory.protects == 0);
  CHECK(fixture.backend.policies == 1);
  HeldAndQuiet(fixture);
}

TEST("transaction.unreadable_factory_layout_refuses_before_true") {
  Fixture fixture;
  fixture.memory.unreadable_factory = true;
  const auto result = fixture.Apply();
  CHECK(result.outcome == TransactionOutcome::NotStarted);
  CHECK(result.original_error.has_value());
  CHECK(result.original_error->code == ErrorCode::UnreadableMemory);
  CHECK(fixture.backend.policies == 1 && fixture.memory.protects == 0);
  HeldAndQuiet(fixture);
}

TEST("transaction.null_factory_member_permits_verified_commit") {
  Fixture fixture;
  fixture.memory.manager = 0xA0000ULL;
  const auto result = fixture.Apply();
  CHECK(result.outcome == TransactionOutcome::Committed);
  CHECK(result.pointers_verified && result.protections_restored && result.exit_policy_restored);
  HeldAndQuiet(fixture);
}

TEST("transaction.all_commit_and_readback_faults_cover_shared_pages") {
  for (int slot = 0; slot < 4; ++slot) {
    Fixture written(true);
    written.memory.fail_write = slot + 1;
    Restored(written, written.Apply());
    for (const bool corrupt : {false, true}) {
      Fixture read(true);
      if (corrupt) read.memory.corrupt_read = 9 + slot;
      else read.memory.fail_read = 9 + slot;
      Restored(read, read.Apply());
    }
  }
}

TEST("transaction.original_journal_is_preserved_after_rollback_and_unsafe_abort") {
  for (const bool abort : {false, true}) {
    Fixture fixture;
    fixture.memory.fail_write = 3;
    fixture.memory.fail_rollback = abort;
    fixture.backend.fail_abort = abort;
    const auto result = fixture.Apply();
    CHECK(result.outcome == (abort ? TransactionOutcome::Failed : TransactionOutcome::RolledBack));
    for (std::size_t slot = 0; slot < 4; ++slot) {
      CHECK(result.journal[slot].slot == fixture.memory.slots[slot]);
      CHECK(result.journal[slot].original == 0x50000ULL + slot * 8);
      CHECK(result.journal[slot].replacement == 0x60000ULL + slot * 8);
      CHECK(result.pages[slot].page == 0x10000ULL + slot * 0x1000);
      CHECK(result.pages[slot].original == 2U);
    }
    HeldAndQuiet(fixture);
  }
}
