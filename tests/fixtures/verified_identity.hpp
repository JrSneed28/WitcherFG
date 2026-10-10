#pragma once

#include "wfg/core.hpp"

namespace wfg {
class VerifiedIdentityTestFactory {
public:
  using Closer = void (*)(void*) noexcept;
  static OwnedProcess Owned(void* handle, Closer closer) {
    return OwnedProcess(handle, 42U, 100ULL, "test_user", "test_session", "test_package", closer);
  }
  static ObservedProcess Observed(void* handle, Closer closer, std::uint32_t pid = 42U,
      std::uint64_t creation_time = 100ULL) {
    return ObservedProcess(handle, pid, creation_time, closer);
  }
};
}
