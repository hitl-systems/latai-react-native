// SPDX-License-Identifier: Apache-2.0 OR MIT
// TEST AID ONLY: the part of fbjni that android/src/main/cpp/cpp-adapter.cpp uses. See stubs/jsi/jsi.h.
#pragma once
#include <jni.h>

namespace facebook::jni {
template <typename T>
class alias_ref {
 public:
  alias_ref(T ref) : ref_(ref) {}
  T operator->() const { return ref_; }

 private:
  T ref_;
};
}  // namespace facebook::jni
