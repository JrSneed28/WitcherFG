#pragma once

#include "wfg/core.hpp"

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
