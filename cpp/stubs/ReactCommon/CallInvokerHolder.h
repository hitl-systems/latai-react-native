// SPDX-License-Identifier: Apache-2.0 OR MIT
// TEST AID ONLY: the part of ReactCommon/CallInvokerHolder.h that cpp-adapter.cpp uses.
#pragma once
#include <ReactCommon/CallInvoker.h>
#include <jni.h>

#include <memory>

namespace facebook::react {
class CallInvokerHolder {
 public:
  struct JavaPart {
    CallInvokerHolder *cthis();
  };
  using javaobject = JavaPart *;
  std::shared_ptr<CallInvoker> getCallInvoker();
};
}  // namespace facebook::react
