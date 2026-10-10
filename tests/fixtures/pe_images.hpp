#pragma once

#include "support.hpp"

#include <cstring>

namespace FixturePe {
struct Image { wfg::Address base; FakeMemory memory; };
inline void Put(std::vector<std::byte>& bytes, std::size_t at, auto value) { std::memcpy(bytes.data() + at, &value, sizeof(value)); }
inline void Text(std::vector<std::byte>& bytes, std::size_t at, const char* value) { std::memcpy(bytes.data() + at, value, std::strlen(value) + 1); }
inline Image SupportedHeaders(wfg::Address base) {
  constexpr auto kSlotRva = 0x2B524D8U;
  std::vector<std::byte> bytes(kSlotRva + 0x100); Put<std::uint16_t>(bytes, 0, 0x5A4D); Put<std::uint32_t>(bytes, 0x3c, 0x80);
  Put<std::uint32_t>(bytes, 0x80, 0x00004550); Put<std::uint16_t>(bytes, 0x84, 0x8664); Put<std::uint16_t>(bytes, 0x86, 1); Put<std::uint32_t>(bytes, 0x88, 0x6AC55CE6); Put<std::uint16_t>(bytes, 0x94, 0xF0);
  Put<std::uint16_t>(bytes, 0x98, 0x20B); Put<std::uint32_t>(bytes, 0x98 + 0x38, 0x3000000); Put<std::uint32_t>(bytes, 0x98 + 0x6c, 16);
  Put<std::uint32_t>(bytes, 0x98 + 0x70 + 8, 0x200); Put<std::uint32_t>(bytes, 0x98 + 0x70 + 12, 0x40);
  Put<std::uint32_t>(bytes, 0x188, 0x1000); Put<std::uint32_t>(bytes, 0x18c, 0x2000); Put<std::uint32_t>(bytes, 0x190, 0x2000);
  Put<std::uint32_t>(bytes, 0x200, 0x320); Put<std::uint32_t>(bytes, 0x20c, 0x300); Put<std::uint32_t>(bytes, 0x210, kSlotRva);
  Text(bytes, 0x300, "dxgi.dll"); Put<std::uint64_t>(bytes, 0x320, 0x340); Put<std::uint64_t>(bytes, 0x328, 0); Put<std::uint64_t>(bytes, kSlotRva, 0x1234567812345678ULL); Put<std::uint64_t>(bytes, kSlotRva + 8, 0);
  Text(bytes, 0x342, "CreateDXGIFactory2");
  return {base, FakeMemory(base, std::move(bytes))};
}
inline Image DirectoryBeyondImage() {
  auto image = SupportedHeaders(0x00007FF600000000ULL);
  // The import directory begins outside SizeOfImage, so parsing must fail before reading it.
  auto bytes = image.memory.Read(image.base, 0x4000);
  auto copy = wfg::Value(bytes);
  Put<std::uint32_t>(copy, 0x98 + 0x70 + 8, 0x3000000);
  return {image.base, FakeMemory(image.base, std::move(copy))};
}
inline Image TruncatedHeaders() {
  return {0x00007FF600000000ULL, FakeMemory(0x00007FF600000000ULL, std::vector<std::byte>(0x20))};
}
inline Image InvalidMachine() {
  auto image = SupportedHeaders(0x00007FF600000000ULL);
  auto bytes = wfg::Value(image.memory.Read(image.base, 0x2B525D8));
  Put<std::uint16_t>(bytes, 0x84, 0x014c);
  return {image.base, FakeMemory(image.base, std::move(bytes))};
}
inline Image DuplicateNamedImports() {
  auto image = SupportedHeaders(0x00007FF600000000ULL);
  auto bytes = wfg::Value(image.memory.Read(image.base, 0x2B525D8));
  Put<std::uint32_t>(bytes, 0x214, 0x360);
  Put<std::uint32_t>(bytes, 0x220, 0x300);
  Put<std::uint32_t>(bytes, 0x224, 0x2B524E8);
  Put<std::uint64_t>(bytes, 0x360, 0x380);
  Put<std::uint64_t>(bytes, 0x368, 0);
  Text(bytes, 0x382, "CreateDXGIFactory2");
  return {image.base, FakeMemory(image.base, std::move(bytes))};
}
inline Image BoundImportWithoutOriginalNames() {
  auto image = SupportedHeaders(0x00007FF600000000ULL);
  auto bytes = wfg::Value(image.memory.Read(image.base, 0x2B525D8));
  Put<std::uint32_t>(bytes, 0x200, 0);
  Put<std::uint64_t>(bytes, 0x2B524D8, 0x00007FF612345678ULL);
  return {image.base, FakeMemory(image.base, std::move(bytes))};
}
inline Image UnterminatedImports() {
  auto image = DuplicateNamedImports();
  auto bytes = wfg::Value(image.memory.Read(image.base, 0x2B525D8));
  Put<std::uint32_t>(bytes, 0x228, 0x390);
  Put<std::uint32_t>(bytes, 0x234, 0x300);
  Put<std::uint32_t>(bytes, 0x238, 0x2B524F8);
  return {image.base, FakeMemory(image.base, std::move(bytes))};
}
inline Image ImportThunkOutsideImage() {
  auto image = SupportedHeaders(0x00007FF600000000ULL);
  auto bytes = wfg::Value(image.memory.Read(image.base, 0x2B525D8));
  Put<std::uint32_t>(bytes, 0x200, 0x2FFFFFF8);
  return {image.base, FakeMemory(image.base, std::move(bytes))};
}
inline Image ExportImage(const char* name, std::uint32_t function_rva, std::string forwarder = {}, std::uint32_t ordinal_base = 1, wfg::Address base = 0x00007FF700000000ULL) {
  std::vector<std::byte> bytes(0x1000);
  Put<std::uint16_t>(bytes, 0, 0x5A4D);
  Put<std::uint32_t>(bytes, 0x3c, 0x80);
  Put<std::uint32_t>(bytes, 0x80, 0x4550);
  Put<std::uint16_t>(bytes, 0x84, 0x8664);
  Put<std::uint16_t>(bytes, 0x86, 1);
  Put<std::uint16_t>(bytes, 0x94, 0xF0);
  Put<std::uint16_t>(bytes, 0x98, 0x20B);
  Put<std::uint32_t>(bytes, 0x98 + 0x38, 0x1000);
  Put<std::uint32_t>(bytes, 0x98 + 0x6c, 16);
  Put<std::uint32_t>(bytes, 0x98 + 0x70, 0x200);
  Put<std::uint32_t>(bytes, 0x98 + 0x74, 0x300);
  Put<std::uint32_t>(bytes, 0x188, 0x800);
  Put<std::uint32_t>(bytes, 0x18c, 0x200);
  Put<std::uint32_t>(bytes, 0x190, 0x800);
  Put<std::uint32_t>(bytes, 0x210, ordinal_base);
  Put<std::uint32_t>(bytes, 0x214, 1);
  Put<std::uint32_t>(bytes, 0x218, 1);
  Put<std::uint32_t>(bytes, 0x21c, 0x280);
  Put<std::uint32_t>(bytes, 0x220, 0x290);
  Put<std::uint32_t>(bytes, 0x224, 0x2A0);
  Put<std::uint32_t>(bytes, 0x280, forwarder.empty() ? function_rva : 0x350);
  Put<std::uint32_t>(bytes, 0x290, 0x2B0);
  Put<std::uint16_t>(bytes, 0x2A0, 0);
  Text(bytes, 0x2B0, name);
  if (!forwarder.empty()) Text(bytes, 0x350, forwarder.c_str());
  return {base, FakeMemory(base, std::move(bytes))};
}
inline Image WithDirectory(std::uint32_t index, std::uint32_t address, std::uint32_t size) {
  auto image = ExportImage("Direct", 0x500);
  auto bytes = wfg::Value(image.memory.Read(image.base, 0x1000));
  Put<std::uint32_t>(bytes, 0x98 + 0x70 + index * 8, address);
  Put<std::uint32_t>(bytes, 0x98 + 0x74 + index * 8, size);
  return {image.base, FakeMemory(image.base, std::move(bytes))};
}
}  // namespace FixturePe
