// SPDX-License-Identifier: Apache-2.0 OR MIT
// A push-fed async iterable (no async generators: they are transformed on Hermes). Events arrive on
// the JS thread from the native callback, so no locking is needed.

export class EventQueue<T> implements AsyncIterable<T> {
  private items: T[] = [];
  private waiters: Array<{ resolve: (r: IteratorResult<T>) => void; reject: (e: unknown) => void }> = [];
  private finished = false;
  private failure: { error: unknown } | undefined;

  /** @param onAbandon called once if the consumer stops early (`break`, `return()`). */
  constructor(private readonly onAbandon?: () => void) {}

  push(item: T): void {
    if (this.finished) return;
    const w = this.waiters.shift();
    if (w) w.resolve({ value: item, done: false });
    else this.items.push(item);
  }

  /** Ends the stream; pending and later `next()` calls see `done`, or `error` once buffered items are read. */
  end(error?: unknown): void {
    if (this.finished) return;
    this.finished = true;
    if (error !== undefined) this.failure = { error };
    for (const w of this.waiters.splice(0)) {
      if (this.failure) w.reject(this.failure.error);
      else w.resolve({ value: undefined, done: true });
    }
  }

  [Symbol.asyncIterator](): AsyncIterator<T> {
    return {
      next: (): Promise<IteratorResult<T>> => {
        const item = this.items.shift();
        if (item !== undefined) return Promise.resolve({ value: item, done: false });
        if (this.finished) {
          if (this.failure) {
            const e = this.failure.error;
            this.failure = undefined;
            return Promise.reject(e);
          }
          return Promise.resolve({ value: undefined, done: true });
        }
        return new Promise((resolve, reject) => this.waiters.push({ resolve, reject }));
      },
      return: (): Promise<IteratorResult<T>> => {
        if (!this.finished) this.onAbandon?.();
        this.end();
        return Promise.resolve({ value: undefined, done: true });
      },
    };
  }
}
