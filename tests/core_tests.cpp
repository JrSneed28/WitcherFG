#include "support.hpp"

#include <string_view>
#include <type_traits>

namespace wfg {
class VerifiedIdentityTestFactory {
public:
  using Closer = void (*)(void*) noexcept;
  static OwnedProcess Owned(void* handle, Closer closer) {
    return OwnedProcess(handle, 42U, 100ULL, "test_user", "test_session", "test_package", closer);
  }
  static ObservedProcess Observed(void* handle, Closer closer) {
    return ObservedProcess(handle, 42U, 100ULL, closer);
  }
};
}

namespace {
struct HandleState { unsigned closed{}; };
void CloseHandle(void* handle) noexcept { ++static_cast<HandleState*>(handle)->closed; }
template <class Process> void SelfMove(Process& process) {
  auto* alias = &process;
  process = std::move(*alias);
}
}

TEST("pe.core_owned_handle_closes_exactly_once_after_move") {
  HandleState handle;
  {
    auto original = wfg::VerifiedIdentityTestFactory::Owned(&handle, CloseHandle);
    auto moved = std::move(original);
    CHECK(original.native_handle() == nullptr);
    CHECK(moved.native_handle() == &handle);
    CHECK(handle.closed == 0);
  }
  CHECK(handle.closed == 1);
}

TEST("pe.core_move_assignment_releases_destination_immediately") {
  HandleState first, second;
  {
    auto destination = wfg::VerifiedIdentityTestFactory::Owned(&first, CloseHandle);
    auto source = wfg::VerifiedIdentityTestFactory::Owned(&second, CloseHandle);
    destination = std::move(source);
    CHECK(first.closed == 1);
    CHECK(second.closed == 0);
    CHECK(source.native_handle() == nullptr);
    SelfMove(destination);
    CHECK(destination.native_handle() == &second);
  }
  CHECK(first.closed == 1);
  CHECK(second.closed == 1);
}

TEST("pe.core_observed_handle_owns_and_releases_on_assignment") {
  HandleState first, second;
  {
    auto destination = wfg::VerifiedIdentityTestFactory::Observed(&first, CloseHandle);
    auto original = wfg::VerifiedIdentityTestFactory::Observed(&second, CloseHandle);
    auto source = std::move(original);
    CHECK(original.native_handle() == nullptr);
    destination = std::move(source);
    CHECK(first.closed == 1);
    CHECK(source.native_handle() == nullptr);
    SelfMove(destination);
    CHECK(destination.native_handle() == &second);
  }
  CHECK(first.closed == 1);
  CHECK(second.closed == 1);
}

TEST("pe.core_handle_closer_is_mandatory") {
  HandleState handle;
  bool rejected{};
  try { auto process = wfg::VerifiedIdentityTestFactory::Owned(&handle, nullptr); }
  catch (const std::invalid_argument&) { rejected = true; }
  CHECK(rejected);
  rejected = false;
  try { auto process = wfg::VerifiedIdentityTestFactory::Observed(&handle, nullptr); }
  catch (const std::invalid_argument&) { rejected = true; }
  CHECK(rejected);
  rejected = false;
  try { auto process = wfg::VerifiedIdentityTestFactory::Owned(nullptr, CloseHandle); }
  catch (const std::invalid_argument&) { rejected = true; }
  CHECK(rejected);
  rejected = false;
  try { auto process = wfg::VerifiedIdentityTestFactory::Observed(nullptr, CloseHandle); }
  catch (const std::invalid_argument&) { rejected = true; }
  CHECK(rejected);
}

TEST("pe.core_error_operation_rejects_paths_and_raw_text") {
  for (const auto unsafe : {"C:\\Users\\player\\private", "/home/player/private", "raw exception text", "bad\noperation", "../secret", ""}) {
    const wfg::Error error{wfg::ErrorCode::MalformedImage, wfg::Stage::Inspect, unsafe};
    CHECK(error.operation.view() == "invalid_operation");
  }
}

TEST("pe.core_error_operation_rejects_overlong_identifiers") {
  const wfg::Error error{wfg::ErrorCode::MalformedImage, wfg::Stage::Inspect, std::string(65, 'a')};
  CHECK(error.operation.view() == "invalid_operation");
}

TEST("pe.core_error_operation_validates_public_reassignment") {
  wfg::Error error{wfg::ErrorCode::MalformedImage, wfg::Stage::Inspect, "CheckFactory"};
  CHECK(error.operation.view() == "CheckFactory");
  error.operation = "/home/private";
  CHECK(error.operation.view() == "invalid_operation");
  error.operation = std::string(64, 'a');
  CHECK(error.operation.view().size() == 64);
}

TEST("pe.core_verified_identity_construction_stays_private") {
  CHECK((!std::is_constructible_v<wfg::OwnedProcess, void*, std::uint32_t, std::uint64_t, std::string, std::string, std::string, wfg::HandleCloser>));
  CHECK((!std::is_constructible_v<wfg::ObservedProcess, void*, std::uint32_t, std::uint64_t, wfg::HandleCloser>));
  CHECK(!std::is_default_constructible_v<wfg::OwnedProcess>);
  CHECK(!std::is_default_constructible_v<wfg::ObservedProcess>);
  CHECK((!std::is_convertible_v<wfg::ObservedProcess, wfg::OwnedProcess>));
}

TEST("pe.core_stages_use_canonical_order") {
  constexpr std::array stages{
    wfg::Stage::configuration, wfg::Stage::package_discovery, wfg::Stage::activation,
    wfg::Stage::process_identity, wfg::Stage::debug_attach, wfg::Stage::dependencies,
    wfg::Stage::build_validation, wfg::Stage::pre_factory_guard, wfg::Stage::import_transaction,
    wfg::Stage::detach, wfg::Stage::runtime_observation};
  for (std::size_t index = 0; index < stages.size(); ++index) CHECK(static_cast<std::size_t>(stages[index]) == index);
}

TEST("pe.core_read_into_returns_exact_bytes_without_writes") {
  FakeMemory fake(0x1000, {std::byte{0x11}, std::byte{0x22}, std::byte{0x33}, std::byte{0x44}});
  wfg::IWritableMemory& memory = fake;
  std::array<std::byte, 2> buffer{};
  REQUIRE_OK(memory.ReadInto(0x1001, buffer));
  CHECK(buffer[0] == std::byte{0x22});
  CHECK(buffer[1] == std::byte{0x33});
  CHECK(fake.write_count() == 0);
  CHECK(fake.protect_count() == 0);
}

TEST("pe.core_read_into_refuses_short_reads_without_writes") {
  FakeMemory fake(0x1000, {std::byte{0x11}, std::byte{0x22}});
  wfg::IWritableMemory& memory = fake;
  std::array<std::byte, 2> buffer{std::byte{0x77}, std::byte{0x88}};
  fake.SetShortReads();
  REQUIRE_ERROR(memory.ReadInto(0x1000, buffer), wfg::ErrorCode::UnreadableMemory);
  CHECK(buffer[0] == std::byte{0x77});
  CHECK(buffer[1] == std::byte{0x88});
  fake.SetShortReads(false);
  REQUIRE_ERROR(memory.ReadInto(0x1001, buffer), wfg::ErrorCode::UnreadableMemory);
  CHECK(fake.write_count() == 0);
  CHECK(fake.protect_count() == 0);
}
