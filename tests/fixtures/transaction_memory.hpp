#pragma once

#include "fixtures/verified_identity.hpp"
#include "wfg/transaction.hpp"

#include <cstring>

namespace transaction_test {
using namespace wfg;
inline thread_local bool allocation_window{};
inline thread_local std::size_t allocation_count{};
inline Error Injected(std::string_view operation) {
  return {ErrorCode::MutationFailure, Stage::Transaction, operation, 123U};
}
inline void IgnoreHandle(void*) noexcept {}

class Backend final : public IDebugBackend {
public:
  std::chrono::steady_clock::time_point Now() const override { return {}; }
  Result<Unit> Attach(const OwnedProcess& process) override { owned = process.native_handle(); return Unit{}; }
  Result<Unit> SetKillOnExit(bool enabled) override {
    ++policies;
    if (policies == fail_policy || (!enabled && fail_all_false && policies > 1)) return Injected("policy_fault");
    kill = enabled;
    if (!enabled && policies > 1 && verify_safe && !verify_safe(verification_context)) unsafe_false = true;
    allocation_window = enabled;
    return Unit{};
  }
  Result<std::optional<DebugEvent>> Wait(std::chrono::milliseconds) override {
    return std::optional<DebugEvent>{{DebugEventKind::Other, 42U, 7U}};
  }
  Result<Unit> Continue(const DebugEvent&, Continuation) override { ++continues; return Unit{}; }
  Result<Unit> Detach() override { ++detaches; return Unit{}; }
  Result<Unit> AbortOwnedStartup() override {
    ++aborts;
    if (fail_abort) return Injected("abort_unverified");
    exited = true;
    return Unit{};
  }
  void* owned{};
  int policies{}, fail_policy{}, continues{}, detaches{}, aborts{};
  bool kill{}, fail_abort{}, fail_all_false{}, exited{};
  bool unsafe_false{};
  bool (*verify_safe)(void*){};
  void* verification_context{};
};

class Memory final : public IWritableMemory {
public:
  explicit Memory(Backend& backend, bool shared) : backend(backend) {
    for (std::size_t index = 0; index < 4; ++index) {
      slots[index] = 0x10000ULL + (shared ? 0 : index * 0x1000ULL) + index * 8;
      pointers[index] = 0x50000ULL + index * 8;
      protections[index] = 2U;
    }
    shared_pages = shared;
  }
  Result<Bytes> Read(Address address, std::size_t size) const override {
    if (backend.kill) ++allocating_reads_under_true;
    Bytes bytes(size);
    const auto read = ReadRaw(address, bytes);
    if (!IsOk(read)) return Failure(read);
    return bytes;
  }
  Result<Unit> ReadInto(Address address, std::span<std::byte> bytes) const override {
    ++reads;
    if (reads == fail_read) return Injected("read_fault");
    auto read = ReadRaw(address, bytes);
    if (IsOk(read) && reads == corrupt_read && !bytes.empty()) bytes[0] ^= std::byte{1};
    if (reads == factory_after_read) { manager = 0xA0000ULL; factory = 0xB0000ULL; }
    if (reads == change_original_after_read) pointers[0] ^= 8;
    return read;
  }
  Result<MemoryRegion> Query(Address address) const override {
    ++queries;
    if (queries == fail_query) return Injected("query_fault");
    if (address == 0x90100ULL || (manager != 0 && address == manager + 8)) {
      if (unreadable_factory) return Injected("factory_unreadable");
      return MemoryRegion{address, 8, true, true, false, false, 2U};
    }
    const auto page = PageIndex(address);
    if (page == 4) return Injected("query_address");
    auto protection = protections[page];
    if (queries == corrupt_query) protection ^= 2U;
    return MemoryRegion{slots[page] & ~0xFFFULL, 4096, true, true, protection == 4U,
      executable, protection};
  }
  Result<Unit> Write(Address address, std::span<const std::byte> bytes) override {
    ++writes;
    if (!backend.kill || bytes.size() != 8) ordering_violation = true;
    const auto page = PageIndex(address);
    if (page == 4 || protections[page] != 4U) return Injected("write_not_writable");
    for (std::size_t index = 0; index < 4; ++index) if (address == slots[index]) {
      if (writes == fail_write || writes == fail_second_write || (fail_rollback && writes > 2)) {
        pointers[index] ^= 0xFFULL;
        return Injected("write_fault");
      }
      std::memcpy(&pointers[index], bytes.data(), 8);
      if (writes == death_after_write) {
        death_seen = backend.kill;
        dead = true;
      }
      return Unit{};
    }
    return Injected("write_address");
  }
  Result<std::uint32_t> Protect(Address address, std::size_t size, std::uint32_t protection) override {
    ++protects;
    if (!backend.kill || size != 4096 || (address & 0xFFFULL) != 0 || protection == 0x40U)
      ordering_violation = true;
    const auto page = PageIndex(address);
    if (page == 4 || dead) return Injected("protect_address");
    const auto original = protections[page];
    protections[page] = protection;
    if (protects == fail_protect) return Injected("protect_fault");
    if (protects == wrong_previous) return 8U;
    return original;
  }
  std::size_t PageIndex(Address address) const {
    for (std::size_t index = 0; index < 4; ++index)
      if ((address & ~0xFFFULL) == (slots[index] & ~0xFFFULL)) return index;
    return 4;
  }
  Result<Unit> ReadRaw(Address address, std::span<std::byte> bytes) const {
    if (dead || bytes.size() != 8) return Injected("read_address");
    Address value{};
    if (address == 0x90100ULL) value = manager;
    else if (address == manager + 8 && manager != 0) value = factory;
    else {
      bool found{};
      for (std::size_t index = 0; index < 4; ++index) if (address == slots[index]) {
        value = pointers[index]; found = true;
      }
      if (!found) return Injected("read_address");
    }
    std::memcpy(bytes.data(), &value, 8);
    return Unit{};
  }
  Backend& backend;
  std::array<Address, 4> slots{};
  mutable std::array<Address, 4> pointers{};
  std::array<std::uint32_t, 4> protections{};
  mutable int reads{}, queries{}, allocating_reads_under_true{};
  int writes{}, protects{}, fail_read{}, corrupt_read{}, fail_query{}, corrupt_query{};
  int fail_write{}, fail_second_write{}, fail_protect{}, wrong_previous{}, death_after_write{};
  int factory_after_read{}, change_original_after_read{};
  mutable Address manager{}, factory{};
  bool unreadable_factory{};
  bool shared_pages{}, executable{}, fail_rollback{}, ordering_violation{}, dead{}, death_seen{};
};

struct Fixture {
  explicit Fixture(bool shared = false) : memory(backend, shared) {
    const auto attached = session.Attach(owned);
    if (!IsOk(attached)) throw std::runtime_error("fixture_attach");
    auto stopped = session.WaitStopped({std::chrono::steady_clock::time_point{} + std::chrono::seconds(1)});
    if (!IsOk(stopped)) throw std::runtime_error("fixture_stop");
    stop.emplace(std::move(std::get<DebugStop>(stopped)));
    build.executable_base = 0x90000ULL;
    build.factory_manager_rva = 0x100;
    build.factory_member_offset = 8;
    for (std::size_t index = 0; index < 4; ++index)
      build.mappings[index] = {memory.slots[index], memory.pointers[index], 0x60000ULL + index * 8, 0};
    originals = memory.pointers;
    backend.verification_context = this;
    backend.verify_safe = [](void* context) {
      const auto& fixture = *static_cast<Fixture*>(context);
      bool originals = true;
      bool replacements = true;
      for (std::size_t index = 0; index < 4; ++index) {
        originals = originals && fixture.memory.pointers[index] == fixture.originals[index];
        replacements = replacements && fixture.memory.pointers[index] == 0x60000ULL + index * 8;
        if (fixture.memory.protections[index] != 2U) return false;
      }
      return originals || replacements;
    };
    allocation_count = 0;
  }
  ~Fixture() { allocation_window = false; }
  TransactionResult Apply() {
    auto result = ApplyImports(session, *stop, memory, build);
    allocation_window = false;
    if (!IsOk(result)) throw std::runtime_error(std::string(Failure(result).operation.view()));
    return Value(result);
  }
  int handle{};
  OwnedProcess owned = VerifiedIdentityTestFactory::Owned(&handle, IgnoreHandle);
  Backend backend;
  DebugSession session{backend};
  Memory memory;
  std::optional<DebugStop> stop;
  ValidatedBuild build;
  std::array<Address, 4> originals{};
};
}
