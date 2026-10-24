// SPDX-License-Identifier: Apache-2.0 OR MIT
package ai.latai.rn;

import com.facebook.react.bridge.ReactApplicationContext;
import com.facebook.react.bridge.ReactContextBaseJavaModule;
import com.facebook.react.bridge.ReactMethod;
import com.facebook.react.turbomodule.core.CallInvokerHolderImpl;

/**
 * Installs {@code global.__latai} (cpp/LataiJsi.cpp) into the JS runtime. The module has one
 * method, {@code install()}, which the TypeScript layer calls synchronously the first time it
 * needs the API. Everything else is JSI.
 */
public class LataiModule extends ReactContextBaseJavaModule {
  static {
    System.loadLibrary("latai"); // liblatai.so first, from jniLibs/<abi>
    System.loadLibrary("latai_rn");
  }

  public LataiModule(ReactApplicationContext context) {
    super(context);
  }

  @Override
  public String getName() {
    return "Latai";
  }

  @ReactMethod(isBlockingSynchronousMethod = true)
  public boolean install() {
    try {
      ReactApplicationContext context = getReactApplicationContext();
      long runtime = context.getJavaScriptContextHolder().get();
      if (runtime == 0) return false;
      CallInvokerHolderImpl holder =
          (CallInvokerHolderImpl) context.getCatalystInstance().getJSCallInvokerHolder();
      nativeInstall(runtime, holder);
      return true;
    } catch (RuntimeException e) {
      return false;
    }
  }

  @Override
  public void invalidate() {
    nativeInvalidate();
    super.invalidate();
  }

  private native void nativeInstall(long runtime, CallInvokerHolderImpl callInvokerHolder);

  private native void nativeInvalidate();
}
