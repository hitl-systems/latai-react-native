// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// JSI glue of the React Native SDK: installs `global.__latai`, a plain object of host functions
// over the latai C ABI (include/latai.h, API v2). Platform code (ios/LataiModule.mm,
// android/src/main/cpp/cpp-adapter.cpp) only calls `install` with the JS runtime and the JS
// call invoker. The TypeScript layer (src/) is the public API.
#pragma once

#include <jsi/jsi.h>

#include <memory>

namespace facebook::react {
class CallInvoker;
}

namespace latai_rn {

// Installs `global.__latai` into `rt`. Call it on the JS thread. `invoker` hops work onto the JS
// thread (`invokeAsync`); every callback and promise of this glue is delivered through it.
void install(facebook::jsi::Runtime &rt, std::shared_ptr<facebook::react::CallInvoker> invoker);

// Call when the JS runtime is going away (module invalidate): pending results are dropped, not
// delivered into a dead runtime. Worker threads are joined by the destructors of the state.
void invalidate();

}  // namespace latai_rn
