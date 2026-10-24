// SPDX-License-Identifier: Apache-2.0 OR MIT
// Typed errors. liblatai reports an error as a positive code (206 for E0206) and keeps the wire error
// {"code","message","hint","details"?} for the calling thread (`last_error`); the code's hundreds
// digit is its category (errors.toml).

import type { ErrorCode, Response, StreamEvent, WireError } from './types.g';

export type ErrorCategory =
  | 'pack'
  | 'compat'
  | 'request'
  | 'trust'
  | 'engine'
  | 'access'
  | 'busy'
  | 'lifecycle'
  | 'internal';

const CATEGORIES: ErrorCategory[] = ['pack', 'compat', 'request', 'trust', 'engine', 'access', 'busy', 'lifecycle', 'internal'];

/** What a call can attach to the error it raises. */
export interface ErrorExtras {
  /** `run`: the whole response of a run that ended `finish: 'error'`. */
  response?: Response;
  /** `listen`: the events the failed open delivered (its one `error` event). */
  events?: StreamEvent[];
}

/**
 * An error from liblatai. Check `category`, or `code` ("E0206": stable, meant for programs).
 *
 * Two calls attach more than the error itself: `Runtime.run` rejects with it and the whole `response`
 * when the run ended `finish: 'error'` (unless called with `{ raiseOnError: false }`), and
 * `Runtime.listen` rejects with the `events` of the failed open (the `error` event is among them).
 */
export class LataiError extends Error {
  /** The numeric code: 206 for `E0206`. */
  readonly number: number;
  /** What the caller can do about it. */
  readonly hint: string;
  /** Typed fields of this code, for example `{limit, max, measured}` for `E0206`. */
  readonly details?: Record<string, unknown>;
  /** When a run ended with `finish: 'error'`: the whole response (`latai.timing`; `latai.started` is false if the engine was never called). */
  readonly response?: Response;
  /** For a speech stream that could not open: the events the failed call delivered; else empty. */
  readonly events: StreamEvent[];

  constructor(number: number, message: string, hint = '', details?: Record<string, unknown>, extras: ErrorExtras = {}) {
    super(message);
    this.name = 'LataiError';
    this.number = number;
    this.hint = hint;
    this.details = details;
    this.response = extras.response;
    this.events = extras.events ?? [];
    Object.setPrototypeOf(this, LataiError.prototype);
  }

  /** The code as the docs write it: `E0206`. */
  get code(): ErrorCode | string {
    return 'E' + String(this.number).padStart(4, '0');
  }

  /** `E01xx` is 'pack', and so on. */
  get category(): ErrorCategory {
    return CATEGORIES[Math.floor(this.number / 100) - 1] ?? 'internal';
  }

  /** From a wire error object (`Response.error`, an `error` event). */
  static fromWire(e: WireError, extras: ErrorExtras = {}): LataiError {
    return new LataiError(parseInt(String(e.code).slice(1), 10) || 0, e.message, e.hint, e.details, extras);
  }

  /** From a non-zero return code and the text of `last_error` (read on the failing thread). */
  static fromReturn(rc: number, lastError: string | null | undefined, extras: ErrorExtras = {}): LataiError {
    if (lastError && lastError !== 'null') {
      try {
        const w = JSON.parse(lastError) as { code?: string; message?: string; hint?: string; details?: Record<string, unknown> };
        return new LataiError(parseInt(String(w.code ?? '').slice(1), 10) || rc, w.message ?? 'liblatai error', w.hint ?? '', w.details, extras);
      } catch {
        // not JSON: fall through to the bare code
      }
    }
    return new LataiError(rc, `liblatai returned error code ${rc}`, '', undefined, extras);
  }
}
