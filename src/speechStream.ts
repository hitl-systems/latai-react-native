// SPDX-License-Identifier: Apache-2.0 OR MIT
// A stream session on a speech pack (C ABI v2: stream_open / push / cancel / close). The stream has no
// thread of its own: the glue gives each session one native thread, every push and the close run on
// it in order, and the events are delivered here on the JS thread.

import { LataiError } from './errors';
import type { NativeApi } from './native';
import { EventQueue } from './queue';
import type { StreamEvent, StreamOpen } from './types.g';

/** The most audio one native push takes. */
export const MAX_CHUNK_BYTES = 1 << 20;

export class SpeechStream {
  /**
   * `partial`, `committed`, `utterance`, `then`, and exactly one terminal `final` or `error`. Partials
   * may be revised; committed text never changes. One consumer; ends when the stream is closed.
   */
  readonly events: AsyncIterable<StreamEvent>;

  private closed = false;
  private chain: Promise<unknown> = Promise.resolve();

  private constructor(
    private readonly native: NativeApi,
    private readonly handle: number,
    private readonly queue: EventQueue<StreamEvent>,
    private readonly onClosed: (s: SpeechStream) => void,
  ) {
    this.events = queue;
  }

  /** Opened by `Runtime.listen`. */
  static async open(native: NativeApi, runtime: number, open: StreamOpen, onClosed: (s: SpeechStream) => void): Promise<SpeechStream> {
    const queue = new EventQueue<StreamEvent>();
    const opening: StreamEvent[] = []; // what the call to `stream_open` itself delivers
    let opened = false;
    const r = await native.streamOpen(runtime, JSON.stringify(open), (json) => {
      const event = JSON.parse(json) as StreamEvent;
      if (!opened) opening.push(event);
      queue.push(event);
    });
    if (r.rc !== 0) {
      // A stream that cannot open is one `error` event and its code: reject with the event attached.
      queue.end();
      throw LataiError.fromReturn(r.rc, r.err, { events: opening });
    }
    opened = true;
    return new SpeechStream(native, r.handle, queue, onClosed);
  }

  /**
   * Gives the stream audio, in order: raw 16-bit little-endian mono samples, or WAV bytes when the stream
   * was opened with `format: 'wav'`. Any size (split into pushes of at most 1 MiB); calls queue in order.
   * A push that completes a hop waits for the engine (backpressure). Rejects with the code of the error that
   * ended the stream in this call (its `final` is still delivered on `events`), or `E0320` when it had
   * already ended.
   */
  push(audio: Uint8Array | ArrayBuffer): Promise<void> {
    if (this.closed) return Promise.reject(new Error('the SpeechStream is closed'));
    const bytes = audio instanceof Uint8Array ? audio : new Uint8Array(audio);
    const run = this.chain.then(async () => {
      for (let at = 0; at < bytes.length; at += MAX_CHUNK_BYTES) {
        const part = bytes.slice(at, Math.min(at + MAX_CHUNK_BYTES, bytes.length));
        const r = await this.native.streamPush(this.handle, part.buffer as ArrayBuffer);
        if (r.rc !== 0) throw LataiError.fromReturn(r.rc, r.err);
      }
    });
    this.chain = run.catch(() => undefined);
    return run;
  }

  /** Stops the stream without queueing behind a push: the pass in flight ends and the `final` (`cancelled`) is delivered by the push or close that runs next. */
  cancel(): void {
    if (!this.closed) this.native.streamCancel(this.handle);
  }

  /**
   * Ends the stream and releases its handle. With `flush` (the default) the audio is over: the last
   * utterance is decoded and the `final` has `finish: 'stop'`; with `flush: false` the stream is
   * abandoned and the `final` is `cancelled`. `events` ends afterwards.
   */
  async close(options: { flush?: boolean } = {}): Promise<void> {
    if (this.closed) return;
    this.closed = true;
    await this.chain;
    try {
      const r = await this.native.streamClose(this.handle, options.flush !== false);
      if (r.rc !== 0) throw LataiError.fromReturn(r.rc, r.err);
    } finally {
      this.queue.end();
      this.onClosed(this);
    }
  }
}
