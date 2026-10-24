// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// Threading model (see docs/mobile-sdks.md):
//   * The C ABI blocks (run, run_stream, stage, pull, stream_push, ...) and its callbacks run on
//     the thread that made the call. So no table call is ever made on the JS thread: each runs on
//     a worker thread (an Executor), and its result, and every event, is posted back with
//     `CallInvoker::invokeAsync`, which runs it on the JS thread. A JS function is only ever
//     touched there.
//   * One Executor for the calls that wait for the run queue (run, run_stream, stage, pull,
//     install, activate, rollback), one for the quick ones (info, status, list, inspect, pin,
//     session_close, open, close), and one per stream session (its events are delivered on the
//     thread that pushes, so its pushes must come from one thread, in order).
//   * `last_error` is per thread: it is read on the worker, right after the failing call.
//   * Events and results are posted in order on the same FIFO, so every `onEvent` of a run is
//     called before its promise resolves.
//   * A `jsi::Function` held across threads lives in a shared_ptr whose deleter deletes it on the
//     JS thread.
#include "LataiJsi.h"

#include <ReactCommon/CallInvoker.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "latai.h"

namespace jsi = facebook::jsi;

namespace latai_rn {
namespace {

// -- a worker thread -------------------------------------------------------------------------

class Executor {
 public:
  Executor() : thread_([this] { loop(); }) {}
  ~Executor() {
    {
      std::lock_guard<std::mutex> lock(mu_);
      stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
  }
  Executor(const Executor &) = delete;
  Executor &operator=(const Executor &) = delete;

  void post(std::function<void()> task) {
    {
      std::lock_guard<std::mutex> lock(mu_);
      queue_.push_back(std::move(task));
    }
    cv_.notify_one();
  }

 private:
  void loop() {
    for (;;) {
      std::function<void()> task;
      {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
        if (queue_.empty()) return;  // stopped and drained
        task = std::move(queue_.front());
        queue_.pop_front();
      }
      task();
    }
  }

  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> queue_;
  bool stop_ = false;
  std::thread thread_;  // last: starts after the other members exist
};

// -- the result of one table call ------------------------------------------------------------

struct Result {
  int32_t rc = 0;
  bool has_out = false;
  std::string out;
  bool has_err = false;
  std::string err;  // the bytes of last_error, read on the worker thread
  int handle = 0;
};

struct JsFn;

struct State {
  jsi::Runtime *rt = nullptr;
  std::shared_ptr<facebook::react::CallInvoker> invoker;
  const LataiApi *api = nullptr;
  std::atomic<bool> alive{true};
  Executor runner;  // calls that wait for the run queue
  Executor query;   // quick calls

  std::mutex mu;  // guards the handle tables
  int next_id = 1;
  std::map<int, LataiRuntime *> runtimes;
  struct Stream;
  std::map<int, std::shared_ptr<Stream>> streams;
};

std::shared_ptr<State> g_state;
// A replaced state is kept, never destroyed: its destructor joins the worker threads, and the last
// reference to it may be dropped on one of them.
std::vector<std::shared_ptr<State>> g_retired;

// A jsi::Function that any thread may copy and drop: the last drop deletes it on the JS thread.
using JsFnPtr = std::shared_ptr<jsi::Function>;

JsFnPtr share_fn(const std::shared_ptr<State> &st, jsi::Function &&fn) {
  return JsFnPtr(new jsi::Function(std::move(fn)), [st](jsi::Function *f) {
    st->invoker->invokeAsync([st, f] {
      if (st->alive.load()) delete f;  // else the runtime is gone: leaking beats touching it
    });
  });
}

// `last_error` into a string (NULL-buffer rule); empty when none.
bool read_last_error(const LataiApi *api, std::string *out) {
  size_t len = 0;
  int32_t rc = api->last_error(nullptr, &len);
  for (int attempt = 0; attempt < 4 && rc == LATAI_OK; attempt++) {
    std::vector<char> buf(len > 0 ? len : 1);
    size_t capacity = len;
    rc = api->last_error(buf.data(), &len);
    if (rc == LATAI_OK && len <= capacity) {
      out->assign(buf.data(), len > 0 ? len - 1 : 0);
      return true;
    }
  }
  return false;
}

Result finish(const LataiApi *api, int32_t rc) {
  Result r;
  r.rc = rc;
  if (rc != LATAI_OK) r.has_err = read_last_error(api, &r.err);
  return r;
}

// Calls `call(char **out, size_t *len)`, copies and frees the string, reads last_error on failure.
template <typename F>
Result with_out(const LataiApi *api, F call) {
  char *out = nullptr;
  size_t len = 0;
  int32_t rc = call(&out, &len);
  Result r = finish(api, rc);
  if (out != nullptr) {  // `run` answers an error with a body: free whenever not NULL
    r.has_out = true;
    r.out.assign(out, len);
    api->free(out);
  }
  return r;
}

jsi::Value to_js(jsi::Runtime &rt, const Result &r) {
  jsi::Object o(rt);
  o.setProperty(rt, "rc", static_cast<double>(r.rc));
  o.setProperty(rt, "out", r.has_out ? jsi::Value(rt, jsi::String::createFromUtf8(rt, r.out)) : jsi::Value::null());
  o.setProperty(rt, "err", r.has_err ? jsi::Value(rt, jsi::String::createFromUtf8(rt, r.err)) : jsi::Value::null());
  o.setProperty(rt, "handle", static_cast<double>(r.handle));
  return jsi::Value(std::move(o));
}

// -- promises --------------------------------------------------------------------------------

struct PromiseSlots {
  JsFnPtr resolve;
};

// Creates a Promise on the JS thread and returns it with the resolver in `slots`.
jsi::Value make_promise(const std::shared_ptr<State> &st, jsi::Runtime &rt, std::shared_ptr<PromiseSlots> slots) {
  jsi::Function ctor = rt.global().getPropertyAsFunction(rt, "Promise");
  jsi::Function executor = jsi::Function::createFromHostFunction(
      rt, jsi::PropNameID::forAscii(rt, "executor"), 2,
      [st, slots](jsi::Runtime &rt2, const jsi::Value &, const jsi::Value *args, size_t) -> jsi::Value {
        slots->resolve = share_fn(st, args[0].asObject(rt2).asFunction(rt2));
        return jsi::Value::undefined();
      });
  return ctor.callAsConstructor(rt, executor);
}

// Runs `work` on `ex`, resolves the returned promise with its Result on the JS thread. `after`
// (optional) runs on the JS thread before the promise resolves, with the Result.
jsi::Value async_call(const std::shared_ptr<State> &st, jsi::Runtime &rt, Executor &ex, std::function<Result()> work,
                      std::function<void(const Result &)> after = nullptr) {
  auto slots = std::make_shared<PromiseSlots>();
  jsi::Value promise = make_promise(st, rt, slots);
  ex.post([st, slots, work = std::move(work), after = std::move(after)]() mutable {
    Result r = work();
    auto resolve = std::move(slots->resolve);  // the JS-thread lambda becomes the owner
    st->invoker->invokeAsync([st, resolve = std::move(resolve), r = std::move(r), after = std::move(after)] {
      if (!st->alive.load()) return;
      if (after) after(r);
      try {
        resolve->call(*st->rt, to_js(*st->rt, r));
      } catch (const jsi::JSError &) {
        // a rejected or throwing continuation is the JS side's problem
      }
    });
  });
  return promise;
}

// -- events ----------------------------------------------------------------------------------

// The `user_data` of the event callbacks: where to deliver, kept alive by its owner for as long
// as the C side may call back (run_stream: the worker's stack frame; a stream: its session).
struct Sink {
  std::shared_ptr<State> st;
  JsFnPtr on_event;
};

// LataiEventCallback. Runs on the worker thread that made the table call. `event_json` is valid
// only here: copy it. Always returns 0: cancellation is `cancel` / `streamCancel`, which cannot
// wait for the JS thread.
int32_t on_event(void *user_data, const char *event_json, size_t len) {
  auto *sink = static_cast<Sink *>(user_data);
  std::string text(event_json, len);
  sink->st->invoker->invokeAsync([st = sink->st, fn = sink->on_event, text = std::move(text)] {
    if (!st->alive.load()) return;
    try {
      jsi::Runtime &rt = *st->rt;
      fn->call(rt, jsi::String::createFromUtf8(rt, text));
    } catch (const jsi::JSError &) {
      // an exception in the app's handler must not unwind into the invoker
    }
  });
  return 0;
}

// A stream session: its handle, its own thread, and the sink the C side calls back.
struct State::Stream {
  // Written once by the stream's worker thread (stream_open), read by it and by the JS thread
  // (streamCancel): atomic, so a cancel that races the open sees NULL (E0302) or the handle.
  std::atomic<LataiStream *> stream{nullptr};
  Sink sink;
  Executor exec;
  bool closing = false;  // JS thread only
};

// -- argument helpers ------------------------------------------------------------------------

std::string str_arg(jsi::Runtime &rt, const jsi::Value *args, size_t count, size_t i) {
  if (i >= count || !args[i].isString()) throw jsi::JSError(rt, "latai: argument " + std::to_string(i) + " must be a string");
  return args[i].asString(rt).utf8(rt);
}

struct OptStr {
  bool is_null = true;
  std::string s;
  const char *c() const { return is_null ? nullptr : s.c_str(); }
};

OptStr opt_str_arg(jsi::Runtime &rt, const jsi::Value *args, size_t count, size_t i) {
  OptStr o;
  if (i < count && args[i].isString()) {
    o.is_null = false;
    o.s = args[i].asString(rt).utf8(rt);
  }
  return o;
}

int int_arg(const jsi::Value *args, size_t count, size_t i) {
  return i < count && args[i].isNumber() ? static_cast<int>(args[i].getNumber()) : 0;
}

bool bool_arg(const jsi::Value *args, size_t count, size_t i) {
  return i < count && args[i].isBool() && args[i].getBool();
}

LataiRuntime *runtime_of(const std::shared_ptr<State> &st, int id) {
  std::lock_guard<std::mutex> lock(st->mu);
  auto it = st->runtimes.find(id);
  return it == st->runtimes.end() ? nullptr : it->second;
}

Result bad_handle(const char *what) {
  Result r;
  r.rc = 302;  // E0302
  r.has_err = true;
  r.err = std::string("{\"code\":\"E0302\",\"message\":\"") + what + " is closed or unknown\",\"hint\":\"\"}";
  return r;
}

using HostFn = std::function<jsi::Value(jsi::Runtime &, const jsi::Value *, size_t)>;

void def(jsi::Runtime &rt, jsi::Object &obj, const char *name, unsigned params, HostFn fn) {
  obj.setProperty(rt, name,
                  jsi::Function::createFromHostFunction(
                      rt, jsi::PropNameID::forAscii(rt, name), params,
                      [fn = std::move(fn)](jsi::Runtime &r, const jsi::Value &, const jsi::Value *args, size_t count) {
                        return fn(r, args, count);
                      }));
}

}  // namespace

void install(jsi::Runtime &rt, std::shared_ptr<facebook::react::CallInvoker> invoker) {
  if (g_state) {  // a reload installs again: drop the old state's results
    g_state->alive.store(false);
    g_retired.push_back(g_state);
  }
  auto st = std::make_shared<State>();
  st->rt = &rt;
  st->invoker = std::move(invoker);
  st->api = latai_get_api(LATAI_API_VERSION);
  g_state = st;
  jsi::Object api(rt);
  if (st->api == nullptr) {
    api.setProperty(rt, "error", jsi::String::createFromUtf8(rt, "liblatai does not serve C API version 2"));
    rt.global().setProperty(rt, "__latai", std::move(api));
    return;
  }
  const LataiApi *c = st->api;

  // open(config: string) -> Promise<Result>; result.handle is the runtime id.
  def(rt, api, "open", 1, [st, c](jsi::Runtime &rt, const jsi::Value *a, size_t n) {
    std::string cfg = n > 0 && a[0].isString() ? a[0].asString(rt).utf8(rt) : std::string();
    return async_call(st, rt, st->query, [st, c, cfg] {
      LataiRuntime *h = nullptr;
      int32_t rc = c->open(cfg.empty() ? nullptr : cfg.data(), cfg.size(), &h);
      Result r = finish(c, rc);
      if (rc == LATAI_OK) {
        std::lock_guard<std::mutex> lock(st->mu);
        r.handle = st->next_id++;
        st->runtimes[r.handle] = h;
      }
      return r;
    });
  });

  def(rt, api, "close", 1, [st, c](jsi::Runtime &rt, const jsi::Value *a, size_t n) {
    int id = int_arg(a, n, 0);
    LataiRuntime *h = runtime_of(st, id);
    return async_call(st, rt, st->query, [st, c, id, h] {
      if (h == nullptr) return bad_handle("runtime");
      Result r = finish(c, c->close(h));
      std::lock_guard<std::mutex> lock(st->mu);
      st->runtimes.erase(id);
      return r;
    });
  });

  def(rt, api, "info", 1, [st, c](jsi::Runtime &rt, const jsi::Value *a, size_t n) {
    LataiRuntime *h = runtime_of(st, int_arg(a, n, 0));
    return async_call(st, rt, st->query, [c, h] {
      if (h == nullptr) return bad_handle("runtime");
      std::string text;
      size_t len = 0;
      int32_t rc = c->info(h, nullptr, &len);
      for (int attempt = 0; attempt < 4 && rc == LATAI_OK; attempt++) {
        std::vector<char> buf(len > 0 ? len : 1);
        size_t capacity = len;
        rc = c->info(h, buf.data(), &len);
        if (rc == LATAI_OK && len <= capacity) {
          Result r;
          r.has_out = true;
          r.out.assign(buf.data(), len > 0 ? len - 1 : 0);
          return r;
        }
      }
      return finish(c, rc);
    });
  });

  // run(h, request, requestId|null) -> Promise<Result>; result.out is the response JSON whenever
  // the request reached the core, also when rc != 0.
  def(rt, api, "run", 3, [st, c](jsi::Runtime &rt, const jsi::Value *a, size_t n) {
    LataiRuntime *h = runtime_of(st, int_arg(a, n, 0));
    std::string req = str_arg(rt, a, n, 1);
    OptStr id = opt_str_arg(rt, a, n, 2);
    return async_call(st, rt, st->runner, [c, h, req, id] {
      if (h == nullptr) return bad_handle("runtime");
      return with_out(c, [&](char **out, size_t *len) { return c->run(h, req.data(), req.size(), id.c(), out, len); });
    });
  });

  // runStream(h, request, requestId|null, onEvent: (json: string) => void) -> Promise<Result>.
  // The request must carry "stream":true (the TypeScript layer sets it). Cancel with `cancel`.
  def(rt, api, "runStream", 4, [st, c](jsi::Runtime &rt, const jsi::Value *a, size_t n) {
    LataiRuntime *h = runtime_of(st, int_arg(a, n, 0));
    std::string req = str_arg(rt, a, n, 1);
    OptStr id = opt_str_arg(rt, a, n, 2);
    if (n < 4 || !a[3].isObject()) throw jsi::JSError(rt, "latai: runStream needs an onEvent function");
    JsFnPtr on = share_fn(st, a[3].asObject(rt).asFunction(rt));
    return async_call(st, rt, st->runner, [st, c, h, req, id, on] {
      if (h == nullptr) return bad_handle("runtime");
      Sink sink{st, on};  // lives on this frame: run_stream returns after the last callback
      return finish(c, c->run_stream(h, req.data(), req.size(), id.c(), on_event, &sink));
    });
  });

  // cancel(h, requestId) -> number. Synchronous: any thread, never blocks.
  def(rt, api, "cancel", 2, [st, c](jsi::Runtime &rt, const jsi::Value *a, size_t n) {
    LataiRuntime *h = runtime_of(st, int_arg(a, n, 0));
    std::string id = str_arg(rt, a, n, 1);
    return jsi::Value(h == nullptr ? 302.0 : static_cast<double>(c->cancel(h, id.c_str())));
  });

  def(rt, api, "sessionClose", 2, [st, c](jsi::Runtime &rt, const jsi::Value *a, size_t n) {
    LataiRuntime *h = runtime_of(st, int_arg(a, n, 0));
    std::string session = str_arg(rt, a, n, 1);
    return async_call(st, rt, st->query, [c, h, session] {
      if (h == nullptr) return bad_handle("runtime");
      return finish(c, c->session_close(h, session.c_str()));
    });
  });

  // -- packs -----------------------------------------------------------------------------------
  def(rt, api, "inspect", 2, [st, c](jsi::Runtime &rt, const jsi::Value *a, size_t n) {
    LataiRuntime *h = runtime_of(st, int_arg(a, n, 0));
    std::string target = str_arg(rt, a, n, 1);
    return async_call(st, rt, st->query, [c, h, target] {
      if (h == nullptr) return bad_handle("runtime");
      return with_out(c, [&](char **o, size_t *l) { return c->inspect(h, target.c_str(), o, l); });
    });
  });

  def(rt, api, "stage", 2, [st, c](jsi::Runtime &rt, const jsi::Value *a, size_t n) {
    LataiRuntime *h = runtime_of(st, int_arg(a, n, 0));
    std::string path = str_arg(rt, a, n, 1);
    return async_call(st, rt, st->runner, [c, h, path] {
      if (h == nullptr) return bad_handle("runtime");
      return with_out(c, [&](char **o, size_t *l) { return c->stage(h, path.c_str(), o, l); });
    });
  });

  def(rt, api, "pull", 3, [st, c](jsi::Runtime &rt, const jsi::Value *a, size_t n) {
    LataiRuntime *h = runtime_of(st, int_arg(a, n, 0));
    std::string url = str_arg(rt, a, n, 1);
    bool force = bool_arg(a, n, 2);
    return async_call(st, rt, st->runner, [c, h, url, force] {
      if (h == nullptr) return bad_handle("runtime");
      return with_out(c, [&](char **o, size_t *l) { return c->pull(h, url.c_str(), force ? 1 : 0, o, l); });
    });
  });

  def(rt, api, "install", 3, [st, c](jsi::Runtime &rt, const jsi::Value *a, size_t n) {
    LataiRuntime *h = runtime_of(st, int_arg(a, n, 0));
    std::string source = str_arg(rt, a, n, 1);
    bool activate = bool_arg(a, n, 2);
    return async_call(st, rt, st->runner, [c, h, source, activate] {
      if (h == nullptr) return bad_handle("runtime");
      return with_out(c, [&](char **o, size_t *l) { return c->install(h, source.c_str(), activate ? 1 : 0, o, l); });
    });
  });

  def(rt, api, "activate", 3, [st, c](jsi::Runtime &rt, const jsi::Value *a, size_t n) {
    LataiRuntime *h = runtime_of(st, int_arg(a, n, 0));
    std::string name = str_arg(rt, a, n, 1);
    OptStr version = opt_str_arg(rt, a, n, 2);
    return async_call(st, rt, st->runner, [c, h, name, version] {
      if (h == nullptr) return bad_handle("runtime");
      return with_out(c, [&](char **o, size_t *l) { return c->activate(h, name.c_str(), version.c(), o, l); });
    });
  });

  def(rt, api, "rollback", 2, [st, c](jsi::Runtime &rt, const jsi::Value *a, size_t n) {
    LataiRuntime *h = runtime_of(st, int_arg(a, n, 0));
    std::string name = str_arg(rt, a, n, 1);
    return async_call(st, rt, st->runner, [c, h, name] {
      if (h == nullptr) return bad_handle("runtime");
      return with_out(c, [&](char **o, size_t *l) { return c->rollback(h, name.c_str(), o, l); });
    });
  });

  def(rt, api, "list", 1, [st, c](jsi::Runtime &rt, const jsi::Value *a, size_t n) {
    LataiRuntime *h = runtime_of(st, int_arg(a, n, 0));
    return async_call(st, rt, st->query, [c, h] {
      if (h == nullptr) return bad_handle("runtime");
      return with_out(c, [&](char **o, size_t *l) { return c->list(h, o, l); });
    });
  });

  def(rt, api, "pin", 3, [st, c](jsi::Runtime &rt, const jsi::Value *a, size_t n) {
    LataiRuntime *h = runtime_of(st, int_arg(a, n, 0));
    std::string name = str_arg(rt, a, n, 1);
    OptStr pin = opt_str_arg(rt, a, n, 2);
    return async_call(st, rt, st->query, [c, h, name, pin] {
      if (h == nullptr) return bad_handle("runtime");
      return with_out(c, [&](char **o, size_t *l) { return c->pin(h, name.c_str(), pin.c(), o, l); });
    });
  });

  def(rt, api, "status", 2, [st, c](jsi::Runtime &rt, const jsi::Value *a, size_t n) {
    LataiRuntime *h = runtime_of(st, int_arg(a, n, 0));
    OptStr name = opt_str_arg(rt, a, n, 1);
    return async_call(st, rt, st->query, [c, h, name] {
      if (h == nullptr) return bad_handle("runtime");
      return with_out(c, [&](char **o, size_t *l) { return c->status(h, name.c(), o, l); });
    });
  });

  // `stage_child` is the table entry for the `latai __stage-child` process alone (it limits the
  // address space of the process that calls it): this glue never calls it, on purpose.

  // -- streams (API v2) ------------------------------------------------------------------------

  // streamOpen(h, openJson, onEvent) -> Promise<Result>; result.handle is the stream id. Events
  // reach onEvent for the life of the stream, on the JS thread.
  def(rt, api, "streamOpen", 3, [st, c](jsi::Runtime &rt, const jsi::Value *a, size_t n) {
    LataiRuntime *h = runtime_of(st, int_arg(a, n, 0));
    std::string json = str_arg(rt, a, n, 1);
    if (n < 3 || !a[2].isObject()) throw jsi::JSError(rt, "latai: streamOpen needs an onEvent function");
    auto stream = std::make_shared<State::Stream>();
    stream->sink = Sink{st, share_fn(st, a[2].asObject(rt).asFunction(rt))};
    State::Stream *raw = stream.get();
    int id;
    {
      std::lock_guard<std::mutex> lock(st->mu);
      id = st->next_id++;
      st->streams[id] = stream;
    }
    return async_call(
        st, rt, raw->exec,
        [c, h, json, raw, id] {
          if (h == nullptr) return bad_handle("runtime");
          LataiStream *opened = nullptr;
          Result r = finish(c, c->stream_open(h, json.data(), json.size(), on_event, &raw->sink, &opened));
          if (r.rc == LATAI_OK && opened != nullptr) {
            raw->stream.store(opened);
            r.handle = id;
          }
          return r;
        },
        [st, id](const Result &r) {  // JS thread: forget a stream that did not open
          if (r.rc != LATAI_OK) {
            std::lock_guard<std::mutex> lock(st->mu);
            st->streams.erase(id);
          }
        });
  });

  // streamPush(stream, audio: ArrayBuffer) -> Promise<Result>. Up to 1 MiB per call.
  def(rt, api, "streamPush", 2, [st, c](jsi::Runtime &rt, const jsi::Value *a, size_t n) {
    std::shared_ptr<State::Stream> s;
    {
      std::lock_guard<std::mutex> lock(st->mu);
      auto it = st->streams.find(int_arg(a, n, 0));
      if (it != st->streams.end() && !it->second->closing) s = it->second;
    }
    std::vector<uint8_t> audio;
    if (n > 1 && a[1].isObject() && a[1].asObject(rt).isArrayBuffer(rt)) {
      jsi::ArrayBuffer buf = a[1].asObject(rt).getArrayBuffer(rt);
      audio.assign(buf.data(rt), buf.data(rt) + buf.size(rt));
    }
    if (!s) return async_call(st, rt, st->query, [] { return bad_handle("stream"); });
    State::Stream *raw = s.get();  // stays in the table until close resolves
    return async_call(st, rt, raw->exec, [c, raw, audio = std::move(audio)] {
      return finish(c, c->stream_push(raw->stream.load(), audio.data(), audio.size()));
    });
  });

  // streamCancel(stream) -> number. Synchronous: any thread, never blocks.
  def(rt, api, "streamCancel", 1, [st, c](jsi::Runtime &, const jsi::Value *a, size_t n) {
    std::lock_guard<std::mutex> lock(st->mu);
    auto it = st->streams.find(int_arg(a, n, 0));
    if (it == st->streams.end() || it->second->closing) return jsi::Value(302.0);
    return jsi::Value(static_cast<double>(c->stream_cancel(it->second->stream.load())));
  });

  // streamClose(stream, flush) -> Promise<Result>. Delivers the last events, then releases the stream.
  def(rt, api, "streamClose", 2, [st, c](jsi::Runtime &rt, const jsi::Value *a, size_t n) {
    int id = int_arg(a, n, 0);
    bool flush = bool_arg(a, n, 1);
    std::shared_ptr<State::Stream> s;
    {
      std::lock_guard<std::mutex> lock(st->mu);
      auto it = st->streams.find(id);
      if (it != st->streams.end() && !it->second->closing) {
        s = it->second;
        s->closing = true;
      }
    }
    if (!s) return async_call(st, rt, st->query, [] { return bad_handle("stream"); });
    State::Stream *raw = s.get();
    return async_call(
        st, rt, raw->exec, [c, raw, flush] { return finish(c, c->stream_close(raw->stream.load(), flush ? 1 : 0)); },
        [st, id](const Result &) {  // JS thread: after the last event was delivered, drop the session
          std::shared_ptr<State::Stream> dead;
          {
            std::lock_guard<std::mutex> lock(st->mu);
            auto it = st->streams.find(id);
            if (it != st->streams.end()) {
              dead = std::move(it->second);
              st->streams.erase(it);
            }
          }
          // `dead` (and its worker thread) is released here, on the JS thread, not on the worker.
        });
  });

  rt.global().setProperty(rt, "__latai", std::move(api));
}

void invalidate() {
  if (g_state) g_state->alive.store(false);
}

}  // namespace latai_rn
