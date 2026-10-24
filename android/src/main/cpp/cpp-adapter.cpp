// SPDX-License-Identifier: Apache-2.0 OR MIT
// The Android entry: LataiModule.install() passes the address of the Hermes/JSC jsi::Runtime and
// the JS CallInvoker holder; this installs `global.__latai` (cpp/LataiJsi.cpp) into that runtime.
#include <ReactCommon/CallInvokerHolder.h>
#include <fbjni/fbjni.h>
#include <jni.h>
#include <jsi/jsi.h>

#include "LataiJsi.h"

extern "C" JNIEXPORT void JNICALL Java_ai_latai_rn_LataiModule_nativeInstall(JNIEnv *, jobject, jlong runtimePtr,
                                                                              jobject callInvokerHolder) {
  auto *runtime = reinterpret_cast<facebook::jsi::Runtime *>(runtimePtr);
  auto holder = facebook::jni::alias_ref<facebook::react::CallInvokerHolder::javaobject>(
      reinterpret_cast<facebook::react::CallInvokerHolder::javaobject>(callInvokerHolder));
  latai_rn::install(*runtime, holder->cthis()->getCallInvoker());
}

extern "C" JNIEXPORT void JNICALL Java_ai_latai_rn_LataiModule_nativeInvalidate(JNIEnv *, jobject) {
  latai_rn::invalidate();
}
