// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// Installs `global.__latai` (cpp/LataiJsi.cpp) into the JS runtime of the bridge. The module has
// one method, `install`, which the TypeScript layer calls synchronously the first time it needs
// the API. Everything else is JSI. Covers the bridge (old architecture and the new architecture
// with the interop layer); bridgeless mode needs a TurboModule install (README, "Unverified").
#import <React/RCTBridge+Private.h>
#import <React/RCTBridge.h>
#import <React/RCTBridgeModule.h>

#import "LataiJsi.h"

@interface LataiModule : NSObject <RCTBridgeModule>
@end

@implementation LataiModule

@synthesize bridge = _bridge;

RCT_EXPORT_MODULE(Latai)

+ (BOOL)requiresMainQueueSetup {
  return NO;
}

RCT_EXPORT_BLOCKING_SYNCHRONOUS_METHOD(install) {
  RCTCxxBridge *cxxBridge = (RCTCxxBridge *)_bridge;
  if (cxxBridge == nil || cxxBridge.runtime == nullptr) {
    return @NO;
  }
  auto *runtime = reinterpret_cast<facebook::jsi::Runtime *>(cxxBridge.runtime);
  latai_rn::install(*runtime, cxxBridge.jsCallInvoker);
  return @YES;
}

- (void)invalidate {
  latai_rn::invalidate();
}

@end
