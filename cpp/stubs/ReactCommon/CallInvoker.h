// SPDX-License-Identifier: Apache-2.0 OR MIT
// TEST AID ONLY: the part of ReactCommon/CallInvoker.h that LataiJsi.cpp uses. See stubs/jsi/jsi.h.
#pragma once
#include <functional>

namespace facebook::react {
class CallInvoker {
 public:
  virtual ~CallInvoker() = default;
  virtual void invokeAsync(std::function<void()> &&func) = 0;
};
}  // namespace facebook::react
