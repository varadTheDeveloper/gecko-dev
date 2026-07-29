#include <stdio.h>
#include <string.h>
#include "js/CallAndConstruct.h"
#include "js/String.h"
#include <string>
#include <utility>
#include "js/CompileOptions.h"
#include "js/CompilationAndEvaluation.h"
#include "js/Context.h"
#include "js/GCAPI.h"
#include "js/GlobalObject.h"
#include "js/Initialization.h"
#include "js/RealmOptions.h"
#include "js/RootingAPI.h"
#include "js/SourceText.h"
#include "js/Promise.h"
#include "js/Object.h"
#include "js/TracingAPI.h"
#include "js/Value.h"

#include "js/CallArgs.h"
#include "js/CharacterEncoding.h"
#include "js/PropertyAndElement.h"
#include "js/Conversions.h"
#include "js/Exception.h"
#include "js/ErrorReport.h"

// Phase 6 (Runtime Integration, see ROADMAP.md): the prototype pieces that
// used to live directly in this file (std::vector<std::unique_ptr<Microtask>>,
// the old std::vector<std::unique_ptr<JsTimer>>-backed timer registry) are
// gone, replaced by the equivalents below. <fstream>/<sstream>/<vector>/
// <memory> are no longer needed for that reason (see HISTORY.md's Phase 6
// entry) -- <string> is still needed for the top-level --version/--help
// command dispatch in main(), which is unrelated CLI plumbing rather than
// runtime data structures and was deliberately left alone.
#include "forge-core/platform/IoLoop.h"
#include "forge-core/File.h"
#include "forge-core/HashMap.h"
#include "forge-core/Path.h"
#include "forge-core/Queue.h"
#include "forge-core/Result.h"
#include "forge-core/String.h"
#include "forge-core/StringView.h"
#include "forge-core/memory/DefaultAllocator.h"
#include "forge-core/memory/MakeUnique.h"
#include "forge-core/memory/UniquePtr.h"

// cx points to SpiderMonkey's context object.
static const JSClass globalClass = {
    "global",
    JSCLASS_GLOBAL_FLAGS | JSCLASS_HAS_RESERVED_SLOTS(1),
    &JS::DefaultGlobalClassOps,
};

enum GlobalSlots { RuntimeSlot = JSCLASS_GLOBAL_SLOT_COUNT };
static bool EnqueueMicrotask(JSContext* cx, JS::HandleObject callback);

// Forward declarations: ForgeTimerFired (defined further down, right after
// JsTimerRegistry) needs to reach the current Runtime's timer registry,
// but Runtime itself is only fully defined later in this file (it embeds
// JsTimerRegistry by value, so it can't be forward-declared-only at that
// point). Same pattern already used for EnqueueMicrotask above.
class JsTimerRegistry;
static JsTimerRegistry& GetRuntimeTimers(JSContext* cx);

// Prints (and consumes) whatever exception is currently pending on `cx`,
// tagged with `context` for the "could not even retrieve it" fallback
// case. Shared by every callback boundary that calls into JS and cannot
// propagate a C++ exception on failure (there are no C++ exceptions in
// this codebase — see forge-core's Result<T>/AGENTS.md philosophy; this is
// the JS-engine-boundary equivalent: report, don't throw, don't silently
// drop it).
static void ReportPendingException(JSContext* cx, const char* context) {
  if (!JS_IsExceptionPending(cx)) {
    return;
  }

  JS::ExceptionStack exn(cx);

  if (JS::StealPendingExceptionStack(cx, &exn)) {
    JS::ErrorReportBuilder report(cx);

    if (report.init(cx, exn, JS::ErrorReportBuilder::NoSideEffects)) {
      JS::PrintError(stderr, report.report(), false);
      return;
    }
  }

  fprintf(stderr, "Forge: %s failed (exception pending, but could not be retrieved)\n",
          context);
}

struct Microtask {
  JS::Heap<JSObject*> callback;
  JS::PersistentRootedVector<JS::Value> arguments;

  Microtask(JSContext* cx) : arguments(cx) {}
};

// Phase 6: was std::vector<std::unique_ptr<Microtask>>. A JS microtask
// queue is genuinely FIFO (push on enqueue, always process the oldest
// first, see runJobs() below) -- forge::core::Queue<T> is Forge Core's
// purpose-built circular-buffer FIFO (see ROADMAP.md's Phase 2 entry),
// giving O(1) Push()/Pop() instead of std::vector::erase(begin())'s O(n)
// shift on every single microtask drained. forge::core::memory::UniquePtr
// (not std::unique_ptr) owns each Microtask so its allocation goes through
// forge::core::memory::Allocator like everything else in this codebase,
// not raw `new`.
forge::core::Queue<forge::core::memory::UniquePtr<Microtask>> microtasks;

class ForgeJobQueue : public JS::JobQueue {
 public:
  bool empty() const override { return microtasks.Empty(); }

  bool isDrainingStopped() const override { return false; }

  bool getHostDefinedData(JSContext* cx,
                          JS::MutableHandle<JSObject*> data) const override {
    data.set(nullptr);
    return true;
  }

  void runJobs(JSContext* cx) override {
    while (!microtasks.Empty()) {
      Microtask* task = microtasks.Front().Get();

      JS::RootedObject callback(cx, task->callback);
      JS::RootedValue thisValue(cx, JS::UndefinedValue());
      JS::RootedValue rval(cx);

      bool ok = JS::Call(cx, thisValue, callback, JS::HandleValueArray::empty(),
                         JS::MutableHandleValue(&rval));

      if (!ok) {
        ReportPendingException(cx, "microtask");
        return;
      }

      microtasks.Pop();
    }
  }

  bool enqueuePromiseJob(JSContext* cx, JS::HandleObject promise,
                         JS::HandleObject job, JS::HandleObject allocationSite,
                         JS::HandleObject hostDefinedData) override {
    return EnqueueMicrotask(cx, job);
  }

 protected:
  class SavedQueue;

  js::UniquePtr<SavedJobQueue> saveJobQueue(JSContext* cx) override;
};
js::UniquePtr<JS::JobQueue::SavedJobQueue> ForgeJobQueue::saveJobQueue(
    JSContext* cx) {
  return nullptr;
}

//==============================================================================
// Timers
//
// Bridges the JS-facing setTimeout/setInterval/clearTimeout API onto
// forge::core::platform::IoLoop's TimerId-based scheduling (see
// ROADMAP.md Phase 2 / HISTORY.md — this replaces the old TimerQueue's
// manual "scan every timer, compare to now()" polling, driven by
// EventLoop's busy-wait, with the IOCP-backed loop actually sleeping until
// the next timer or I/O event).
//
// Two id spaces are kept deliberately separate: `jsId` is the small
// integer setTimeout()/setInterval() return to script (unchanged
// behaviour); `nativeId` is IoLoop's own forge::core::platform::TimerId,
// used only internally to cancel the right native timer.
//==============================================================================

static void ForgeTimerFired(void* userData) noexcept;

struct JsTimer {
  JSContext* cx;
  JS::Heap<JSObject*> callback;
  JS::PersistentRootedVector<JS::Value> arguments;

  int jsId{0};
  forge::core::platform::TimerId nativeId{0};
  bool repeat{false};

  explicit JsTimer(JSContext* cx) : cx(cx), arguments(cx) {}
};

class JsTimerRegistry {
 public:
  explicit JsTimerRegistry(forge::core::platform::IoLoop& loop) : loop_(loop) {}

  // Takes ownership of `timer` (already populated with callback/arguments/
  // repeat/delayMs by the caller) and schedules it. Returns the JS-visible
  // id, or -1 if native scheduling (or storing the wrapper) failed —
  // `timer` is destroyed in that case, nothing is left registered.
  int Add(forge::core::memory::UniquePtr<JsTimer> timer, uint64_t delayMs) {
    timer->jsId = nextJsId_++;

    JsTimer* raw = timer.Get();

    forge::core::Result<forge::core::platform::TimerId> scheduled =
        loop_.ScheduleTimer(delayMs, raw->repeat, &ForgeTimerFired, raw);

    if (scheduled.HasError()) {
      return -1;
    }

    raw->nativeId = scheduled.Value();

    int jsId = raw->jsId;

    forge::core::Result<bool> inserted = timers_.Insert(jsId, std::move(timer));

    if (inserted.HasError()) {
      // The native timer is already scheduled and pointing at `raw` --
      // Insert()'s failure path never moves from its argument (see
      // HashMap::Insert(K, V&&)), so `timer` (the local UniquePtr) still
      // owns *raw and will free it when this function returns. Cancel the
      // native timer first, or ForgeTimerFired would eventually fire into
      // memory this registry never actually took ownership of -- same
      // "don't leave a native resource pointing at something about to be
      // freed" discipline as ThreadPool::Initialize's own rollback fix
      // (see HISTORY.md's Phase 4 entry).
      loop_.CancelTimer(raw->nativeId);
      return -1;
    }

    return jsId;
  }

  void CancelByJsId(int jsId) {
    forge::core::memory::UniquePtr<JsTimer>* found = timers_.Find(jsId);

    if (found == nullptr) {
      return;
    }

    loop_.CancelTimer((*found)->nativeId);
    timers_.Erase(jsId);
  }

  // Called once a one-shot timer's callback has finished running, to
  // release its JsTimer. The native side has already removed the
  // corresponding entry itself (TimerScheduler::PopDue erases one-shots on
  // firing) — this only releases the JS-side wrapper.
  void RemoveFired(int jsId) {
    timers_.Erase(jsId);
  }

  // Traces every live timer's JS callback so the GC does not collect it
  // out from under a still-pending timer. JS::Heap<T> (unlike
  // JS::PersistentRootedVector, which self-registers and needs no manual
  // tracing — that's why `arguments` above needed no attention here) is
  // *not* traced automatically; something owning it must do so explicitly.
  void TraceRoots(JSTracer* trc) {
    for (auto entry : timers_) {
      JS::TraceEdge(trc, &entry.Value()->callback, "forge-timer-callback");
    }
  }

  // Cancels and releases every still-pending timer. Must be called (via
  // Runtime::Shutdown()) while `cx` is still alive and before
  // JS_DestroyContext(cx) — each JsTimer holds a
  // JS::PersistentRootedVector<JS::Value>, which needs to unregister
  // itself from the still-live context when destroyed. Normally run()
  // only stops once every timer is already gone, but it can now also stop
  // early on a genuine I/O error (see Runtime::run()), which is exactly
  // the case this exists for.
  void Clear() {
    for (auto entry : timers_) {
      loop_.CancelTimer(entry.Value()->nativeId);
    }
    timers_.Clear();
  }

 private:
  forge::core::platform::IoLoop& loop_;
  // Phase 6: was std::vector<std::unique_ptr<JsTimer>>, searched linearly
  // by both CancelByJsId and RemoveFired. Every lookup here is genuinely
  // "find the one timer with this jsId", which is exactly what a HashMap
  // keyed on jsId does in O(1) instead of an O(n) scan -- forge::core's
  // HashMap<K, V> (see ROADMAP.md's Phase 2 entry) already exists for
  // this.
  forge::core::HashMap<int, forge::core::memory::UniquePtr<JsTimer>> timers_;
  int nextJsId_{1};
};

static void ForgeTimerFired(void* userData) noexcept {
  auto* timer = static_cast<JsTimer*>(userData);
  JSContext* cx = timer->cx;
  bool repeat = timer->repeat;
  int jsId = timer->jsId;

  JS::RootedObject callback(cx, timer->callback);
  JS::RootedValue thisValue(cx, JS::UndefinedValue());
  JS::RootedValue rval(cx);

  bool ok = JS::Call(cx, thisValue, callback, timer->arguments,
                     JS::MutableHandleValue(&rval));

  if (!ok) {
    ReportPendingException(cx, "timer callback");
  }

  // Must be last: on a one-shot timer this destroys *timer (via the
  // registry's owning HashMap<int, UniquePtr<JsTimer>>, keyed on jsId), so
  // nothing above may touch `timer` again after this — hence copying
  // jsId/repeat out at the top instead of reading timer->... below.
  if (!repeat) {
    GetRuntimeTimers(cx).RemoveFired(jsId);
  }
}

//==============================================================================
// Runtime / GC root tracing
//==============================================================================

static void TraceForgeRoots(JSTracer* trc, void* data) {
  auto* timers = static_cast<JsTimerRegistry*>(data);

  // Queue<T> is a FIFO, not a general sequence container -- it has no
  // begin()/end(), only Front()/Back() plus the front-relative
  // operator[](offset) added this phase specifically so every still-
  // pending microtask can be walked and traced here, not just the front
  // one (see Queue.h's own doc comment on operator[]).
  for (forge::core::Size i = 0; i < microtasks.Size(); ++i) {
    JS::TraceEdge(trc, &microtasks[i]->callback, "forge-microtask-callback");
  }

  timers->TraceRoots(trc);
}

class Runtime {
 public:
  explicit Runtime(JSContext* cx)
      : cx_(cx), loop_(), timers_(loop_), jobQueue_() {
    JS::SetJobQueue(cx, &jobQueue_);
  }

  // Must be called exactly once, after construction and before any timer
  // is scheduled or run() is called.
  //
  // Phase 6: was `[[nodiscard]] bool`, which discarded loop_.Initialize()'s
  // actual Error (only its success/failure was visible to the caller in
  // main(), via a hardcoded "failed to initialize the event loop" message
  // with no detail on why). Routed through Result<void> so the native
  // error code actually reaches the one fprintf that reports it, per this
  // project's "route error paths through Result<T> where practical"
  // policy for anything that isn't a bool-returning SpiderMonkey API.
  [[nodiscard]] forge::core::Result<void> Initialize() {
    if (forge::core::Result<void> looped = loop_.Initialize(); looped.HasError()) {
      return looped;
    }

    JS_AddExtraGCRootsTracer(cx_, TraceForgeRoots, &timers_);
    return {};
  }

  // Drains microtasks, then blocks on the event loop for the next timer
  // or I/O completion, repeating until there is genuinely nothing left
  // pending — replaces the old EventLoop's busy-poll `while (...) { ...;
  // sleep_for(1ms); }` with the loop actually sleeping until something is
  // due (see ROADMAP.md Phase 2).
  void run() {
    for (;;) {
      jobQueue_.runJobs(cx_);

      if (jobQueue_.empty() && loop_.Empty()) {
        break;
      }

      forge::core::Result<void> result = loop_.RunOnce();

      if (result.HasError()) {
        fprintf(stderr, "Forge: event loop I/O error (native code %d)\n",
                result.Error().NativeCode());
        break;
      }
    }
  }

  JsTimerRegistry& timers() { return timers_; }

  // Releases every still-pending timer. Call this once, after run() has
  // returned (however it returned), and before JS_DestroyContext(cx) — see
  // JsTimerRegistry::Clear() for exactly why this ordering matters.
  void Shutdown() { timers_.Clear(); }

 private:
  JSContext* cx_;
  forge::core::platform::IoLoop loop_;
  JsTimerRegistry timers_;
  ForgeJobQueue jobQueue_;
};

Runtime* GetRuntime(JSContext* cx)
{
    JS::RootedObject global(
        cx,
        JS::CurrentGlobalOrNull(cx)
    );

    if (!global) {
        return nullptr;
    }

    JS::Value value =
        JS::GetReservedSlot(global, RuntimeSlot);

    return static_cast<Runtime*>(value.toPrivate());
}

// Thin helper so ForgeTimerFired (a free function, declared above Runtime)
// can reach the registry without needing Runtime's full definition visible
// at its point of use.
static JsTimerRegistry& GetRuntimeTimers(JSContext* cx) {
  return GetRuntime(cx)->timers();
}

static bool Print(JSContext* cx, unsigned argc, JS::Value* vp) {
  JS::CallArgs args = JS::CallArgsFromVp(argc, vp);

  for (unsigned i = 0; i < args.length(); i++) {
    JS::RootedString str(cx, JS::ToString(cx, args[i]));

    if (!str) {
      return false;
    }

    JS::UniqueChars bytes = JS_EncodeStringToUTF8(cx, str);

    if (!bytes) {
      return false;
    }

    printf("%s", bytes.get());

    if (i + 1 < args.length()) {
      printf(" ");
    }
  }

  printf("\n");

  args.rval().setUndefined();
  return true;
}

static bool SetTimeout(JSContext* cx, unsigned argc, JS::Value* vp) {
  JS::CallArgs args = JS::CallArgsFromVp(argc, vp);

  if (argc < 2) {
    return false;
  }
  if (!args[0].isObject()) {
    return false;
  }
  double delay = 0;

  if (!JS::ToNumber(cx, args[1], &delay)) {
    return false;
  }
  if (delay < 0 || !(delay == delay)) {  // NaN-safe: NaN < 0 is false, so clamp NaN explicitly too.
    delay = 0;
  }

  forge::core::Result<forge::core::memory::UniquePtr<JsTimer>> timer =
      forge::core::memory::MakeUnique<JsTimer>(cx);

  if (timer.HasError()) {
    JS_ReportOutOfMemory(cx);
    return false;
  }

  JS::RootedObject callback(cx, &args[0].toObject());
  timer.Value()->callback = callback;
  timer.Value()->repeat = false;

  for (unsigned i = 2; i < args.length(); i++) {
    if (!timer.Value()->arguments.append(args[i])) {
      return false;
    }
  }

  int id = GetRuntime(cx)->timers().Add(std::move(timer.Value()), (uint64_t)delay);

  if (id < 0) {
    JS_ReportErrorASCII(cx, "setTimeout: failed to schedule timer");
    return false;
  }

  args.rval().setInt32(id);

  return true;
}
static bool ClearTimeout(JSContext* cx, unsigned argc, JS::Value* vp) {
  JS::CallArgs args = JS::CallArgsFromVp(argc, vp);

  if (argc < 1) {
    return false;
  }

  int32_t id;

  if (!JS::ToInt32(cx, args[0], &id)) {
    return false;
  }

  GetRuntime(cx)->timers().CancelByJsId(id);

  args.rval().setUndefined();

  return true;
}
static bool SetInterval(JSContext* cx, unsigned argc, JS::Value* vp) {
  JS::CallArgs args = JS::CallArgsFromVp(argc, vp);

  if (argc < 2) {
    return false;
  }
  if (!args[0].isObject()) {
    return false;
  }
  double delay = 0;

  if (!JS::ToNumber(cx, args[1], &delay)) {
    return false;
  }
  if (delay < 0 || !(delay == delay)) {
    delay = 0;
  }

  forge::core::Result<forge::core::memory::UniquePtr<JsTimer>> timer =
      forge::core::memory::MakeUnique<JsTimer>(cx);

  if (timer.HasError()) {
    JS_ReportOutOfMemory(cx);
    return false;
  }

  JS::RootedObject callback(cx, &args[0].toObject());
  timer.Value()->callback = callback;
  timer.Value()->repeat = true;

  for (unsigned i = 2; i < args.length(); i++) {
    if (!timer.Value()->arguments.append(args[i])) {
      return false;
    }
  }

  int id = GetRuntime(cx)->timers().Add(std::move(timer.Value()), (uint64_t)delay);

  if (id < 0) {
    JS_ReportErrorASCII(cx, "setInterval: failed to schedule timer");
    return false;
  }

  args.rval().setInt32(id);

  return true;
}
static bool EnqueueMicrotask(JSContext* cx, JS::HandleObject callback) {
  forge::core::Result<forge::core::memory::UniquePtr<Microtask>> task =
      forge::core::memory::MakeUnique<Microtask>(cx);

  if (task.HasError()) {
    JS_ReportOutOfMemory(cx);
    return false;
  }

  task.Value()->callback = callback;

  if (forge::core::Result<void> pushed = microtasks.Push(std::move(task.Value()));
      pushed.HasError()) {
    JS_ReportOutOfMemory(cx);
    return false;
  }

  return true;
}
static bool QueueMicrotask(JSContext* cx, unsigned argc, JS::Value* vp) {
  JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
  if (argc < 1) {
    return false;
  }

  if (!args[0].isObject()) {
    return false;
  }

  // Delegates to EnqueueMicrotask (Phase 6 cleanup: this used to duplicate
  // EnqueueMicrotask's allocate-and-push logic inline instead of calling
  // it, the only difference being this JSNative also sets the return
  // value) rather than maintaining two copies of the same allocation-
  // failure handling.
  JS::RootedObject callback(cx, &args[0].toObject());

  if (!EnqueueMicrotask(cx, callback)) {
    return false;
  }

  args.rval().setUndefined();

  return true;
}

int main(int argc, char* argv[]) {
      if (argc == 1) {
        printf("Forge JavaScript Runtime\n");
        printf("Usage:\n");
        printf("  forge <file.js>\n");
        printf("  forge --version\n");
        printf("  forge --help\n");
        return 0;
    }

    std::string command = argv[1];

    if (command == "--version") {
        printf("Forge v0.1.0\n");
        return 0;
    }

    if (command == "--help") {
        printf("Forge JavaScript Runtime\n\n");
        printf("Commands:\n");
        printf("  forge <file.js>    Run a JavaScript file\n");
        printf("  forge --version    Show runtime version\n");
        printf("  forge --help       Show this help\n");
        return 0;
    }

  if (!JS_Init()) { // this is where engine start
    return 1;
  }

  JSContext* cx = JS_NewContext(8L * 1024 * 1024); // new context

  if (!cx) {
    return 1;
  }

  if (!JS::InitSelfHostedCode(cx)) {
    return 1;
  } // SpiderMonkey's own JavaScript library.
  Runtime runtime(cx);

  if (forge::core::Result<void> initialized = runtime.Initialize();
      initialized.HasError()) {
    fprintf(stderr,
            "Forge: failed to initialize the event loop (native code %d)\n",
            initialized.Error().NativeCode());
    return 1;
  }

  // Everything from here down that touches `global` (including the
  // JSAutoRealm block below) lives in this outer scope, so `global` — a
  // JS::RootedObject, which unregisters itself from `cx`'s rooting list
  // when destroyed — is destroyed *before* JS_DestroyContext(cx) below,
  // not after. Originally `global` was declared at the same scope as
  // `cx`/`runtime`, so its destructor only ran when main() itself
  // returned, which is after JS_DestroyContext(cx) already ran a few
  // lines down — a real use-after-free (JS::Rooted<T>'s destructor
  // dereferences a now-dangling context pointer to unlink itself). This
  // was a pre-existing bug, not something the event-loop rewrite
  // introduced — it just never had a chance to fire before, because no
  // script had ever run to natural completion through a real
  // `mach`-built forge.exe until the Phase 0 benchmark scripts (see
  // HISTORY.md): hello.js's uncancelled setInterval never lets main()
  // reach this point on its own, so Ctrl+C always killed the process
  // first.
  {
    JS::RealmOptions options;
    options.creationOptions().setSharedMemoryAndAtomicsEnabled(true); // This Realm is allowed to use SharedArrayBuffer Atomics

    JS::RootedObject global(
        cx, JS_NewGlobalObject(cx, &globalClass, nullptr, JS::FireOnNewGlobalHook,
                               options)); // JavaScript universe is born
    JS::SetReservedSlot(global, RuntimeSlot, JS::PrivateValue(&runtime));
    if (!global) {
      return 1;
    }

    {
      JSAutoRealm ar(cx, global); // JSAutoRealm enters that JavaScript world.

      if (!JS::InitRealmStandardClasses(cx)) {
        return 1;
      } // Initialize standard JS class constructors, prototypes, and any top-level functions and constants associated with the standard classes
      JS_DefineFunction(cx, global, "clearTimeout", ClearTimeout, 1, 0);
      if (!JS_DefineFunction(cx, global, "setInterval", SetInterval, 2, 0)) {
        return 1;
      }
      JS_DefineFunction(cx, global, "queueMicrotask", QueueMicrotask, 1, 0);
      if (!JS_DefineFunction(cx, global, "print", Print, 0, 0)) {
        return 1;
      }
      if (!JS_DefineFunction(cx, global, "setTimeout", SetTimeout, 2, 0)) {
        return 1;
      }

      // Note: argc < 2 is unreachable here — the argc == 1 case already
      // returned at the top of main(), so argc >= 2 is guaranteed by this
      // point. The old dead check has been removed.

      // Phase 6: was std::ifstream + std::stringstream. forge-core already
      // has a purpose-built, verified pair for exactly this job --
      // Path (pure text manipulation, Phase 3) and File::ReadAllText
      // (Win32 I/O, Phase 3, real-build-confirmed) -- and using them here
      // means a bad script path reports through the same Result<T>/Error
      // machinery as everything else in this codebase, instead of a bare
      // "Cannot open %s" with no actual reason.
      forge::core::Result<forge::core::Path> scriptPath =
          forge::core::Path::Create(forge::core::StringView(argv[1]));

      if (scriptPath.HasError()) {
        fprintf(stderr, "Forge: invalid script path %s\n", argv[1]);
        return 1;
      }

      forge::core::Result<forge::core::String> source =
          forge::core::File::ReadAllText(scriptPath.Value());

      if (source.HasError()) {
        fprintf(stderr, "Forge: cannot read %s (error code %d)\n", argv[1],
                static_cast<int>(source.Error().Code()));
        return 1;
      }

      JS::SourceText<mozilla::Utf8Unit> src;

      // `source` (declared in this scope, not a temporary) stays alive for
      // the rest of this block, same lifetime requirement the old
      // std::string local satisfied -- JS::SourceOwnership::Borrowed means
      // SpiderMonkey does not copy this buffer, so it must outlive every
      // use of `src` below, including JS::Evaluate().
      if (!src.init(cx, source.Value().Data(), source.Value().Size(),
                    JS::SourceOwnership::Borrowed)) {
        return 1;
      }

      JS::RootedValue rval(cx);

      JS::CompileOptions opts(cx);
      opts.setFileAndLine(argv[1], 1);

      if (!JS::Evaluate(cx, opts, src, &rval)) {
        ReportPendingException(cx, "script evaluation");
        // The script may have already called setTimeout/setInterval before
        // failing — release those before cx goes away, same reasoning as
        // the runtime.Shutdown() call after the event loop below.
        runtime.Shutdown();
        return 1;
      }

      if (rval.isNumber()) {
        printf("Result = %f\n", rval.toNumber());
      }
      runtime.run();
    }
  } // end of the `global`-owning scope — global is destroyed here, cx is still alive.

  runtime.Shutdown();
  JS_DestroyContext(cx);
  JS_ShutDown();

  return 0;
}
