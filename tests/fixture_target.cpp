#include "fixtures/lifecycle_protocol.hpp"
#include <cstdio>
namespace {
using namespace lifecycle;
template <class T> void Put(void *p, unsigned at, T value) {
  std::memcpy(static_cast<char *>(p) + at, &value, sizeof(value));
}
void Text(void *p, unsigned at, const char *value) {
  std::memcpy(static_cast<char *>(p) + at, value, std::strlen(value) + 1);
}
void Headers(void *p, unsigned size, bool exports) {
  Put<std::uint16_t>(p, 0, 0x5a4d);
  Put<std::uint32_t>(p, 0x3c, 0x80);
  Put<std::uint32_t>(p, 0x80, 0x4550);
  Put<std::uint16_t>(p, 0x84, 0x8664);
  Put<std::uint16_t>(p, 0x86, 1);
  Put<std::uint32_t>(p, 0x88, 42);
  Put<std::uint16_t>(p, 0x94, 0xf0);
  Put<std::uint16_t>(p, 0x98, 0x20b);
  Put<std::uint32_t>(p, 0xd0, size);
  Put<std::uint32_t>(p, 0x104, 16);
  Put<std::uint32_t>(p, exports ? 0x108 : 0x110, 0x200);
  Put<std::uint32_t>(p, exports ? 0x10c : 0x114, exports ? 0x400 : 100);
}
void *Exports(unsigned first) {
  auto *p = VirtualAlloc(nullptr, 8192, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  Must(p != nullptr, "export_alloc");
  Headers(p, 8192, true);
  Put<unsigned>(p, 0x210, 1);
  Put<unsigned>(p, 0x214, 4);
  Put<unsigned>(p, 0x218, 4);
  Put<unsigned>(p, 0x21c, 0x280);
  Put<unsigned>(p, 0x220, 0x2a0);
  Put<unsigned>(p, 0x224, 0x2c0);
  for (unsigned i = 0; i < 4; ++i) {
    Put<unsigned>(p, 0x280 + i * 4, 0x1000 + i * 16);
    Put<unsigned>(p, 0x2a0 + i * 4, 0x300 + i * 40);
    Put<std::uint16_t>(p, 0x2c0 + i * 2, static_cast<std::uint16_t>(i));
    Text(p, 0x300 + i * 40, Names[i]);
    // Disposable x64 function: mov eax, immediate; ret. This belongs solely to
    // the child's synthetic export page, never the launcher or a game process.
    Put<unsigned char>(p, 0x1000 + i * 16, 0xb8);
    Put<unsigned>(p, 0x1001 + i * 16, first + i);
    Put<unsigned char>(p, 0x1005 + i * 16, 0xc3);
  }
  DWORD old{};
  Must(VirtualProtect(p, 4096, PAGE_READONLY, &old), "export_headers");
  Must(VirtualProtect(static_cast<char *>(p) + 4096, 4096, PAGE_EXECUTE_READ, &old), "export_code");
  Must(FlushInstructionCache(GetCurrentProcess(), p, 8192), "fixture_flush");
  return p;
}
Layout Create() {
  Layout l;
  l.pid = GetCurrentProcessId();
  l.creation = Creation(GetCurrentProcess());
  l.native = reinterpret_cast<std::uint64_t>(Exports(10));
  l.replacement = reinterpret_cast<std::uint64_t>(Exports(20));
  auto *p = VirtualAlloc(nullptr, ImageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  Must(p != nullptr, "image_alloc");
  l.image = reinterpret_cast<std::uint64_t>(p);
  Headers(p, ImageSize, false);
  Text(p, 0x500, "fixture-native.dll");
  Text(p, 0x800, "01234567890123456789012345678901");
  Text(p, 0x840, "abcdefghijklmnopqrstuvwxyzABCDEF");
  for (unsigned i = 0; i < 4; ++i) {
    const unsigned d = 0x200 + 20 * i;
    Put<unsigned>(p, d, 0x600 + i * 16);
    Put<unsigned>(p, d + 12, 0x500);
    Put<unsigned>(p, d + 16, 0x2000 + i * 0x1000);
    Put<std::uint64_t>(p, 0x600 + i * 16, 0xa00 + i * 40);
    Text(p, 0xa02 + i * 40, Names[i]);
    l.slots[i] = l.image + 0x2000 + i * 0x1000;
    l.originals[i] = l.native + 0x1000 + i * 16;
    l.replacements[i] = l.replacement + 0x1000 + i * 16;
    l.protections[i] = i % 2 ? PAGE_READWRITE : PAGE_READONLY;
    Put<std::uint64_t>(p, 0x2000 + i * 0x1000, l.originals[i]);
  }
  DWORD old{};
  Must(VirtualProtect(p, 8192, PAGE_READONLY, &old), "image_headers");
  for (unsigned i = 0; i < 4; ++i)
    Must(VirtualProtect(reinterpret_cast<void *>(l.slots[i]), 4096, l.protections[i], &old),
         "slot_protection");
  return l;
}
unsigned Exception() {
  __try {
    RaiseException(FixtureException, 0, 0, nullptr);
  } __except (GetExceptionCode() == FixtureException ? EXCEPTION_EXECUTE_HANDLER
                                                     : EXCEPTION_CONTINUE_SEARCH) {
    return 1;
  }
  return 0;
}
} // namespace
int wmain(int argc, wchar_t **argv) {
  if (argc != 5)
    return 2;
  try {
    auto pipe = Arg(argv[1]), go = Arg(argv[2]), quit = Arg(argv[3]);
    const bool exception = std::wcscmp(argv[4], L"exception") == 0;
    auto l = Create();
    Send(pipe, l);
    if (WaitForSingleObject(go, 9000) != WAIT_OBJECT_0)
      return 3;
    Report r;
    r.pid = GetCurrentProcessId();
    if (exception)
      r.exception_handled = Exception();
    r.debugged = IsDebuggerPresent();
    for (unsigned i = 0; i < 4; ++i) {
      r.pointers[i] = *reinterpret_cast<volatile std::uint64_t *>(l.slots[i]);
      MEMORY_BASIC_INFORMATION m{};
      Must(VirtualQuery(reinterpret_cast<void *>(l.slots[i]), &m, sizeof(m)) == sizeof(m),
           "slot_query");
      r.protections[i] = m.Protect;
      if (r.pointers[i] == l.originals[i] || r.pointers[i] == l.replacements[i])
        r.calls[i] = reinterpret_cast<unsigned (*)()>(r.pointers[i])();
    }
    Send(pipe, r);
    WaitForSingleObject(quit, 9000);
    return 0;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "fixture: %s\n", e.what());
    return 5;
  }
}
