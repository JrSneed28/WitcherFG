#pragma once

#include "wfg/compatibility.hpp"

#include <algorithm>
#include <cstring>
#include <map>

class RuntimeMemory final : public wfg::IWritableMemory {
public:
  wfg::Result<wfg::Bytes> Read(wfg::Address address, std::size_t size) const override;
  wfg::Result<wfg::MemoryRegion> Query(wfg::Address address) const override;
  wfg::Result<wfg::Unit> ReadInto(wfg::Address address, std::span<std::byte> bytes) const override;
  wfg::Result<wfg::Unit> Write(wfg::Address, std::span<const std::byte>) override;
  wfg::Result<std::uint32_t> Protect(wfg::Address, std::size_t, std::uint32_t) override;
  void Put(wfg::Address address, std::span<const std::byte> bytes);
  template <class T> void Put(wfg::Address address, T value) { Put(address, std::as_bytes(std::span(&value, 1))); }
  void AddRegion(wfg::Address base, std::size_t size, bool readable = true);
  void SetReadable(wfg::Address address, bool readable);
  std::size_t write_count() const { return writes_; }
  std::size_t protect_count() const { return protects_; }
private:
  struct Region { wfg::Address base; std::size_t size; bool readable; };
  std::map<wfg::Address, std::byte> bytes_;
  std::vector<Region> regions_;
  std::size_t writes_{};
  std::size_t protects_{};
};

enum class Mutation { Package, Version, Machine, Timestamp, ImageSize, InterposerVersion, InterposerLength, InterposerHash, Signer, Correspondence, FirstHash, SecondHash };

class SupportedRuntimeFixture {
public:
  explicit SupportedRuntimeFixture(wfg::Address executable_base = 0x00007FF600000000ULL);
  wfg::CompatibilityRecord SyntheticBuild() const;
  void SetImportPointer(std::string_view name, wfg::Address value);
  void SetAllImportsToInterposer();
  void SetManager(wfg::Address value);
  void SetFactory(wfg::Address value);
  void MakeManagerUnreadable();
  void MakeFactoryUnreadable();
  void SetNativeTrusted(bool trusted);
  void EnableNativeForwarder();
  void EnableInterposerForwarder();
  void AddApplicationLocalLoader(std::string basename, std::string version, bool recognized_official);
  void Mutate(Mutation mutation);
  wfg::Address InterposerExport(std::string_view name) const;

  RuntimeMemory memory;
  wfg::RuntimeInventory inventory;
  wfg::Address foreign_hook{0x00007FFA00001234ULL};
  wfg::Address native_factory{0x00007FFB00005678ULL};
private:
  wfg::Address executable_base_{};
  wfg::Address manager_{0x00007FF900000000ULL};
  std::map<std::string, wfg::Address, std::less<>> slots_;
  std::map<std::string, wfg::Address, std::less<>> interposer_exports_;
  std::map<std::string, wfg::Address, std::less<>> native_exports_;
};

inline wfg::Error RuntimeReadError() { return {wfg::ErrorCode::UnreadableMemory, wfg::Stage::Compatibility, "fixture_read"}; }

inline void RuntimeMemory::AddRegion(wfg::Address base, std::size_t size, bool readable) { regions_.push_back({base, size, readable}); }
inline void RuntimeMemory::Put(wfg::Address address, std::span<const std::byte> bytes) {
  for (std::size_t index = 0; index < bytes.size(); ++index) bytes_[address + index] = bytes[index];
}
inline wfg::Result<wfg::Bytes> RuntimeMemory::Read(wfg::Address address, std::size_t size) const {
  const auto region = std::find_if(regions_.begin(), regions_.end(), [=](const Region& candidate) { return candidate.readable && address >= candidate.base && size <= candidate.size && address - candidate.base <= candidate.size - size; });
  if (region == regions_.end()) return RuntimeReadError();
  wfg::Bytes result(size);
  for (std::size_t index = 0; index < size; ++index) if (const auto found = bytes_.find(address + index); found != bytes_.end()) result[index] = found->second;
  return result;
}
inline wfg::Result<wfg::MemoryRegion> RuntimeMemory::Query(wfg::Address address) const {
  const auto region = std::find_if(regions_.begin(), regions_.end(), [=](const Region& candidate) { return address >= candidate.base && address - candidate.base < candidate.size; });
  if (region == regions_.end()) return RuntimeReadError();
  return wfg::MemoryRegion{region->base, region->size, true, region->readable, false, false, 0};
}
inline wfg::Result<wfg::Unit> RuntimeMemory::ReadInto(wfg::Address address, std::span<std::byte> bytes) const {
  const auto result = Read(address, bytes.size());
  if (!wfg::IsOk(result)) return wfg::Failure(result);
  std::memcpy(bytes.data(), wfg::Value(result).data(), bytes.size());
  return wfg::Unit{};
}
inline wfg::Result<wfg::Unit> RuntimeMemory::Write(wfg::Address, std::span<const std::byte>) { ++writes_; return wfg::Unit{}; }
inline wfg::Result<std::uint32_t> RuntimeMemory::Protect(wfg::Address, std::size_t, std::uint32_t) { ++protects_; return 0U; }
inline void RuntimeMemory::SetReadable(wfg::Address address, bool readable) {
  for (auto& region : regions_) if (address >= region.base && address - region.base < region.size) region.readable = readable;
}

namespace RuntimeFixtureDetail {
inline wfg::Hash256 Hash(std::string_view hex) {
  wfg::Hash256 result{};
  const auto value = [](char ch) { return static_cast<unsigned char>(ch <= '9' ? ch - '0' : ch - 'a' + 10); };
  for (std::size_t index = 0; index < result.size(); ++index) result[index] = static_cast<std::byte>((value(hex[index * 2]) << 4) | value(hex[index * 2 + 1]));
  return result;
}
inline void Text(RuntimeMemory& memory, wfg::Address address, std::string_view text) {
  memory.Put(address, std::as_bytes(std::span(text.data(), text.size())));
  memory.Put<std::byte>(address + text.size(), std::byte{});
}
inline void Headers(RuntimeMemory& memory, wfg::Address base, std::uint32_t size, std::uint32_t timestamp, std::uint32_t import_rva = 0, std::uint32_t import_size = 0, std::uint32_t export_rva = 0, std::uint32_t export_size = 0) {
  memory.Put<std::uint16_t>(base, 0x5A4D); memory.Put<std::uint32_t>(base + 0x3c, 0x80); memory.Put<std::uint32_t>(base + 0x80, 0x4550);
  memory.Put<std::uint16_t>(base + 0x84, 0x8664); memory.Put<std::uint16_t>(base + 0x86, 1); memory.Put<std::uint32_t>(base + 0x88, timestamp); memory.Put<std::uint16_t>(base + 0x94, 0xF0);
  memory.Put<std::uint16_t>(base + 0x98, 0x20B); memory.Put<std::uint32_t>(base + 0x98 + 0x38, size); memory.Put<std::uint32_t>(base + 0x98 + 0x6c, 16);
  memory.Put<std::uint32_t>(base + 0x98 + 0x70, export_rva); memory.Put<std::uint32_t>(base + 0x98 + 0x74, export_size);
  memory.Put<std::uint32_t>(base + 0x98 + 0x78, import_rva); memory.Put<std::uint32_t>(base + 0x98 + 0x7c, import_size);
}
inline void Exports(RuntimeMemory& memory, wfg::Address base, const std::vector<std::string>& names) {
  Headers(memory, base, 0x1000, 1, 0, 0, 0x200, 0x400);
  memory.Put<std::uint32_t>(base + 0x210, 1U); memory.Put<std::uint32_t>(base + 0x214, static_cast<std::uint32_t>(names.size())); memory.Put<std::uint32_t>(base + 0x218, static_cast<std::uint32_t>(names.size()));
  memory.Put<std::uint32_t>(base + 0x21c, 0x280); memory.Put<std::uint32_t>(base + 0x220, 0x2a0); memory.Put<std::uint32_t>(base + 0x224, 0x2c0);
  for (std::size_t index = 0; index < names.size(); ++index) {
    memory.Put<std::uint32_t>(base + 0x280 + index * 4, static_cast<std::uint32_t>(0x600 + index * 0x10));
    memory.Put<std::uint32_t>(base + 0x2a0 + index * 4, static_cast<std::uint32_t>(0x300 + index * 0x30));
    memory.Put<std::uint16_t>(base + 0x2c0 + index * 2, static_cast<std::uint16_t>(index));
    Text(memory, base + 0x300 + index * 0x30, names[index]);
  }
}
}  // namespace RuntimeFixtureDetail

inline SupportedRuntimeFixture::SupportedRuntimeFixture(wfg::Address executable_base) : executable_base_(executable_base) {
  constexpr auto executable_size = 0x658F000U;
  constexpr wfg::Address d3d12_base = 0x00007FF700000000ULL;
  constexpr wfg::Address dxgi_base = 0x00007FF710000000ULL;
  constexpr wfg::Address dxcore_base = 0x00007FF720000000ULL;
  constexpr wfg::Address interposer_base = 0x00007FF730000000ULL;
  memory.AddRegion(executable_base_, executable_size); memory.AddRegion(d3d12_base, 0x1000); memory.AddRegion(dxgi_base, 0x1000); memory.AddRegion(dxcore_base, 0x1000); memory.AddRegion(interposer_base, 0x1000);
  memory.AddRegion(manager_, 0x302F600);
  RuntimeFixtureDetail::Headers(memory, executable_base_, executable_size, 0x6AC55CE6U, 0x200, 0x60);
  memory.Put<std::uint32_t>(executable_base_ + 0x200, 0x1000U); memory.Put<std::uint32_t>(executable_base_ + 0x20c, 0x500U); memory.Put<std::uint32_t>(executable_base_ + 0x210, 0x2B52450U);
  memory.Put<std::uint32_t>(executable_base_ + 0x214, 0x1200U); memory.Put<std::uint32_t>(executable_base_ + 0x220, 0x510U); memory.Put<std::uint32_t>(executable_base_ + 0x224, 0x2B524D0U);
  RuntimeFixtureDetail::Text(memory, executable_base_ + 0x500, "d3d12.dll"); RuntimeFixtureDetail::Text(memory, executable_base_ + 0x510, "dxgi.dll");
  memory.Put<std::uint64_t>(executable_base_ + 0x1000, 0x600); memory.Put<std::uint64_t>(executable_base_ + 0x1200, 0x620); memory.Put<std::uint64_t>(executable_base_ + 0x1208, 0x640); memory.Put<std::uint64_t>(executable_base_ + 0x1210, 0x660);
  RuntimeFixtureDetail::Text(memory, executable_base_ + 0x602, "D3D12CreateDevice"); RuntimeFixtureDetail::Text(memory, executable_base_ + 0x622, "CreateDXGIFactory1"); RuntimeFixtureDetail::Text(memory, executable_base_ + 0x642, "CreateDXGIFactory2"); RuntimeFixtureDetail::Text(memory, executable_base_ + 0x662, "CreateDXGIFactory");
  RuntimeFixtureDetail::Exports(memory, d3d12_base, {"D3D12CreateDevice"}); RuntimeFixtureDetail::Exports(memory, dxgi_base, {"CreateDXGIFactory1", "CreateDXGIFactory2", "CreateDXGIFactory"}); RuntimeFixtureDetail::Exports(memory, dxcore_base, {"CreateDXGIFactory1"}); RuntimeFixtureDetail::Exports(memory, interposer_base, {"D3D12CreateDevice", "CreateDXGIFactory1", "CreateDXGIFactory2", "CreateDXGIFactory"});
  native_exports_ = {{"D3D12CreateDevice", d3d12_base + 0x600}, {"CreateDXGIFactory1", dxgi_base + 0x600}, {"CreateDXGIFactory2", dxgi_base + 0x610}, {"CreateDXGIFactory", dxgi_base + 0x620}};
  interposer_exports_ = {{"D3D12CreateDevice", interposer_base + 0x600}, {"CreateDXGIFactory1", interposer_base + 0x610}, {"CreateDXGIFactory2", interposer_base + 0x620}, {"CreateDXGIFactory", interposer_base + 0x630}};
  slots_ = {{"D3D12CreateDevice", executable_base_ + 0x2B52450}, {"CreateDXGIFactory1", executable_base_ + 0x2B524D0}, {"CreateDXGIFactory2", executable_base_ + 0x2B524D8}, {"CreateDXGIFactory", executable_base_ + 0x2B524E0}};
  for (const auto& [name, slot] : slots_) memory.Put(slot, native_exports_.at(name));
  constexpr std::string_view first = "01234567890123456789012345678901"; constexpr std::string_view second = "abcdefghijklmnopqrstuvwxyzABCDEF";
  memory.Put(executable_base_ + 0x1CB0384, std::as_bytes(std::span(first.data(), first.size()))); memory.Put(executable_base_ + 0x1CB061D, std::as_bytes(std::span(second.data(), second.size())));
  memory.Put(executable_base_ + 0x5A75F30, manager_); memory.Put(manager_ + 0x302F568, static_cast<wfg::Address>(0));
  inventory.package_name = "CDPROJEKTSA.Witcher3GOTY"; inventory.package_version = "5.0.4.0"; inventory.executable = {"witcher3.exe", {executable_base_, 0x8664, 0x6AC55CE6U, executable_size}};
  inventory.trusted_system_modules = {{{"d3d12.dll", {d3d12_base, 0x8664, 1, 0x1000}}, true}, {{"dxgi.dll", {dxgi_base, 0x8664, 1, 0x1000}}, true}, {{"dxcore.dll", {dxcore_base, 0x8664, 1, 0x1000}}, true}};
  inventory.interposer = {{"sl.interposer.dll", {interposer_base, 0x8664, 1, 0x1000}}, "2.14.1", 652928, RuntimeFixtureDetail::Hash("8c87c9499461da561edd529aa9bf7831d67d7b94ebb1c1a5ed54ef4934e1ea4c"), true, true};
}

inline wfg::CompatibilityRecord SupportedRuntimeFixture::SyntheticBuild() const { auto build = wfg::SupportedBuild(); build.fingerprints = {{{0x1CB0384U, 32U, RuntimeFixtureDetail::Hash("861009ec4d599fab1f40abc76e6f89880cff5833c79c548c99f9045f191cd90b")}, {0x1CB061DU, 32U, RuntimeFixtureDetail::Hash("cfd2f1fad75a1978da0a444883db7251414b139f31f5a04704c291fdb0e175e6")}}}; return build; }
inline void SupportedRuntimeFixture::SetImportPointer(std::string_view name, wfg::Address value) { memory.Put(slots_.at(std::string(name)), value); }
inline void SupportedRuntimeFixture::SetAllImportsToInterposer() { for (const auto& [name, slot] : slots_) memory.Put(slot, interposer_exports_.at(name)); }
inline void SupportedRuntimeFixture::SetManager(wfg::Address value) { memory.Put(executable_base_ + 0x5A75F30, value); }
inline void SupportedRuntimeFixture::SetFactory(wfg::Address value) { memory.Put(manager_ + 0x302F568, value); }
inline void SupportedRuntimeFixture::MakeManagerUnreadable() { memory.SetReadable(executable_base_ + 0x5A75F30, false); }
inline void SupportedRuntimeFixture::MakeFactoryUnreadable() { memory.SetReadable(manager_, false); }
inline void SupportedRuntimeFixture::SetNativeTrusted(bool trusted) { inventory.trusted_system_modules.front().trusted_system = trusted; }
inline void SupportedRuntimeFixture::EnableNativeForwarder() { RuntimeFixtureDetail::Text(memory, 0x00007FF710000000ULL + 0x400, "dxcore.CreateDXGIFactory1"); memory.Put<std::uint32_t>(0x00007FF710000000ULL + 0x280, 0x400U); native_exports_["CreateDXGIFactory1"] = 0x00007FF720000000ULL + 0x600; SetImportPointer("CreateDXGIFactory1", native_exports_.at("CreateDXGIFactory1")); }
inline void SupportedRuntimeFixture::EnableInterposerForwarder() { RuntimeFixtureDetail::Text(memory, 0x00007FF730000000ULL + 0x400, "dxgi.CreateDXGIFactory"); memory.Put<std::uint32_t>(0x00007FF730000000ULL + 0x280, 0x400U); }
inline void SupportedRuntimeFixture::AddApplicationLocalLoader(std::string basename, std::string version, bool recognized_official) { inventory.application_local_loaders.push_back({std::move(basename), std::move(version), true, recognized_official}); }
inline void SupportedRuntimeFixture::Mutate(Mutation mutation) { switch (mutation) { case Mutation::Package: inventory.package_name = "other"; break; case Mutation::Version: inventory.package_version = "1"; break; case Mutation::Machine: memory.Put<std::uint16_t>(executable_base_ + 0x84, 0x14c); break; case Mutation::Timestamp: memory.Put<std::uint32_t>(executable_base_ + 0x88, 1U); break; case Mutation::ImageSize: memory.Put<std::uint32_t>(executable_base_ + 0xd0, 0x1234U); break; case Mutation::InterposerVersion: inventory.interposer.version = "0"; break; case Mutation::InterposerLength: ++inventory.interposer.length; break; case Mutation::InterposerHash: inventory.interposer.sha256[0] ^= std::byte{1}; break; case Mutation::Signer: inventory.interposer.trusted_nvidia_signer = false; break; case Mutation::Correspondence: inventory.interposer.backing_image_matches_loaded_module = false; break; case Mutation::FirstHash: memory.Put<std::byte>(executable_base_ + 0x1CB0384, std::byte{1}); break; case Mutation::SecondHash: memory.Put<std::byte>(executable_base_ + 0x1CB061D, std::byte{1}); break; } }
inline wfg::Address SupportedRuntimeFixture::InterposerExport(std::string_view name) const { return interposer_exports_.at(std::string(name)); }
