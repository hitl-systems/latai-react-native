// SPDX-License-Identifier: Apache-2.0 OR MIT
// The JSI object installed by cpp/LataiJsi.cpp as `global.__latai`, and how to get it.

import { NativeModules } from 'react-native';

/** What every asynchronous native call resolves with. */
export interface NativeResult {
  /** The return code of the C ABI call: 0 is `LATAI_OK`, otherwise the number of the error. */
  rc: number;
  /** The string the call returned in `*out`, or null. */
  out: string | null;
  /** The text of `last_error` for a failed call, read on the thread that failed, or null. */
  err: string | null;
  /** The runtime or stream id, for `open` and `streamOpen`. */
  handle: number;
}

/** Host functions of `global.__latai`. A handle is an id, not an address. */
export interface NativeApi {
  /** Set instead of the functions when liblatai does not serve the C API version 2. */
  error?: string;
  open(config: string): Promise<NativeResult>;
  close(rt: number): Promise<NativeResult>;
  info(rt: number): Promise<NativeResult>;
  run(rt: number, request: string, requestId: string | null): Promise<NativeResult>;
  /** `request` must carry `"stream":true`. `onEvent` is called on the JS thread, in order. */
  runStream(rt: number, request: string, requestId: string | null, onEvent: (eventJson: string) => void): Promise<NativeResult>;
  /** Synchronous; any thread; returns the return code. */
  cancel(rt: number, requestId: string): number;
  sessionClose(rt: number, session: string): Promise<NativeResult>;
  inspect(rt: number, target: string): Promise<NativeResult>;
  stage(rt: number, path: string): Promise<NativeResult>;
  pull(rt: number, url: string, force: boolean): Promise<NativeResult>;
  install(rt: number, source: string, activate: boolean): Promise<NativeResult>;
  activate(rt: number, name: string, version: string | null): Promise<NativeResult>;
  rollback(rt: number, name: string): Promise<NativeResult>;
  list(rt: number): Promise<NativeResult>;
  pin(rt: number, name: string, pin: string | null): Promise<NativeResult>;
  status(rt: number, name: string | null): Promise<NativeResult>;
  streamOpen(rt: number, openJson: string, onEvent: (eventJson: string) => void): Promise<NativeResult>;
  /** Up to 1 MiB. */
  streamPush(stream: number, audio: ArrayBuffer): Promise<NativeResult>;
  /** Synchronous; any thread; returns the return code. */
  streamCancel(stream: number): number;
  streamClose(stream: number, flush: boolean): Promise<NativeResult>;
}

/** The installed API; installs it first (the native module's `install()`) if needed. */
export function nativeApi(): NativeApi {
  const g = globalThis as { __latai?: NativeApi };
  if (g.__latai === undefined) {
    const mod = NativeModules.Latai as { install?: () => boolean } | undefined;
    if (mod === undefined || typeof mod.install !== 'function') {
      throw new Error('latai: the native module "Latai" is not linked (rebuild the app after adding latai-react-native)');
    }
    if (mod.install() !== true) {
      throw new Error('latai: the JSI install failed (is the app running on the new architecture bridgeless mode? see README)');
    }
  }
  const api = g.__latai;
  if (api === undefined) throw new Error('latai: global.__latai is missing after install');
  if (api.error !== undefined) throw new Error(`latai: ${api.error}`);
  return api;
}
