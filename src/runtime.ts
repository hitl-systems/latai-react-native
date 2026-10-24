// SPDX-License-Identifier: Apache-2.0 OR MIT
// The TypeScript API. One Runtime per process (liblatai allows exactly one).
//
// Threads: every native call runs on a native worker thread, never on the JS thread, and its result
// and every event are delivered back on the JS thread, in order (cpp/LataiJsi.cpp). So `await`,
// `for await` and the callbacks here are ordinary JS: setState from them directly.

import { LataiError } from './errors';
import { nativeApi, type NativeApi, type NativeResult } from './native';
import { EventQueue } from './queue';
import { SpeechStream } from './speechStream';
import type {
  Event,
  PackInfo,
  PackVersion,
  Request,
  Response,
  RuntimeInfo,
  StageReceipt,
  Status,
  StreamOpen,
} from './types.g';

/** Throws the typed error of a failed call; returns the `out` string otherwise. */
export function expectOk(r: NativeResult): string {
  if (r.rc !== 0 || r.out === null) throw LataiError.fromReturn(r.rc, r.err);
  return r.out;
}

/** A streaming run: iterate it for events, or `cancel()` it. */
export interface RunStream extends AsyncIterable<Event> {
  /** Cancels the request; the run still ends with one `final` (`finish: 'cancelled'`) unless you stop iterating. */
  cancel(): void;
  readonly requestId: string;
}

/** Options of `Runtime.run`. */
export interface RunOptions {
  /** Names the request for `cancel`. */
  requestId?: string;
  /** Reject when the response ended `finish: 'error'` (default true); else resolve with it. */
  raiseOnError?: boolean;
}

export interface OpenOptions {
  /** The `data_dir` key of `open`; default `LATAI_DATA_DIR` or the OS default. */
  dataDir?: string;
  /** The `bundle_dir` key of `open`. */
  bundleDir?: string;
}

export class Runtime {
  private closed = false;
  private counter = 0;
  private readonly streams = new Set<SpeechStream>();

  private constructor(
    private readonly native: NativeApi,
    private readonly handle: number,
  ) {}

  /** Opens the runtime of this process. A second `open` before `close` rejects with `E0901`. */
  static async open(options: OpenOptions = {}): Promise<Runtime> {
    const native = nativeApi();
    const config: Record<string, string> = {};
    if (options.dataDir !== undefined) config.data_dir = options.dataDir;
    if (options.bundleDir !== undefined) config.bundle_dir = options.bundleDir;
    const r = await native.open(Object.keys(config).length === 0 ? '' : JSON.stringify(config));
    if (r.rc !== 0) throw LataiError.fromReturn(r.rc, r.err);
    return new Runtime(native, r.handle);
  }

  private live(): void {
    if (this.closed) throw new Error('the latai Runtime is closed');
  }

  private async json<T>(call: Promise<NativeResult>): Promise<T> {
    return JSON.parse(expectOk(await call)) as T;
  }

  // -- requests ------------------------------------------------------------------------------

  /**
   * Runs `request` and waits for the response.
   *
   * A response that ended `finish: 'error'` rejects with the typed `LataiError` of its `error`, with the
   * whole response attached (`err.response`; `latai.started` is false in it when the engine was never
   * called). With `{ raiseOnError: false }` the response resolves instead (check `finish`, or call
   * `orThrow`). `cancelled` and `length` are normal results. A request that does not parse (`E0301`)
   * always rejects: the core produced no response.
   */
  async run(request: Request, options: RunOptions = {}): Promise<Response> {
    this.live();
    const r = await this.native.run(this.handle, JSON.stringify(request), options.requestId ?? null);
    if (r.out === null) throw LataiError.fromReturn(r.rc, r.err); // no response: E0301, E0302
    const response = JSON.parse(r.out) as Response;
    // A non-zero return code is the code of `response.error`; `finish: 'error'` always carries one.
    if (options.raiseOnError !== false && (r.rc !== 0 || response.finish === 'error')) {
      throw response.error ? LataiError.fromWire(response.error, { response }) : LataiError.fromReturn(r.rc, r.err, { response });
    }
    return response;
  }

  /**
   * Runs `request` and streams its events: `delta`s, then exactly one `final` or `error`. Breaking out of
   * the `for await` loop (or `cancel()`) cancels the request in liblatai. The SDK sets `"stream":true`
   * (without it the run sends no deltas).
   *
   * **Events are never thrown.** An `error` event, or a `final` whose `finish` is `'error'`, is delivered
   * like any event and the iteration then ends normally (read the event, or `orThrow(final.response)`).
   * Only a call that fails before it delivers a terminal event (a closed runtime, `E0302`, or `E0303`)
   * makes the iteration reject with a `LataiError`.
   */
  runStream(request: Request, options: { requestId?: string } = {}): RunStream {
    this.live();
    const id = options.requestId ?? `rn-${++this.counter}`;
    let terminal = false;
    let ended = false;
    let cancelRequested = false;
    const cancel = () => {
      cancelRequested = true;
      if (!ended && !terminal && !this.closed) this.native.cancel(this.handle, id);
    };
    const queue = new EventQueue<Event>(cancel);
    const body = JSON.stringify({ ...request, stream: true });
    this.native
      .runStream(this.handle, body, id, (json) => {
        const e = JSON.parse(json) as Event;
        if (e.event !== 'delta') terminal = true;
        // A cancel that came before the run started (the worker was busy) was for an id liblatai did
        // not know yet and did nothing: an event means the run is registered now, so ask again.
        else if (cancelRequested && !ended && !this.closed) this.native.cancel(this.handle, id);
        queue.push(e);
      })
      .then(
        (r) => {
          ended = true;
          // E0302 / E0303 are returned with no event; any other failure was delivered as an `error` event.
          if (r.rc !== 0 && !terminal) queue.end(LataiError.fromReturn(r.rc, r.err));
          else queue.end();
        },
        (e) => {
          ended = true;
          queue.end(e);
        },
      );
    return { [Symbol.asyncIterator]: () => queue[Symbol.asyncIterator](), cancel, requestId: id };
  }

  /** Cancels a request by id; an unknown or finished id does nothing. */
  cancel(requestId: string): void {
    this.live();
    this.native.cancel(this.handle, requestId);
  }

  /** Ends a session: its KV slot is freed and its next request runs cold. */
  async sessionClose(session: string): Promise<void> {
    this.live();
    const r = await this.native.sessionClose(this.handle, session);
    if (r.rc !== 0) throw LataiError.fromReturn(r.rc, r.err);
  }

  // -- packs ---------------------------------------------------------------------------------

  /** What a pack says about itself (`target` is a pack file or an installed pack's name). */
  inspect(target: string): Promise<PackInfo> {
    this.live();
    return this.json(this.native.inspect(this.handle, target));
  }

  /** Checks a pack file and its `.minisig` and stores it as a version that is not activated. */
  stage(path: string): Promise<StageReceipt> {
    this.live();
    return this.json(this.native.stage(this.handle, path));
  }

  /** Downloads the pack at `url` (and its `.minisig`) and stages it; `force` ignores the `[pull]` rules. */
  pull(url: string, options: { force?: boolean } = {}): Promise<StageReceipt> {
    this.live();
    return this.json(this.native.pull(this.handle, url, options.force === true));
  }

  /** Stages a pack file, or every pack file of a folder, and activates it unless `activate` is false. */
  install(source: string, options: { activate?: boolean } = {}): Promise<StageReceipt[]> {
    this.live();
    return this.json(this.native.install(this.handle, source, options.activate !== false));
  }

  /** Makes a version of `name` the one that serves (`version`: serial, folder name or sha256; default the newest the pin allows). */
  activate(name: string, version?: string): Promise<PackVersion> {
    this.live();
    return this.json(this.native.activate(this.handle, name, version ?? null));
  }

  /** Brings the retained version of `name` back (`E0803` when there is none). */
  rollback(name: string): Promise<PackVersion> {
    this.live();
    return this.json(this.native.rollback(this.handle, name));
  }

  /** Every installed version with its state and pin. */
  list(): Promise<PackVersion[]> {
    this.live();
    return this.json(this.native.list(this.handle));
  }

  /** Pins `name` to a sha256 or to `channel:NAME`; `null` clears it. Resolves with the pin now in force, or null after a clear. */
  pin(name: string, pin: string | null): Promise<string | null> {
    this.live();
    return this.json(this.native.pin(this.handle, name, pin));
  }

  /** The state of a pack from local state only. */
  status(name: string): Promise<Status> {
    this.live();
    return this.json(this.native.status(this.handle, name));
  }

  /** The runtime's info (version, thread budget, engine variant, data directory, packs, plug-ins). */
  info(): Promise<RuntimeInfo> {
    this.live();
    return this.json(this.native.info(this.handle));
  }

  // -- speech streams ------------------------------------------------------------------------

  /**
   * Opens a stream session on a speech pack (API v2). Push audio with `SpeechStream.push`, read
   * `partial` / `committed` / `utterance` / `then` and one terminal `final` or `error` by iterating
   * `SpeechStream.events`, and end it with `SpeechStream.close`.
   */
  async listen(open: StreamOpen): Promise<SpeechStream> {
    this.live();
    const stream = await SpeechStream.open(this.native, this.handle, open, (s) => this.streams.delete(s));
    this.streams.add(stream);
    return stream;
  }

  // -- lifecycle -----------------------------------------------------------------------------

  /**
   * Closes every open stream, then the runtime: requests in flight are stopped, queued ones are
   * answered `cancelled`. The Runtime is unusable afterwards; a new one may be opened.
   */
  async close(): Promise<void> {
    if (this.closed) return;
    this.closed = true;
    for (const s of [...this.streams]) {
      s.cancel(); // not queued behind a push that is waiting for the engine
      await s.close({ flush: false }).catch(() => undefined);
    }
    const r = await this.native.close(this.handle);
    if (r.rc !== 0) throw LataiError.fromReturn(r.rc, r.err);
  }
}

/**
 * `Runtime.run` already rejects for a response that ended `finish: 'error'`; `orThrow` is for a response
 * obtained with `{ raiseOnError: false }` (or read from a stream's `final` event): it throws the typed
 * error of `error`, with the response attached, and returns the response otherwise.
 */
export function orThrow(response: Response): Response {
  if (response.error) throw LataiError.fromWire(response.error, { response });
  return response;
}
