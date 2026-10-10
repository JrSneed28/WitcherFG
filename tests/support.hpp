#pragma once

#include "wfg/core.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

class FakeMemory final : public wfg::IWritableMemory {
public:
  FakeMemory(wfg::Address base, wfg::Bytes bytes) : base_(base), bytes_(std::move(bytes)) {}
  wfg::Result<wfg::Bytes> Read(wfg::Address address, std::size_t size) const override {
    if (short_reads_ || address < base_ || size > bytes_.size() || address - base_ > bytes_.size() - size) return Error();
    return wfg::Bytes(bytes_.begin() + static_cast<std::ptrdiff_t>(address - base_), bytes_.begin() + static_cast<std::ptrdiff_t>(address - base_ + size));
  }
  wfg::Result<wfg::MemoryRegion> Query(wfg::Address) const override { return wfg::MemoryRegion{base_, bytes_.size(), true, true, true, false, 0}; }
  wfg::Result<wfg::Unit> Write(wfg::Address, std::span<const std::byte>) override { ++writes_; return wfg::Unit{}; }
  wfg::Result<std::uint32_t> Protect(wfg::Address, std::size_t, std::uint32_t) override { ++protects_; return 0U; }
  void SetShortReads(bool value = true) { short_reads_ = value; }
  std::size_t write_count() const { return writes_; }
  std::size_t protect_count() const { return protects_; }
private:
  static wfg::Error Error() { return {wfg::ErrorCode::UnreadableMemory, wfg::Stage::Inspect, "read"}; }
  wfg::Address base_; wfg::Bytes bytes_; bool short_reads_{}; std::size_t writes_{}; std::size_t protects_{};
};

class FakeAddressSpace final : public wfg::IMemory {
public:
  void Add(const wfg::IMemory& memory) { memories_.push_back(&memory); }
  wfg::Result<wfg::Bytes> Read(wfg::Address address, std::size_t size) const override {
    for (const auto* memory : memories_) {
      auto result = memory->Read(address, size);
      if (wfg::IsOk(result)) return result;
    }
    return wfg::Error{wfg::ErrorCode::UnreadableMemory, wfg::Stage::Inspect, "read"};
  }
  wfg::Result<wfg::MemoryRegion> Query(wfg::Address address) const override {
    for (const auto* memory : memories_) {
      auto result = memory->Query(address);
      if (wfg::IsOk(result) && address >= wfg::Value(result).base && address - wfg::Value(result).base < wfg::Value(result).size) return result;
    }
    return wfg::Error{wfg::ErrorCode::UnreadableMemory, wfg::Stage::Inspect, "query"};
  }
private:
  std::vector<const wfg::IMemory*> memories_;
};

namespace test {
struct Case { const char* name; void (*run)(); };
inline std::vector<Case>& Cases() { static std::vector<Case> cases; return cases; }
struct Register { Register(const char* name, void (*run)()) { Cases().push_back({name, run}); } };
inline void Check(bool value, const char* expression, const char* file, int line) { if (!value) throw std::runtime_error(std::string(file) + ":" + std::to_string(line) + ": " + expression); }
}
#define WFG_TEST_JOIN_INNER(a, b) a##b
#define WFG_TEST_JOIN(a, b) WFG_TEST_JOIN_INNER(a, b)
#define WFG_TEST_IMPL(name, line) static void WFG_TEST_JOIN(test_, line)(); static test::Register WFG_TEST_JOIN(register_, line){name, WFG_TEST_JOIN(test_, line)}; static void WFG_TEST_JOIN(test_, line)()
#define TEST(name) WFG_TEST_IMPL(name, __LINE__)
#define CHECK(expr) test::Check(static_cast<bool>(expr), #expr, __FILE__, __LINE__)
#define REQUIRE_OK(expr) ([&]() { auto result = (expr); if (!wfg::IsOk(result)) throw std::runtime_error("expected success: " + wfg::Failure(result).operation); return wfg::Value(result); }())
#define REQUIRE_ERROR(expr, expected) do { auto result = (expr); CHECK(!wfg::IsOk(result)); CHECK(wfg::Failure(result).code == (expected)); } while (false)
