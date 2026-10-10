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
}  // namespace FixturePe
