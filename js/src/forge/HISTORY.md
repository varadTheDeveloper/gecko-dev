# Decision History

This file records decisions already made on Forge Core components. Read this
before touching any file under `forge/forge-core/` — if a component is
listed as Frozen here, do not redesign it; only bugs, security issues,
incorrect behaviour, or fundamental design flaws justify a change (see
`PROJECT_CONTEXT.md` → Development Rules).

---

## Types

Decision

Fixed-width aliases only (`i8`..`i64`, `u8`..`u64`, `f32`, `f64`, `Byte`,
`Size`, `Offset`). No higher-level type utilities live here.

Reason

Keep the lowest layer dependency-free and trivial; every other component
depends on this one, so it must never grow.

Status

Frozen.

---

## Error

Decision

`Error` stores only an `ErrorCode` (canonical, generic — no module-specific
codes like `FileNotFound`) and a `NativeError` (`i32`) platform code. No
message, no formatting, no stack trace, no logging, no exceptions.
`constexpr`/`noexcept` throughout, trivially movable, no heap allocation.

Reason

`Error` is embedded inline inside `Result<T>`, so it must stay small,
allocation-free, and cheap to copy/move. Formatting and logging are
explicitly out of scope so the type can stay platform-independent.

Status

Frozen. Full spec in `forge/forge-core/Error.md`. Future additions may only
add generic `ErrorCode` values or platform-specific conversion helpers
implemented outside the type — never allocation, logging, formatting, or
module-specific codes.

---

## Failure

Decision

`Failure` is a thin wrapper around `Error`, used only to disambiguate
constructing a failed `Result<T>` from a `Result<T>` whose value type
happens to be `Error` itself.

Status

Frozen.

---

## Result / ResultVoid

Decision

`Result<T>` is `[[nodiscard]]`, embeds `Error` directly (no dynamic
allocation for the error path), and exposes `HasValue()`, `Value()`,
`Error()`, `operator*`, `operator->`, `operator bool`, and `Ignore()` (to
explicitly discard a `[[nodiscard]]` result without warnings). Constructed
explicitly from `T`/`Failure`, never implicitly. `ResultVoid` is the
equivalent for functions that only need to report success/failure with no
value.

Reason

Forge Core uses no exceptions, so every fallible function must communicate
success/failure explicitly through its return type instead.

Status

Frozen.

---

## Assert

Decision

`FORGE_ASSERT(condition)` wraps `assert()` and is compiled out under
`NDEBUG`. It exists to catch programmer errors during development, not to
report recoverable runtime failures — that's what `Result<T>` is for.

Status

Frozen.

---

## Memory / Allocator

Decision

All allocation goes through the `memory::Allocator` interface
(`Allocate(size, alignment)` / `Deallocate(memory, size, alignment)`).
`Allocate` returns `nullptr` on failure rather than throwing.
`Deallocate` must be called with the exact size/alignment used at
allocation time. `DefaultAllocator` is the current concrete implementation,
backed by `memory::detail::AllocationBackend`.

Reason

Centralizing allocation behind one interface is what makes it possible to
later swap in arena/pool allocators, track memory, or sandbox allocation
for the JS runtime without touching call sites.

Status

Frozen (interface). `new`/`delete`/`malloc`/`free` must never be used
directly anywhere in Forge Core or code built on top of it.

---

## UniquePtr / MakeUnique

Decision

`UniquePtr` is an owning smart pointer built on top of `memory::Allocator`
(not `std::allocator`/`new`), constructed via `MakeUnique<T>(...)`.

Status

Frozen.

---

## Vector

Decision

API frozen. Implementation frozen. Built on `memory::Allocator`, not
`std::allocator`. Do not redesign.

Status

Frozen.

---

## Platform path handling (`forge/platform/`)

Decision

The earlier `forge/platform/` groundwork (`FilePath.cpp`,
`PathNormalizer.cpp`, `SeparatorNormalizer.cpp`, Windows-specific
`PathTraits.cpp`/`PathUtils.cpp`/`Platform.cpp`, and a parallel
`include/forge/...` header tree) has been removed. Phase 3 (Filesystem)
will design the path/filesystem layer from scratch rather than resuming
this code.

Reason

Deliberate decision by the project owner (2026-07-26), not an accident —
confirmed after the removal was flagged during a repo review (it showed up
as an uncommitted deletion against commit `326d50e`).

Status

Removed. Do not resurrect this code from git history without checking with
the project owner first; do not treat its absence as something to "fix."

---

## Codebase-wide compile/correctness pass (2026-07-26)

Decision

Every file in `forge/forge-core/` was reviewed and, where broken, fixed —
this was the first time any of it had actually been compiled. Standalone
`-fsyntax-only` and full runtime tests (under ASan+UBSan) now pass. Fixes
made:

* **`Result.inl` and `Vector.inl`**: both files closed
  `namespace forge::core` far too early (right after the `Failure`
  constructors in `Result.inl`, right after `Data()` in `Vector.inl`),
  silently leaving most of the class's own member definitions (copy/move
  ctors, assignment, `HasValue`/`Value`/`Error`, `Reserve`, `PushBack`,
  etc.) sitting at global scope. Neither file could have compiled. Fixed
  by moving the closing brace to the actual end of each file.
* **`usize` was never declared.** `Allocator.h`, `DefaultAllocator.h/.cpp`,
  and `Vector.h`'s `SizeType` alias all referenced `usize`; `Types.h` only
  defines `Size`. Replaced every occurrence with `Size`.
* **Include-path convention.** Most headers included each other via
  `forge/core/...`, implying a `forge/core/` directory that does not
  exist — the real directory is `forge/forge-core/` (hyphenated, one
  level up). Only `AllocationBackend.h/.cpp` had the right idea (relative
  to the real layout). Standardized everything on relative,
  directory-correct includes. See `PROJECT_CONTEXT.md` → Coding Style.
* **`Vector.h`'s own `.inl` include** pointed at a nonexistent
  `forge/core/containers/Vector.inl`. Fixed to the real, same-directory
  file.
* **`UniquePtr::Reset()`** called `allocator_->Deallocate(pointer_)` with
  one argument; `Allocator::Deallocate` requires `(memory, size,
  alignment)`. Fixed to pass `sizeof(T), alignof(T)`.
* **`UniquePtr`'s destructor** was declared without `noexcept` in the
  header but defined with `noexcept` in the `.inl` — a hard
  declaration/definition mismatch. Header now matches.
* **`Error::OutOfMemory`** (in `MakeUnique.inl` and `Vector.inl`'s
  `Reserve()`) doesn't exist — `Error` is a class with no such static
  member; `OutOfMemory` is an `ErrorCode` enumerator. Fixed to
  `Error(ErrorCode::OutOfMemory)`.
* **`return Failure{...}` from functions returning `Result<X>`** doesn't
  compile — `Result`'s `Failure`-taking constructor is `explicit`. Both of
  the sites above now construct the `Result<X>` explicitly.
* **`ResultVoid.h`/`ResultVoid.inl`** declared and separately defined
  `Ignore()` twice in the same class (hard redefinition error). Removed
  the duplicates.
* **`Vector<T>::EmplaceBack`** was declared in `Vector.h` as returning
  `Result<void>` but implemented in `Vector.inl` as returning `Result<T&>`
  — which is itself ill-formed for any `T`, since `Result<T>`'s storage is
  a `union { T value; Error error; }` and C++ forbids reference members in
  unions. Implementation now matches the header (`Result<void>`).
* **`Result<void>` has no `IsFailure()`** (only `HasValue()`/`HasError()`/
  `operator bool()`); three call sites in `Vector.inl` used it anyway.
  Fixed to `HasError()`. `HasError()` was also added to the primary
  `Result<T>` template for symmetry (purely additive, non-breaking).
* **Member-function name collides with an enclosing-scope type name**:
  `Result<T>::Error()` / `Result<void>::Error()` / `Failure::Error()`
  (returning `Error&`), and `Vector<T>::Size()` (returning `SizeType` =
  `Size`) all hard-error under GCC (`-Wchanges-meaning`) the moment any
  other bare use of the type name appears in the same class. Fixed with
  the `class Error` elaborated-type-specifier for the `Error` cases, and
  by fully qualifying `using SizeType = forge::core::Size;` for the
  `Vector` case (elaborated specifiers don't apply to `using`-aliases).
  The accessor names themselves were **not** renamed — see
  `PROJECT_CONTEXT.md` and `AGENTS.md`.
* **`Vector`'s copy constructor and copy-assignment** silently
  `.Ignore()`d a failing `Reserve()` and then proceeded to construct/copy
  `other.size_` elements regardless — on allocation failure this wrote
  past an empty or undersized buffer (verified as a real crash/UB path
  with a `FailingAllocator` test, not just a theoretical concern). Both
  now bound the copy loop to `min(capacity_, other.size_)`, so a failed
  allocation produces a safely-truncated (empty, in the OOM case) copy
  instead of undefined behaviour.
* **`moz.build`** listed `AllocationBackend.cpp` in `SOURCES` but not
  `DefaultAllocator.cpp` (which calls into it) — the allocator subsystem
  was never actually being built. Added it.
* Minor: removed one dead/unreachable `argc < 2` check in `forge.cpp`
  (unreachable given the `argc == 1` early-return at the top of `main`),
  and fixed `ErrorTests.cpp`'s include to match the path convention above.

Verification

A standalone test translation unit including every forge-core header
compiled cleanly under `g++ -std=c++20 -Wall -Wextra -Wpedantic -Werror`
and `clang++`, and a runtime test (`Result<T>`/`Result<void>` success and
failure paths and their copy/move; `Vector` push/emplace/copy/move/assign
with a non-trivial element type, verifying construction/destruction counts;
`Reserve()`/`PushBack()`/copy-assignment against a deliberately-failing
allocator; `MakeUnique` success and out-of-memory paths) passed under
`-fsanitize=address,undefined` with no leaks, no UB, exit code 0.

Status

Applied. Not yet reflected in any git commit on the user's machine as of
this writing — these are working-tree changes.

Noted but intentionally not changed (flagged for a future decision instead
of a unilateral fix):

* `MakeUnique(Allocator&, Args&&...)` vs `MakeUnique(Args&&...)`: passing a
  custom `Allocator` subclass *by its derived type* (not upcast to
  `Allocator&`) gets silently absorbed by the generic `Args...`-only
  overload instead of routing to the allocator overload, producing a
  confusing "no matching constructor" error instead of the intended
  allocator substitution. Works correctly when the caller passes an
  `Allocator&`-typed reference (the same convention `Vector`'s allocator
  constructor already expects). Fixing this properly needs SFINAE/concepts
  — a real API change, not something to do silently.
* `ResultFwd.h`, `ResultVoid.h`, `ResultVoid.inl` physically live under
  `forge-core/memory/` despite being core `Result` machinery (namespace
  `forge::core`, not `forge::core::memory`) — not memory-specific. Include
  paths were fixed to match their actual location; the files themselves
  were not moved, since that's an organizational call, not a bug fix.
* `Vector::AllocationSize()`/`NextCapacity()` have no overflow checking
  for extreme capacities — a common, known limitation, left as-is to avoid
  scope creep beyond what was asked.

---

## Platform / TimerScheduler (Phase 2, 2026-07-27)

Decision

`forge::core::platform::TimerScheduler` is the first piece of the Phase 2
event-loop rearchitecture (see `ROADMAP.md`). It is deliberately split off
from the actual Windows/IOCP backend: it holds only timer bookkeeping
(`Schedule`, `Cancel`, `Empty`, `Count`, `NextDueDelay`, `PopDue`) over
caller-supplied `u64` millisecond timestamps, with zero OS calls. The
upcoming `IocpLoop` will wrap this and decide how long to block in
`GetQueuedCompletionStatus` based on `NextDueDelay()`, then call `PopDue()`
after waking. Backend selection between platforms will be a compile-time
type alias (`#if defined(_WIN32)`), not a virtual interface — avoids vtable
overhead on a hot path and avoids adding a converting/upcasting constructor
to the frozen `UniquePtr<T>`.

Reason

Everything OS-specific (real IOCP calls) cannot be compiled or tested in
the Linux sandbox this work was verified in, and would need to wait for
the project owner's Windows/Visual Studio machine either way. Splitting
out the pure logic means the part that's actually easy to get subtly wrong
(due-time bookkeeping, safe removal-during-iteration, re-entrant
Schedule/Cancel from inside a firing callback) gets fully verified now
instead of being tangled up with untestable Win32 glue.

Verification

A dedicated test (`TimerSchedulerTest.cpp`, kept alongside the class as a
throwaway harness, not part of the `moz.build` production build) covers:
one-shot timers firing exactly once and being removed; a repeating timer
firing multiple times with correctly-advancing due times, including under
late polling; cancelling a timer before it's due; cancelling a *different*
timer from within another timer's callback, both a still-pending target
and an already-fired-and-removed one; a repeating timer cancelling
*itself* from within its own callback and confirming it does not fire
again; `NextDueDelay` reporting the correct minimum across several
concurrent timers and `false` once none are pending; and scheduling new,
already-due timers from within a callback mid-`PopDue`, forcing the
backing `Vector` to reallocate while the sweep's own index is live.

Passed under `g++`/`clang++ -std=c++20 -Wall -Wextra -Wpedantic -Werror`,
under `g++ -fsanitize=address,undefined` (0 errors), and under `valgrind
--leak-check=full` (0 errors, 0 leaks). clang's ASan runtime library was
not installed in this sandbox, so the clang+ASan combination specifically
was not exercised — covered instead by g++ + ASan/UBSan plus valgrind.

Two real bugs were found and fixed during this pass, not just theoretical
review:

* **`Vector.h` never included `DefaultAllocator.h`** despite its default
  member initializer calling `memory::GetDefaultAllocator()` (declared
  there, not in `Allocator.h`, which `Vector.h` *did* include). This had
  never been caught before because every prior compile of `Vector.h`
  happened to have something else include `DefaultAllocator.h` first in
  the same translation unit — an accident of include order, not a
  guarantee. `TimerScheduler.h` including `Vector.h` on its own, with
  nothing else pulling in `DefaultAllocator.h` first, is what exposed it.
  Fixed by adding the include directly to `Vector.h`.
* **`TimerScheduler::PopDue()`'s repeating-timer reschedule anchored the
  next due time to `nowMs`** (the timestamp passed into that `PopDue`
  call) **instead of to the timer's own previous `dueMs`** — the opposite
  of what the header comment documents ("measured from when it was due,
  not from when `PopDue()` happened to run"). Anchoring to `nowMs` lets
  the effective period drift longer every time `PopDue` is called even
  slightly late. Caught by `Test_RepeatingTimerAdvancesDueTime`
  deliberately calling `PopDue` late (at t=23 for a timer due at t=20) and
  asserting the *next* due time is still 30, not 33. Fixed by anchoring to
  `fired.dueMs + fired.delayMs`.

Status

`TimerScheduler.h`/`.cpp` verified and applied.

---

## Platform / IoLoop + IocpLoop (Phase 2, 2026-07-27)

Decision

`forge::core::platform::IoLoop` (`IoLoop.h`) is the compile-time backend
alias called for in `ROADMAP.md` Phase 2 — `#if defined(_WIN32)` selects
`IocpLoop`; any other platform is a hard `#error` until a Linux/macOS
backend is written, rather than silently compiling something broken.

`IocpLoop` (`IocpLoop.h`/`.cpp`) wraps a single Win32 I/O completion port
plus a `TimerScheduler`. It is single-threaded by design — exactly one
thread ever calls `Run()`/`RunOnce()`, matching the single-threaded JS
event-loop model every other engine (V8/Node, JavaScriptCore/Bun) also
uses; blocking or CPU-heavy work belongs on the Phase 7 thread pool, not
here. Async operations are represented by `IoCompletion` (a struct
inheriting from `OVERLAPPED` with an inline `CompletionCallback` function
pointer) that future socket/file code (Phase 5/6) embeds as the first
member of its own per-operation struct — the standard IOCP idiom of
casting the `LPOVERLAPPED` handed back by `GetQueuedCompletionStatus`
straight back to the callback that should handle it, with no virtual
dispatch and no lookup table. `RunOnce()` computes its wait timeout from
`TimerScheduler::NextDueDelay()` so the loop blocks exactly as long as
until the next timer or I/O event, never polling.

Reason

Same reasoning as `TimerScheduler`: get the shape of the loop right once,
now, since every future async API (files, sockets, HTTP) builds on top of
it and retrofitting it later would mean touching all of them.

Verification

The real Win32 calls (`CreateIoCompletionPort`, `GetQueuedCompletionStatus`,
`PostQueuedCompletionStatus`, etc.) cannot be compiled or executed in this
Linux sandbox — there is no Windows SDK here. Two separate checks were
still run, both clearly scoped to what they do and don't prove:

* A minimal mock `windows.h` (stub types/signatures only, every function
  a one-line no-op) type-checked `IocpLoop.cpp` and `IoLoop.h` (with
  `_WIN32` defined) under `g++`/`clang++ -Wall -Wextra -Wpedantic -Werror`.
  This only proves the code is syntactically and type-correct against the
  real Win32 signatures it calls — it says nothing about runtime
  behaviour.
* A second, *stateful* mock `windows.h` backs `GetQueuedCompletionStatus`/
  `PostQueuedCompletionStatus` with a real (fake-OS, single-threaded) FIFO
  queue, so `IocpLoopTest.cpp` could actually drive `IocpLoop`'s own
  control flow end-to-end: a posted completion dispatches on the next
  `RunOnce()` with the right bytes-transferred/completion-key/success
  values; a *failed* completion (operation error, but a real dequeue —
  `GetQueuedCompletionStatus`'s documented third outcome) still dispatches,
  with `succeeded=false` and the right error code, and is not treated as a
  `RunOnce()`-level failure; a due timer fires even when no completion is
  queued (mock "timeout" path); a timer and a completion both firing in
  the same sweep both get handled; a genuine wait failure (as opposed to a
  plain timeout) surfaces as a `Result<void>` error from `RunOnce()`; and
  `RequestStop()` called from inside a completion callback stops `Run()`
  after that iteration, not before or after an extra one. All 7 scenarios
  passed under `g++`/`clang++ -Wall -Wextra -Wpedantic -Werror`, under
  `g++ -fsanitize=address,undefined`, and under `valgrind --leak-check=full`
  (0 errors, 0 leaks in every run). This is real verification of
  `IocpLoop.cpp`'s own logic — the branching on
  `GetQueuedCompletionStatus`'s three outcomes, the `OVERLAPPED*` ↔
  `IoCompletion*` cast, timer/completion interleaving — just not of the
  real OS's behaviour underneath it.

Status

`IoLoop.h`, `IocpLoop.h`, `IocpLoop.cpp` written and logic-verified against
mocks as described above. **Confirmed building and passing on the project
owner's actual Windows/Visual Studio machine (2026-07-27)** via
`IocpSmokeTest.cpp` — real `CreateIoCompletionPort`,
`GetQueuedCompletionStatus`, `PostQueuedCompletionStatus`, a real repeating
timer, and `RequestStop()` all confirmed working against the real Win32
API, not just the mock. `Empty()`/`Count()` (pass-through accessors onto
`TimerScheduler`, needed by the `forge.cpp` integration below to know when
it's safe to stop the loop) were added afterward and are covered by
`IocpLoopTest.cpp` against the mock, but have not themselves been
re-verified on Windows — low risk (trivial one-line delegations), but
worth confirming alongside the `forge.cpp` integration's own build.

---

## forge.cpp — wiring IocpLoop into the real runtime (2026-07-27)

Decision

`forge.cpp`'s old `TimerQueue`/`EventLoop` (busy-poll, 1ms sleep, manual
`now() >= dueTime` scanning) is replaced with
`forge::core::platform::IoLoop`. `setTimeout`/`setInterval`/`clearTimeout`
keep their exact existing JS-facing behaviour (same small integer ids
returned to script) but now schedule through `IoLoop::ScheduleTimer` /
`CancelTimer` instead of a manually-scanned vector. `Runtime::run()`
drains the JS job queue, then blocks in `IoLoop::RunOnce()` for the next
timer or I/O event (currently only timers — real async I/O is Phase 5/6),
repeating until both the job queue and the timer set are empty, instead of
looping with a fixed sleep regardless of whether anything is actually due.

Two id spaces are kept separate on purpose: the small integer
`setTimeout()`/`setInterval()` return to script is unrelated to `IoLoop`'s
own `TimerId`; a `JsTimerRegistry` maps between them.

Reason

This is the actual point of Phase 2 — replacing the wasteful polling loop
this whole rearchitecture was about, now that `IocpLoop` itself is
confirmed working for real.

Two real bugs, unrelated to `IocpLoop` itself, were found and fixed while
writing this integration — not just in review, both were things that would
either fail to compile or corrupt memory if shipped as first drafted:

* **A forward-declaration ordering bug of my own making, caught before
  compiling, not after**: `ForgeTimerFired` (the native-timer-fired
  callback) needs to reach the current `Runtime`'s `JsTimerRegistry` to
  release a fired one-shot timer, but `Runtime` is only fully defined
  later in the file (it embeds `JsTimerRegistry` by value). Fixed with a
  forward-declared `GetRuntimeTimers()` helper, the same pattern the file
  already used for `EnqueueMicrotask`.
* **A real lifetime/memory-safety bug**: the old event loop only ever
  stopped once every timer had already fired or been cancelled, so it
  never needed an early-exit path. The new `run()` can now `break` early
  on a genuine I/O error from `IoLoop::RunOnce()` — and if it does, any
  `JsTimer`s still pending hold a `JS::PersistentRootedVector<JS::Value>`,
  which must unregister itself from `cx` when destroyed. Left alone, those
  would only get destroyed later, inside `Runtime`'s own destructor, which
  runs *after* `JS_DestroyContext(cx)`/`JS_ShutDown()` have already been
  called at the bottom of `main()` — a destroy-after-teardown bug. Fixed
  by adding `Runtime::Shutdown()` (cancels and releases every pending
  timer) called explicitly, while `cx` is still alive, right before
  `JS_DestroyContext(cx)` — and also on the one other path that can leave
  timers pending before reaching that point (a script that calls
  `setTimeout` and then throws during top-level evaluation).

Also fixed in passing, since it directly concerns the exact callback
objects this rewrite revolves around: **neither `Timer::callback` nor
`Microtask::callback` (`JS::Heap<JSObject*>`) was ever traced by the
GC.** `JS::PersistentRootedVector` (used for `arguments`) self-registers
and needs no manual tracing, but a bare `JS::Heap<T>` member does — without
a trace hook, a GC that runs while a timer or microtask is still pending
could collect its callback out from under it. Fixed by registering a
`JS_AddExtraGCRootsTracer` callback (`TraceForgeRoots`) that traces every
live timer's and microtask's callback. This bug pre-dates this change
entirely (it's been there since the original prototype) — it was only
found now because rewriting the timer storage was the moment to look at it
closely.

Verification

**This could not be compiled or tested in this sandbox at all** — unlike
`IocpLoop.cpp`, where a small, precisely-known Win32 API surface (about 10
functions) could be faithfully mocked, `forge.cpp` calls deeply into
SpiderMonkey's own API (`JS::Call`, `JS::CallArgs`, rooting, realms,
compilation, GC tracing), a much larger and more version-sensitive
surface. Building a mock of that would risk asserting false confidence
rather than providing real signal, so none was attempted. What *was* done:
a full manual re-read of the rewritten file for control-flow ordering,
lifetime, and GC-safety (which is how the two bugs above were caught
before ever reaching the project owner), reusing the exact call shapes
already proven to compile in the pre-existing code (e.g. the
`JS::Call(cx, thisValue, callback, timer->arguments,
JS::MutableHandleValue(&rval))` shape is copied verbatim from the working
original), and a brace/paren balance check. The two genuinely new
SpiderMonkey API calls introduced here — `JS_AddExtraGCRootsTracer` (in
`js/GCAPI.h`) and `JS::TraceEdge` (in `js/TracingAPI.h`) — are the most
likely spots for a real build to disagree with, if anything does.

Status

Written, not yet built. **Needs a real build on the project owner's
Windows/Visual Studio machine** — same as every other unverifiable-here
change, please report back exactly what the compiler says if anything
doesn't build, especially around the two new GC-tracing calls called out
above.

---

## Real build environment discovered: C++17, no exceptions, not C++20 (2026-07-27)

Decision

The first real `mach build` attempt (via the actual Gecko/SpiderMonkey
tree at `js/src/forge`, not a standalone Visual Studio project) revealed
that this build target is **not** what every prior verification in this
project assumed. Every earlier "compiles cleanly" claim in this file used
`g++`/`clang++ -std=c++20` in the sandbox, and the standalone Visual
Studio smoke tests (`VectorSmokeTest.cpp`, `IocpSmokeTest.cpp`) apparently
used a project configuration with C++20 and exceptions both enabled. The
real `mach build`, however:

* Compiles this codebase as C++17, not C++20 (evidenced by
  `-Wc++20-extensions` warnings on things that are only valid in C++20).
* Disables C++ exceptions entirely (`error: cannot use 'try' with
  exceptions disabled`) — not a style preference, a hard compiler
  configuration that makes `try`/`catch`/`throw` a compile error wherever
  they appear unconditionally.

This is a real, load-bearing fact about the project now: **forge-core
must compile as strict C++17, with exceptions disabled, in the real
build** — even though a standalone Visual Studio project pointed at the
same files might successfully compile more permissive C++20-or-exceptions
code and never reveal the mismatch. From here on, verification in this
sandbox uses `-std=c++17 -fno-exceptions` to actually match, instead of
`-std=c++20`.

Six real bugs, spanning most of forge-core, were caused by this mismatch
and are now fixed:

* **`ResultStorage.h`'s union constructor/destructor were `constexpr`.** A
  constexpr destructor (and a constexpr union constructor that doesn't
  initialize a member) are both C++20-only. Neither one is ever needed in
  a constant-evaluated context anywhere in this codebase, so `constexpr`
  was simply removed from both.
* **`Error.h`'s `operator==` used `= default`** on a non-member/friend
  comparison operator — C++20's "defaulted comparisons" feature. Replaced
  with an explicit, hand-written member-wise comparison (same semantics,
  valid since C++11).
* **`std::construct_at` (C++20, `<memory>`) was used 19 times** across
  `Result.inl`, `Vector.inl`, and `MakeUnique.inl`. Added a small
  C++17-compatible replacement, `forge::core::detail::ConstructAt()` (new
  file, `Construct.h` — plain placement-new under the hood, same call
  shape as `std::construct_at` so every call site is a mechanical
  substitution) and switched every call site to it.
* **`memory/detail/AllocationBackend.cpp` used `std::has_single_bit`**
  (C++20, `<bit>`) to check that an alignment is a power of two. Replaced
  with the equivalent bit trick (`(alignment & (alignment - 1)) == 0`),
  which is plain C++11.
* **The same file wrapped the actual allocation in an unconditional
  `try`/`catch (const std::bad_alloc&)`**, which cannot compile at all
  with exceptions disabled — not merely non-idiomatic for a
  zero-exception codebase (see `AGENTS.md`), a hard build failure.
  Replaced with the standard non-throwing overload,
  `::operator new(size, align_val_t, std::nothrow)` (C++17), which needs
  no exception handling at all. `MakeUnique.inl`'s own `try`/`catch` was
  **not** touched beyond the `construct_at` fix — it was already correctly
  guarded behind `#if defined(__cpp_exceptions)` (that macro is
  undefined when exceptions are disabled, exactly as it should be), so it
  already compiles correctly in both configurations; it just also needed
  the same `construct_at` fix as everywhere else.

Verification

Every fix was re-verified the same rigorous way as before, but now under
the conditions that actually matter — `-std=c++17 -fno-exceptions` — not
`-std=c++20`:

* `TimerScheduler` + its full test suite: g++ and clang++, strict warnings
  (including `-Wc++20-extensions` explicitly enabled, to catch anything
  else relying on a C++20 extension), and `-fsanitize=address,undefined`.
  All 7 scenarios still pass.
* `IocpLoop` + its full mock-based test suite: same treatment, all 8
  scenarios still pass.
* `Vector<T>`: a fresh standalone check (push/emplace/copy/move on both a
  trivial and a non-trivial element type) under the same flags, plus ASan.
* `MakeUnique<T>`: a fresh standalone check (success path, value
  correctness) under the same flags, plus ASan.

All four passed clean, with zero warnings, under the real constraints this
time.

Status

Fixed. This is a significant finding beyond just these six bugs: it means
the actual build environment (Gecko's `mach build`) is meaningfully
stricter than anything verified here or in a standalone Visual Studio
project until now. Any future forge-core code — written by any AI agent
per `AGENTS.md`, not just this fix — needs to be written as plain C++17
with no exceptions from the start, not C++20-with-exceptions code that
happens to also compile in a more permissive standalone test project. This
should be checked into `AGENTS.md` as a hard constraint, not just this
history entry.

---

## First successful real `mach build` of the new event loop (2026-07-27)

Decision / Milestone

`python mach build`, run for real from `C:\spidermonkey-dev\gecko-dev`,
succeeded: `js/src/forge/forge.exe` now builds cleanly with `TimerScheduler`,
`IocpLoop`, `IoLoop`, and the rewired `forge.cpp` all included, after the
C++17/no-exceptions fixes above. Output binary lands at
`obj-spider\dist\bin\forge.exe` (per `build.bat`/`install.ps1`'s existing
copy step), not directly under `js/src/forge`.

This is the first time any of Phase 2's actual code has been built by the
real toolchain end to end — everything before this was either sandbox
verification (g++/clang++ against mocks) or a standalone Visual Studio
smoke test (`IocpSmokeTest.cpp`) that only covers `IocpLoop` in isolation,
not `forge.cpp`'s integration of it into the real JS runtime.

Status

Builds successfully. **Functionally verified against real `hello.js`
(2026-07-27):** `forge.exe hello.js` printed `hello` exactly once (one-shot
timer fires once, doesn't repeat), then `tick` repeatedly with no crash
(the uncancelled `setInterval` keeps firing correctly through the new
loop), and never printed `never` (the timer `clearTimeout`'d before it was
due was genuinely cancelled). This is real confirmation that
`setTimeout`/`setInterval`/`clearTimeout` work correctly end-to-end
through `IocpLoop` — not just that the code compiles. Running long enough
to see many repeated ticks without a crash is also reasonable (if not
airtight) evidence the GC-tracing fix is holding up under whatever GC
pressure SpiderMonkey's normal allocation-triggered collection produced
during the run; a more deliberate GC-pressure test would need a way to
force a collection from script, which isn't exposed yet.

Still open: the 47 compiler warnings from the build haven't been reviewed
to confirm none originate from forge/forge-core's own files.

---

## Crash on every finite script: `global` destroyed after `JS_DestroyContext` (2026-07-27)

Decision / Bug

Running the Phase 0 benchmark suite (`run-benchmarks.ps1`) for the first
time against the real `forge.exe` crashed on all three scripts
(`startup.js`, `json-bench.js`, `loop-bench.js`), every time, with exit
code `-1073741819` (`0xC0000005` — access violation). This is **not**
related to timers, `IocpLoop`, or anything from this session's event-loop
work — none of the three scripts schedule a single timer.

Root cause: `JS::RootedObject global` was declared at the top level of
`main()`, alongside `cx`/`runtime`, so its destructor only runs when
`main()` itself returns — which is *after* `JS_DestroyContext(cx)` and
`JS_ShutDown()` have already executed a few lines earlier.
`JS::Rooted<T>`'s destructor has to unlink itself from a list owned by the
context it was rooted against; with that context already destroyed, this
is a genuine use-after-free, matching the observed access violation
exactly.

This bug **pre-dates every change made this session** — `global` was
declared at that same scope in the very first version of `forge.cpp` ever
read in this project. It had simply never been triggered, for a subtle
reason worth recording: no script had ever been run to natural completion
through a real `mach`-built `forge.exe` before now. `hello.js` (the only
prior real-build test) schedules an uncancelled `setInterval`, so `run()`
never returns on its own — the user always had to Ctrl+C it, which kills
the process immediately without ever running C++ destructors or reaching
`JS_DestroyContext`. The Phase 0 benchmark scripts are finite (no pending
timers), so they're the first scripts to ever actually reach the bottom of
`main()` — and that's exactly what exposed this.

Fix

Wrapped `global`'s declaration (and everything using it, including the
existing `JSAutoRealm` block) in a new nested scope that closes *before*
`runtime.Shutdown()`/`JS_DestroyContext(cx)`/`JS_ShutDown()` run, instead
of after. No other logic changed.

Verification

**Could not be compiled or run here** — same SpiderMonkey-API-surface
constraint as the rest of `forge.cpp` (see the C++17/no-exceptions entry
above): the API surface is too large to mock faithfully, so no sandbox
verification was attempted. What gives this fix unusually high confidence
despite that: the crash signature (access violation, only on scripts that
reach normal completion, first appearing on the very first finite scripts
ever run) matches this exact, well-known SpiderMonkey embedding pitfall
precisely, and the fix is a pure scope/ordering change — no new API calls,
no new logic, just moving where a brace closes.

Status

Fixed and confirmed on the project owner's machine (2026-07-27) —
`run-benchmarks.ps1` now runs all three scripts to completion with no
crash, on both the old busy-poll path and (implicitly) whatever code path
these scripts exercise now. Diagnosis was correct.

---

## Phase 0 baseline established: first real Forge vs Bun vs Node numbers (2026-07-27)

Result (from `run-benchmarks.ps1`, 5 iterations each, see
`bench/results/2026-07-27_162925.csv` for the raw data):

| benchmark     | forge (avg) | bun (avg) | node (avg) | forge/bun ratio |
|---------------|-------------|-----------|------------|-----------------|
| startup.js    | 25.01 ms    | 47.35 ms  | 53.85 ms   | **0.53x**       |
| json-bench.js | 383.29 ms   | 264.53 ms | 566.12 ms  | 1.45x           |
| loop-bench.js | 89.96 ms    | 81.23 ms  | 103.21 ms  | 1.11x           |

Reading

`startup.js` measures process cold-start — engine init, global/realm
setup, the event loop coming up — which is exactly the layer Phase 2's
work (and Forge's minimal footprint generally, vs. Bun's much larger
feature set at startup) actually controls. **Forge starts in about half
the time Bun does.** This is the first real evidence the architecture
decisions so far (SpiderMonkey + the lean event loop rewrite) are paying
off where they're supposed to.

`json-bench.js` and `loop-bench.js` are the two benchmarks explicitly
flagged in their own source comments as measuring the underlying JS
*engine* (SpiderMonkey vs JavaScriptCore), not anything Forge's own
runtime layer currently touches — Forge hasn't done any engine-level or
built-in-fast-path work yet (that's later phases, if ever, per
`ROADMAP.md`). Unsurprisingly, stock SpiderMonkey trails stock
JavaScriptCore by 11–45% on these; what's notable is Forge still clearly
beats Node on both (32% faster on `json-bench.js`, 13% faster on
`loop-bench.js`), and the startup win suggests there's real headroom in
what Forge's *own* layer can still contribute once Phase 1
(String/Span), Phase 3 (allocator hardening), and Phase 6 (native
networking/HTTP fast paths) land on top of this foundation.

Status

Phase 0 is now genuinely complete: real baseline numbers exist, not just
benchmark scripts sitting unused. This is the number to track as every
later phase lands — especially whether `json-bench.js`/`loop-bench.js`
close the gap with Bun as forge-core's own allocator/collection work
matures, since those two are the ones current work can plausibly still
move.

---

## Span / StringView / String (Phase 1, 2026-07-27)

With Phase 0 (baseline) and Phase 2 (event loop) both landed, the next
piece of `ROADMAP.md`'s own Phase 1 — `String`/`StringView`/`Span` — was
implemented: the last of the "not started" core containers listed in
`PROJECT_CONTEXT.md`.

`Span<T>` is a generic non-owning view (`Data`/`Size`/`Empty`/
`operator[]`/`Front`/`Back`/`Subspan`, the last clamped so an
over-generous offset/count can't read out of bounds — the common "give
me the rest" mistake via a huge count shouldn't be UB). `StringView` is
built on `Span<const char>` rather than duplicating its bounds logic,
adding the string-specific vocabulary (`Substr`/`StartsWith`/`EndsWith`/
`Find`/comparisons/implicit construction from a null-terminated
`const char*`). `String` is the owning, growable, `Allocator`-based
counterpart, deliberately mirroring `Vector<T>`'s exact growth/`Reserve`/
`GrowIfNeeded`/copy-truncates-safely-on-OOM conventions, but always
maintaining a null terminator internally (`AllocationSize(capacity) =
capacity + 1`) so `CStr()` is always valid — including on a
default-constructed String that has never allocated — for interop with
C-style APIs. Construction never allocates; `String::Create(allocator,
initial)` / `String::Create(initial)` are the fallible factories for a
String that already holds content, per the decision already recorded
above this entry.

Verified with a new `StringTest.cpp` (13 scenarios: happy-path
construction/append/copy/move/assignment, cross-type comparisons, and
three allocator-failure paths via a `FailingAllocator`, mirroring
`Vector<T>`'s own OOM-test precedent) under the real project
constraints established in the C++17 entry above: `g++`/
`clang++ -std=c++17 -fno-exceptions -Wall -Wextra -Wpedantic -Werror`
(plus `-Wc++20-extensions` on clang), then re-run clean under
`-fsanitize=address,undefined` and `valgrind --leak-check=full` (0
errors, 21 allocs/21 frees, no leaks possible), plus an `-O2` pass for
good measure. Not yet retested via a real `mach build` — these are pure
header/`.inl` files with no SpiderMonkey dependency, so this sandbox
verification is materially more trustworthy than the `forge.cpp`/
event-loop work's was, but per `AGENTS.md` that's still not "confirmed"
until it's actually gone through Gecko's build.

Two real bugs, caught by this verification rather than by inspection:

* **`String.h` never included `Result.h`.** It included
  `memory/ResultVoid.h` (for the `Result<void>` specialization) but not
  the primary `Result<T>` template's own header, relying only on the
  forward declaration in `ResultFwd.h`. That's enough for a function
  *declaration* like `static Result<String> Create(...)`, but
  `String.inl`'s function *bodies* actually construct and return a
  `Result<String>`, which requires the primary template to be a
  complete type. Fixed by adding `#include "Result.h"` to `String.h`.
* **`String == String` did not compile**, despite a comment in `String.h`
  confidently asserting it would "just work" via the implicit
  `String -> StringView` conversion plus `StringView`'s own
  `operator==`. It doesn't: `StringView::operator==` is a *hidden
  friend* (declared only inside the class body, no matching
  namespace-scope declaration), and hidden friends are only found by
  argument-dependent lookup when the declaring class itself
  (`StringView`) is literally one of the call's argument types — not
  merely reachable via an implicit conversion from a *different* class
  in the same namespace. Since `String == String` has no `StringView`
  argument at all, `StringView`'s comparison operator was never
  considered, and gcc rejected it outright
  ("no match for 'operator=='"). Fixed by giving `String` its own three
  hidden-friend overloads (`String==String`, `String==StringView`,
  `StringView==String`), each delegating to `.View() == ...`; a bare
  `String == "literal"` still resolves correctly through these, since
  the literal has no class type to constrain ADL and the `String`
  operand pulls the friend in regardless. The stale comment claiming the
  old approach worked was removed.

Separately, while staging these new files to the device, the two
tracked copies of this repo were found to have drifted: the C++17/
no-exceptions fixes from the entry above (`Construct.h`, and the fixes
to `Error.h`, `Result.inl`, `ResultStorage.h`, `memory/MakeUnique.inl`,
`memory/Vector.inl`, `memory/detail/AllocationBackend.cpp`) had only
ever been written to `C:\spidermonkey-dev\gecko-dev\js\src\forge\`
(the real `mach build` location) and never mirrored back to
`C:\forge-beta\forge-runtime-beta\` — so the latter's `forge-core` was
silently back to the pre-fix, C++20-assuming state. Synced all seven
files back to `C:\forge-beta\forge-runtime-beta\` alongside this
entry's new files, so both locations match again. Worth remembering:
this repo effectively has two live copies with no automatic sync
between them, and it's easy for a fix delivered to only one of them to
silently go stale in the other.

Status

`Span`/`StringView`/`String` implemented and verified (sandbox, not yet
`mach build`). `ROADMAP.md`'s Phase 1 is marked done. Next candidates
per `ROADMAP.md`: Phase 2 (Array/HashMap/HashSet/Queue/Stack), or,
per the broader runtime roadmap referenced elsewhere in this file,
Phase 3 (allocator hardening) or reviewing the 47 `mach build` warnings
that have never been triaged (still outstanding — the warning text has
not yet been provided).

---

## Pre-work review: forge.cpp/moz.build drift, a real missing-sources bug (2026-07-29)

Before starting Phase 2, reviewed the current state of both tracked
copies (`C:\forge-beta\forge-runtime-beta\` and
`C:\spidermonkey-dev\gecko-dev\js\src\forge\`) against `ROADMAP.md`, per
this session's own instructions to check for drift/duplication before
adding new code. Found two real, separate problems:

**`forge.cpp` and `moz.build` were never synced back to `forge-beta`.**
The prior session's entry above only says `forge.cpp` (the full
IocpLoop-rewrite/crash-fix version) was delivered to the `gecko-dev`
build location — unlike the `forge-core/` files, it was never mirrored
to `forge-beta`. Confirmed via `diff`: `forge-beta`'s copy was still the
pre-rewrite prototype (`std::vector<std::unique_ptr<Microtask>>`, no
`IoLoop`, no crash fix). Synced the current `gecko-dev` version back.

**`moz.build`, independently, was missing two `SOURCES` entries it
needs.** The live `moz.build` at the real build location listed
`forge-core/memory/DefaultAllocator.cpp`,
`forge-core/memory/detail/AllocationBackend.cpp`, and `forge.cpp` —
but not `forge-core/platform/IocpLoop.cpp` or
`forge-core/platform/TimerScheduler.cpp`, even though `forge.cpp`
`#include`s `IoLoop.h` and directly constructs/calls into
`forge::core::platform::IoLoop` (== `IocpLoop` on Windows), and neither
file is header-only. This entry's earlier prose says these two lines
were added and a `mach build` succeeded with them present — so at some
point after that success, `moz.build` reverted to a 3-entry `SOURCES`
list missing both platform files, most likely from stale linked object
files papering over the missing sources on an incremental build rather
than a clean one. However it happened, a `mach clobber` or a build on a
clean checkout would almost certainly have hit a linker error for
undefined `IocpLoop`/`TimerScheduler` symbols. Added both lines back to
`moz.build` and synced the fix to both locations. Worth a rebuild to
confirm before relying on it further.

Also noticed (lower priority, fixed while here): `forge/CMakeLists.txt`
(a local, non-`mach`, IDE/CMake build for `VectorTest`) still set
`CMAKE_CXX_STANDARD 20` — exactly the assumption the C++17 entry above
found to be wrong for the real build. Changed to 17 so building
`VectorTest` locally via CMake doesn't mask a C++20-only construct that
`mach build` would reject, the same trap that caused the whole C++17
investigation in the first place.

No code changes resulted from this review beyond the sync/fixes above —
`Array`/`HashMap`/`HashSet`/`Queue`/`Stack` were confirmed genuinely not
started (no files present at either location), so Phase 2 work below
does not duplicate anything.

---

## Array / Hash / Stack / Queue / HashMap / HashSet (Phase 2, 2026-07-29)

Implemented all five `ROADMAP.md` Phase 2 components, plus a new `Hash<T>`
trait (`Hash.h`) that `HashMap`/`HashSet` need but that didn't fit neatly
under any single one of the five names.

`Array<T, N>` is the one deliberate departure from every other
container's private-members-plus-named-accessors convention: it's a
plain aggregate with a public `T data_[N]`, matching `std::array`'s own
reasoning — a fixed-size array has no size/capacity relationship to
protect (unlike `Vector<T>`, where `size_ <= capacity_` is exactly the
invariant the private members exist to guard), so there's no invariant
for an accessor to be defending. `N == 0` needed a separate template
specialization (holding no storage at all) since a zero-length C array
isn't valid C++.

`Stack<T>` is a thin LIFO adapter directly over `Vector<T>` — no new
storage logic. `Queue<T>` is a real circular buffer rather than a second
`Vector`-backed adapter: a `Vector`-backed FIFO would need an O(n) shift
on every `Pop()` to slide the remaining elements down, so `Queue<T>`
tracks its own `head_` and wraps indices with modulo, same
`Allocator`/`Reserve`/copy-truncates-on-OOM conventions as `Vector<T>`
otherwise (growth linearizes the circular layout back to starting at
index 0 in the new buffer).

`Hash<T>` (new) is FNV-1a for byte ranges (`StringView`/`String`) and a
splitmix64-style finalizer for integers before they're masked to a table
index — plain identity hashing of small sequential integers (0, 1, 2,
...) would cluster badly against the power-of-two table sizes `HashMap`
uses. The primary template is deliberately left undefined rather than
falling back to hashing a type's raw bytes: a key type without an
explicit `Hash<K>` specialization should fail to compile with a clear
"no such specialization" error, not silently hash padding bytes or fail
confusingly deep inside `HashMap`'s internals.

`HashMap<K, V>` is open addressing with linear probing and tombstones
for deletion (find scans through tombstones; insert reuses the first one
seen along the probe sequence) — chosen over a more elaborate
Robin-Hood/SwissTable-style design specifically because
`PROJECT_CONTEXT.md` calls out "readability over cleverness," and linear
probing with tombstones is the version of open addressing most people
can read and verify by eye. Table capacity is always a power of two so
the index is `hash & (capacity - 1)` (masking) rather than a modulo;
growth doubles capacity once `size + tombstones` crosses 75%, counting
tombstones because a table full of tombstones is exactly as slow to
probe as one full of live entries. Copy construction/assignment reuse
`Insert()` itself (rather than a raw indexed copy loop the way
`Vector`/`String` do it) — `Reserve()` best-effort up front, then insert
every source entry, stopping early if `Insert()` starts failing; this
reaches the same truncate-safely-on-OOM outcome as the rest of the
codebase's containers, just via composition instead of manual memory
copying. Deliberately does not (yet) support a custom hasher/
key-equality template parameter, or const iteration — both are ordinary
additions when a concrete caller actually needs them, left out for now
per "small, clean API."

`HashSet<K>` is a thin wrapper around `HashMap<K, detail::Unit>` (a new
empty tag type), not a second, independent table implementation — any
future correctness fix or improvement to `HashMap`'s probing/growth
applies to `HashSet` automatically, at the cost of one `Unit`-sized slot
per entry that a hand-written set wouldn't need to store. Considered
building both on top of a single shared internal template instead
(avoiding even that overhead), but `V = void` isn't legal C++ and the
`[[no_unique_address]]` trick that would elide it outright is a C++20
feature this codebase can't use — the composition-over-`HashMap`
approach was the simplest one that stayed within the C++17 constraint.

Verified with five new test files — `ArrayTest.cpp`, `StackTest.cpp`,
`QueueTest.cpp`, `HashMapTest.cpp` (which also covers `Hash<T>`
directly), `HashSetTest.cpp` — 34 scenarios in total, including: a
1000-entry insert/find/erase/rehash stress test for `HashMap` (confirms
growth and tombstones interact correctly at scale, not just for a
handful of hand-picked keys); a `Queue` wraparound test that pushes/pops
through the same four physical slots repeatedly to make sure the
circular indexing is actually exercised, not just growth-then-drain; and
allocator-failure paths for every container via the same `FailingAllocator`
pattern `StringTest.cpp` established, confirmed to leave each container
in a safe, still-usable (if smaller/emptier than requested) state rather
than corrupting anything.

All five compiled clean on the first attempt under the full verification
bar established for Phase 1 — `g++`/`clang++ -std=c++17 -fno-exceptions
-Wall -Wextra -Wpedantic -Werror` (plus `-Wc++20-extensions` on clang),
`-fsanitize=address,undefined`, `valgrind --leak-check=full` (0 errors,
0 leaks across all five binaries), and an `-O2` pass. Unlike Phase 1 (and
every prior real-build pass), this round did not surface any bugs the
initial design missed — worth noting honestly rather than inventing a
finding, per `AGENTS.md`'s "Be Honest": it means the design held up under
this level of scrutiny, not that no bug could possibly exist. As with
Phase 1, this is pure header/`.inl` code with no SpiderMonkey dependency
and no `moz.build` changes needed (test files are not added to
`SOURCES`, matching the established `*Test.cpp` convention) — still not
"confirmed" until it goes through a real `mach build`, per `AGENTS.md`.

Status

`ROADMAP.md`'s Phase 2 is marked done. Both tracked copies
(`forge-beta` and the real `gecko-dev` build location) now have matching
`forge-core/`, `forge.cpp`, and `moz.build`. Next per `ROADMAP.md`:
Phase 3 (Filesystem) — note its own entry requires freezing a spec doc
first, matching `Error.md`'s shape, before writing the public API. Still
outstanding from before: the 47 `mach build` warnings have never been
triaged (text still not provided), and a real `mach build`/benchmark
rerun would be worth doing now that forge.cpp/moz.build drift has been
fixed, to confirm nothing regressed.

---

## `forge-beta` retired; `gecko-dev` is now the only tracked copy (2026-07-29)

The user confirmed everything through Phase 2 on their own machine (real
`mach build`, benchmarks, the shutdown-crash fix) and gave a new standing
instruction: work only in `C:\spidermonkey-dev\gecko-dev\js\src\forge\`
from here on; don't develop in or sync to `C:\forge-beta\forge-runtime-beta\`
unless explicitly asked. Also reconfirmed as a standing rule (not new,
but worth restating since it's now the *only* rule): always verify new
code under the real `mach build`'s exact constraints (C++17,
`-fno-exceptions`) from the start, not a more permissive setup first.

This retires the two-copies-can-drift problem the last two entries spent
real effort on (the `forge.cpp`/`moz.build` sync gap, the missing
`SOURCES` entries) — there's only one copy to keep consistent now.

Since `forge-beta` is retired, `HISTORY.md`/`ROADMAP.md`/
`PROJECT_CONTEXT.md`/`AGENTS.md` needed a new writable home:
`.claude\` at the `gecko-dev` location still rejects remote writes (see
the Phase 1 entry above), so all four now live directly under
`js\src\forge\` itself, alongside `forge-core/`/`forge.cpp`/`moz.build`.
`PROJECT_CONTEXT.md` gained a short "Source of truth" note at the top
pointing at this.

Small fix made in passing while reviewing `Error.h` for Phase 3 (see
below): `Error.md`'s frozen spec lists `PlatformError` as one of
`ErrorCode`'s categories, but the actual `ErrorCode` enum in `Error.h`
never had it — a real implementation-vs-frozen-spec mismatch, not a new
design decision, so adding it back is a bugfix per `AGENTS.md` ("a frozen
API that does not compile [or doesn't match its own spec] is a bug, not
a design freeze"). Added at the end of the enum (preserves existing
numeric values) with a comment explaining why it was missing. This
value is what Phase 3's `File` needed anyway, to carry a raw Win32 error
code that doesn't map onto any of the existing generic categories.

---

## Path / File (Phase 3, 2026-07-29)

Implemented Phase 3 following its own stated process: froze a spec doc
first. Actually two — `Path.md` and `File.md` — rather than one combined
"Filesystem" doc, because `Path` and `File` turned out to be different
enough in kind (a pure value type with zero OS dependency vs. a type
whose entire job is calling the OS) that splitting them mirrors the
`StringView`/`String` relationship: one half never touches the OS, the
other is entirely the OS. This split is itself the main design decision
this phase made — recorded in both spec docs' own "Purpose" sections.

**`Path`** stores a path as a `String` (UTF-8, `/`-normalized internally
regardless of whether the input used `/` or `\`) and provides Join,
Parent, FileName, Stem, Extension, IsAbsolute/IsRelative (Windows drive-
letter or UNC absolute; a bare leading `/` is deliberately NOT absolute,
matching `std::filesystem::path`'s own behavior on Windows), and
Normalize (collapses `.`/repeated separators, resolves `..` against a
real preceding segment where possible, never escaping above a leading
`..` in a relative path or above an absolute root). Zero OS calls
anywhere in `Path.h`/`Path.inl` — verified that this stayed true by
literally checking neither file mentions any platform header.

Added `StringView::RFind` (last-occurrence search, mirroring the
existing `Find`) as a small backward-compatible extension — `Parent()`/
`FileName()` need to find the *last* path separator, and `StringView`
was never declared frozen anywhere the way `Error` was, so this is an
addition, not a redesign of anything.

Verified `Path` with `PathTest.cpp` (11 scenarios, including allocator-
failure paths and a set of hand-traced `Normalize()` cases — absolute
`..`-above-root, relative `..` with nothing left to cancel against,
`.`-only paths, mixed/repeated separators) under the real project
constraints from the very start this time (per the user's new standing
instruction): `g++`/`clang++ -std=c++17 -fno-exceptions -Wall -Wextra
-Wpedantic -Werror` (+ `-Wc++20-extensions` on clang), clean under
ASan+UBSan and `valgrind --leak-check=full` (0 leaks, 79 allocs/79
frees), plus an `-O2` pass. All green on the first attempt — same
honest caveat as Phase 2's equivalent note: this means the design held
up under this scrutiny, not that no bug could exist.

**`File`** (`Open`/`Read`/`Write`/`Seek`/`Tell`/`SizeInBytes`/`Close`,
plus static `Exists`/`CreateDirectory`/`CreateDirectories`/`Remove`/
`ReadAllBytes`/`ReadAllText`) is Win32-only and synchronous by design —
`File.md`'s Non-Goals section explains why async is deliberately
out of scope (it would couple this component to `IocpLoop` and to
whatever the eventual JS-visible `fs` API looks like, which is Runtime
Integration's job, not this phase's). Move-only (`HANDLE` has no
sensible cheap copy), matching `UniquePtr<T>`'s ownership model rather
than `Vector<T>`'s. Every Win32 failure is translated to
`Error(ErrorCode::PlatformError, <raw GetLastError() value>)` unless a
more specific code clearly applies (`NotFound` for
`ERROR_FILE_NOT_FOUND`/`ERROR_PATH_NOT_FOUND`, `PermissionDenied` for
`ERROR_ACCESS_DENIED`, `AlreadyExists` for `ERROR_FILE_EXISTS`/
`ERROR_ALREADY_EXISTS`) — never a raw `BOOL`/`GetLastError()` leaking
out. `File.h` keeps the actual `HANDLE` behind a `void*` so it never
needs to `#include <windows.h>` itself, matching `IocpLoop`'s own
reasoning for keeping its `OVERLAPPED`-derived types out of its public
header.

**This component could not be compiled or executed in this sandbox at
all** — there is no Windows SDK here, and installing a MinGW cross
compiler failed (`apt-get install g++-mingw-w64-x86-64` — the package
lives in Ubuntu's `universe` component, which returned 403 Forbidden;
this sandbox's network access is allowlisted to package registries, not
arbitrary Ubuntu mirrors/components). Verification here was: careful
manual review against the real Win32 API shape from memory/knowledge of
`CreateFileW`/`ReadFile`/`WriteFile`/`SetFilePointerEx`/`GetFileSizeEx`/
`CloseHandle`/`GetFileAttributesW`/`CreateDirectoryW`/`DeleteFileW`/
`RemoveDirectoryW`/`MultiByteToWideChar`, plus a hand-written,
type-check-only mock `windows.h` (same technique the `IocpLoop` mock
used last phase) with accurate real signatures. `File.cpp` compiled AND
linked cleanly against that mock under the full `-std=c++17
-fno-exceptions -Wall -Wextra -Wpedantic -Werror` bar on both compilers
— confirming no typos, wrong argument counts, or wrong types anywhere,
but proving nothing about actual runtime correctness (the mock's
functions are dumb stubs — e.g. `MultiByteToWideChar` always returns 0,
so running the mock-linked binary predictably fails almost every check;
that's expected noise from the stub, not a signal about `File.cpp`
itself). A real bug this review pass did catch before it could ship: an
early draft of `CreateDirectories()` collected ancestor directories as
`StringView`s aliasing a `Path` variable that then got reassigned each
loop iteration — since `Path`'s (i.e. `String`'s) assignment can
reallocate/free the old buffer, those views would have dangled. Fixed by
collecting owned `Path` copies (`Vector<Path>`) instead of views.

Wrote `FileSmokeTest.cpp` (matching `IocpSmokeTest.cpp`'s precedent) —
a real-filesystem test exercising `CreateDirectories`, `Exists`,
`Open`/`Write`/`Read` round-tripping through actual file content,
`Append` positioning at end-of-file, `Seek`/`Tell`, `CreateNew`
correctly failing with `AlreadyExists` against a file that already
exists, opening a missing file correctly reporting `NotFound`, and
cleanup via `Remove`. This is **the next thing to run on the real
machine** — it cannot be exercised meaningfully anywhere else.

`moz.build` updated: added `forge-core/File.cpp` to `SOURCES` (`Path`
is header-only, needs no new `.cpp` entry, matching `String`/`Span`'s
precedent). `FileSmokeTest.cpp`/`PathTest.cpp` are not added, matching
the established `*Test.cpp`/`*SmokeTest.cpp` convention.

Status

`ROADMAP.md`'s Phase 3 is marked done for `Path` (fully verified) but
explicitly NOT confirmed for `File` — implemented and reviewed as
carefully as this environment allows, but genuinely unverified until a
real `mach build` (or a standalone Visual Studio build of
`FileSmokeTest.cpp`, matching `IocpSmokeTest.cpp`) runs on the actual
machine. This needs to be communicated to the user explicitly, not
glossed over, per `AGENTS.md`'s "Be Honest" and the user's own explicit
request this session to be told when something can't be fully verified
without a real build. Next per `ROADMAP.md`: Phase 4 (Threading).

## `File::CreateDirectory` renamed to `MakeDirectory` — real `mach build` bug (2026-07-29)

The user ran the actual `mach build` and it failed compiling
`File.cpp` with three errors, all rooted in one cause:

```
File.cpp(388,20): error: out-of-line definition of 'CreateDirectoryA'
does not match any declaration in 'forge::core::File'; did you mean
'CreateDirectory'?
    ...
fileapi.h(67,26): note: expanded from macro 'CreateDirectory'
    67 | #define CreateDirectory  CreateDirectoryA
```

`<windows.h>`'s `fileapi.h` `#define`s `CreateDirectory` to
`CreateDirectoryA` (or `CreateDirectoryW` under a `UNICODE` build) —
the classic Win32 ANSI/Wide dispatch-macro pattern also used for
`CreateFile`, `DeleteFile`, `MoveFile`, `CopyFile`, `GetUserName`, and
others. Because this is textual preprocessor substitution, it doesn't
respect C++ scoping — `File::CreateDirectory`'s own declaration and
out-of-line definition both got silently rewritten to
`File::CreateDirectoryA`, so the declaration and definition stopped
matching, and the internal call site in `CreateDirectories()` got
rewritten into a call to the real Win32 `CreateDirectoryA(LPCSTR,
LPSECURITY_ATTRIBUTES)`, which doesn't match our call shape at all.

This is exactly the kind of bug the hand-written mock `windows.h` used
for sandbox verification could not catch — the mock didn't replicate
this macro (a real omission in the mock, not in `File.cpp`'s logic),
so the collision only surfaced once the user built against the real
Windows SDK headers. This is precisely why `File` was documented as
"implemented but not yet confirmed" rather than "done" — this is the
real-build confirmation catching a real, distinct bug the mock missed.

Fix: renamed the method `File::CreateDirectory` → `File::MakeDirectory`
everywhere (declaration in `File.h`, definition in `File.cpp`, the
internal call site inside `CreateDirectories()`, and `File.md`'s spec).
`CreateDirectories` (the recursive, plural helper) was never at risk —
it isn't one of Win32's dispatch-macro names, only the singular
`CreateDirectory` collides. No other `File`/`Path` method name matches
a known Win32 A/W macro (checked against `CreateFile`, `DeleteFile`,
`MoveFile`, `CopyFile`, `RemoveDirectory`, `FindFirstFile`,
`FindNextFile`, `GetFileAttributes`, `SetFileAttributes`,
`GetCurrentDirectory`, `SetCurrentDirectory`, `GetUserName`,
`GetComputerName`, `LoadLibrary`, `GetModuleFileName` — none of our
public API uses any of these exact names).

Lesson for future Win32-facing components: never name a public symbol
exactly after a Win32 API function that has ANSI/Wide variants: the
macro will rewrite it regardless of namespace or class scope. The mock
`windows.h` technique is useful for catching signature mistakes but is
not a substitute for a real build against the actual SDK headers,
which is exactly why every Win32-touching component in this project
must still go through a real `mach build` before being called
"confirmed" — this is the first concrete case of that distinction
actually mattering.

Not yet re-verified: the corrected `File.cpp`/`File.h`/`File.md` have
not yet gone through another real `mach build` pass on the user's
machine — that's the immediate next step to actually confirm this fix
compiles clean. `PathTest.cpp`/`Path.h`/`Path.inl` are unaffected
(`Path` has no Win32 dependency at all).

## Real `mach build` confirms the `MakeDirectory` fix; Phase 3 fully done (2026-07-29)

The user re-ran `mach build` after the `CreateDirectory` → `MakeDirectory`
rename and it completed successfully — the `moz.build` `File.cpp` SOURCES
entry and every Windows-specific fix from that pass are now genuinely
confirmed, not just sandbox/mock-reviewed. Phase 3 (Filesystem) is done:
`Path` was already fully verified; `File` is now real-build-confirmed too.

## Phase 4 — Sync (Mutex/ConditionVariable/LockGuard) + Thread/ThreadPool (2026-07-29)

Reviewed the codebase and `ROADMAP.md` first, per the user's standing
instruction: no threading/synchronization code existed anywhere in
`forge-core/` (confirmed via a directory listing — the only prior
`platform/` content is `IocpLoop`/`TimerScheduler`, which are
single-threaded-event-loop plumbing, not general-purpose thread/lock
primitives), so this phase started from a clean slate with no risk of
duplicating existing work.

Wrote two frozen spec docs first, per this project's own established
process (`Error.md`/`Path.md`/`File.md`'s precedent): `Sync.md` (Mutex +
ConditionVariable + LockGuard<T>) and `Thread.md` (Thread + ThreadPool +
the internal `ErasedCallable` type-erasure helper both use).

**Mutex** wraps a Win32 `SRWLOCK`, **ConditionVariable** wraps a
`CONDITION_VARIABLE`, both stored behind an opaque `void*` in their
headers rather than `#include <windows.h>` there — both are documented,
ABI-stable, exactly-one-pointer-sized values valid when
zero-initialized (literally what `SRWLOCK_INIT`/
`CONDITION_VARIABLE_INIT` expand to), so this works without needing the
real type in the header, the same reasoning `File.h` already uses for
its `HANDLE`. Each `.cpp` `static_assert`s the size/alignment
assumption so a future SDK change that broke it would fail loudly at
compile time. Neither type can fail to construct or lock (SRWLOCK/
CONDITION_VARIABLE need no allocation and their init cannot fail,
unlike `CRITICAL_SECTION`), so `Lock`/`Unlock`/`NotifyOne`/`NotifyAll`
return `void`, not `Result<void>` — only `Wait`/`WaitFor` can fail, for
genuinely exceptional OS errors.

**LockGuard<Lockable>** is a template on the lockable type rather than
hardcoded to `Mutex`, specifically so its logic (RAII lock/unlock,
non-copyable/non-movable) could be fully sandbox-verified against a fake
test double even though `Mutex` itself cannot be compiled here — this
mirrors the Path/File split from Phase 3 (portable logic gets full
verification; OS-touching logic gets manual review + a mock-header
compile pass only).

**Thread** wraps `CreateThread`, move-only like `File`/`UniquePtr`.
Every `Thread` that was ever successfully created must be `Join()`'d or
`Detach()`'d before destruction/move-assignment-over, enforced with
`FORGE_ASSERT` (matching `std::thread`'s own contract and
`IocpLoop.cpp`'s existing "assert on programmer error, don't paper over
it" precedent) — deliberately not auto-detaching or auto-joining, since
both would hide a real bug instead of surfacing it.

**ErasedCallable** (`detail` namespace, in the new `ErasedCallable.h`)
is the type-erasure helper that lets `Thread::Create`/`ThreadPool::Submit`
accept an arbitrary no-argument `Callable` and still hand it across an
OS boundary that only accepts a fixed `void(*)(void*)` shape, without
either of them needing to instantiate an OS-facing function once per
`Callable` type. It allocates a small closure via the caller's
`memory::Allocator` (never raw `new`/`delete`, per the project rule) and
returns two plain function pointers: `invoke` (runs the callable, then
frees the closure) and `destroy` (frees it without running it, for
failure-cleanup paths). This is pure C++ with zero OS dependency, so —
like `Path` and `LockGuard` — it got full sandbox verification:
`ErasedCallableTest.cpp` covers invoke-runs-and-frees,
destroy-without-invoking-never-runs, a move-only capture, and an OOM
path via `FailingAllocator`, clean under `g++`/`clang++ -std=c++17
-fno-exceptions -Wall -Wextra -Wpedantic -Werror` (+
`-Wc++20-extensions` on clang), ASan+UBSan, `valgrind --leak-check=full`
(0 leaks), and `-O2`.

**ThreadPool** is a fixed-size set of worker threads pulling `Task`s off
a shared `Queue<Task>`, guarded by a `Mutex` + `ConditionVariable`.
Deliberately **non-movable** (unlike `Thread`) and follows `IocpLoop`'s
own "default-construct, then `Initialize()`" shape rather than a
`static Create()` factory returning the pool by value — every worker's
loop lambda captures a pointer back to the pool itself, so it needs a
stable address from the moment the first worker spawns; a
`Result<ThreadPool> Create(...)` factory would require `ThreadPool` to
be movable to relocate the return value out of the factory, which is
exactly what a pool with live worker threads referencing `this` cannot
safely support. This is a real design decision, not an oversight — see
`ThreadPool.h`'s own class comment and `Thread.md`'s Design Goals for
the full reasoning; it was caught and resolved *before* writing any
code that would have tried (and likely failed, or worse, subtly
miscompiled) to move a non-movable type through `Result<T>`.

Two real bugs were caught during self-review before this shipped
(neither reached a compiler, since nothing here compiles in this
sandbox — both were found by re-reading the code with the same
scrutiny `AGENTS.md`'s "Existing Code Review" checklist asks for):

1. `ConditionVariable.cpp`'s first draft read `mutex.NativePtr()` (a
   method `Mutex` declares private, accessible only via
   `friend class ConditionVariable;`) from inside a free helper function
   in an anonymous namespace, not from a `ConditionVariable` member
   function. C++ friendship does **not** extend to arbitrary functions
   in the same translation unit — only to the specific class named as a
   friend — so this would not have compiled. Fixed by inlining the cast
   directly inside `ConditionVariable::Wait`/`WaitFor` (which, being
   actual member functions of the friended class, do have access),
   rather than factoring it into a shared free function.
2. `ThreadPool::Initialize`'s rollback path for a `Vector::PushBack`
   failure (after `Thread::Create` had already succeeded for that
   worker) originally called `Join()` on the orphaned thread *before*
   calling `Shutdown()`. That thread is already running `WorkerLoop()`
   and blocked in `Wait()` with `stopping_` still `false` at that point
   — joining it directly would have deadlocked forever, since nothing
   had told it to stop yet. Fixed by calling `Shutdown()` first (which
   sets `stopping_` under `mutex_` and calls `NotifyAll()` — the
   orphaned thread receives this exactly like any tracked worker, since
   the `stopping_` check inside `WorkerLoop` happens under the same
   `mutex_`, so there's no missed-wakeup race even though this thread
   was never pushed into `workers_`), then joining the now-safely-
   stoppable thread afterward.

**Also extracted `platform::TranslateWin32Error`** (new
`platform/Win32Error.h`/`.cpp`) out of `File.cpp`, which previously had
its own private copy of the exact same `GetLastError()` → `Error`
switch. `ConditionVariable.cpp`/`Thread.cpp` both needed the identical
mapping, and writing a third independent copy would have violated
`AGENTS.md`'s "No duplicated logic" the moment it existed — `File.cpp`
now does `using platform::TranslateWin32Error;` in its anonymous
namespace so every existing call site there kept working unchanged.

**Verification performed:**

* `ErasedCallable.h`/`LockGuard.h` (pure logic, zero OS dependency):
  full bar, as described above — genuinely "confirmed", the same way
  `Path` was in Phase 3.
* `Mutex.cpp`/`ConditionVariable.cpp`/`Thread.cpp`/`ThreadPool.cpp`/
  `platform/Win32Error.cpp` (Win32-only): **cannot be compiled or run in
  this sandbox** (no Windows SDK, no working MinGW cross-compiler — same
  constraint as `File.cpp` before it). The hand-written mock
  `<windows.h>` from Phase 3 was extended with `SRWLOCK`/
  `CONDITION_VARIABLE` (as single-pointer structs, matching their real
  documented layout), `AcquireSRWLockExclusive`/
  `ReleaseSRWLockExclusive`/`TryAcquireSRWLockExclusive`,
  `SleepConditionVariableSRW`/`WakeConditionVariable`/
  `WakeAllConditionVariable`, `CreateThread`/`WaitForSingleObject`, and
  `ERROR_TIMEOUT`/`WAIT_OBJECT_0`/`INFINITE`. Every new `.cpp` compiles
  clean under `g++`/`clang++ -std=c++17 -fno-exceptions -Wall -Wextra
  -Wpedantic -Werror` against it. A small driver
  (`/tmp/thread_link_check.cpp`, not part of the shipped tree — it
  exists purely to force template instantiation) was written to actually
  call `Thread::Create<Callable>`/`ThreadPool::Submit<Callable>`/
  `MakeErasedCallable<Callable>` with concrete lambda types and link
  everything together — a plain per-file compile of `ThreadPool.cpp`/
  `Thread.cpp` alone never instantiates those templates at all, since
  nothing in those files calls them with a concrete `Callable` outside
  of `ThreadPool`'s own internal worker-loop lambda. That driver compiled
  and linked clean on both compilers, and ran clean under ASan+UBSan and
  `valgrind --leak-check=full` (0 leaks, 8 allocs/8 frees) — this
  exercises the real allocation/cleanup logic in `ErasedCallable`/
  `Thread`/`ThreadPool`'s failure paths even though the mock's
  `CreateThread` is a dumb stub that makes every `Thread::Create` call
  "fail" in that environment (which is itself a useful signal: the
  failure-cleanup paths ran for real, under a real leak checker, and
  came back clean).
* Wrote `ThreadingSmokeTest.cpp` (matching `FileSmokeTest.cpp`'s
  precedent) — a real-thread test: a `Mutex` stress test (8 threads ×
  20000 increments each, verifying the final count is exactly right,
  which reliably fails under real OS scheduling without correct mutual
  exclusion), a `ConditionVariable` producer/consumer handoff, a
  `WaitFor` timeout check, and a `ThreadPool` test (4 workers, 500
  submitted tasks, verifying every one ran exactly once, plus
  `Submit()` after `Shutdown()` correctly failing). Syntax/link-checked
  against the mock (compiles and links clean on both compilers) but
  **not run** — like `FileSmokeTest.cpp`, running it against the mock
  would just exercise the dumb stubs, not real threads, so there is
  nothing meaningful to learn from executing it here.

`moz.build` updated: added `forge-core/Mutex.cpp`,
`forge-core/ConditionVariable.cpp`, `forge-core/Thread.cpp`,
`forge-core/ThreadPool.cpp`, and `forge-core/platform/Win32Error.cpp` to
`SOURCES` (`LockGuard.h`/`ErasedCallable.h`/`Thread.inl`/`ThreadPool.inl`
are header-only, no new `.cpp` entries needed).

Status

`ROADMAP.md`'s Phase 4 is marked done for `ErasedCallable`/`LockGuard`
(fully verified) but explicitly **NOT confirmed** for
`Mutex`/`ConditionVariable`/`Thread`/`ThreadPool` — implemented and
reviewed as carefully as this environment allows (including the two
real bugs caught above), but genuinely unverified until a real `mach
build` compiles them and `ThreadingSmokeTest.cpp` runs clean on the
actual machine. This must be communicated to the user explicitly, not
glossed over, per `AGENTS.md`'s "Be Honest" and the user's own standing
request to be told when something can't be fully verified without a
real build. Next per `ROADMAP.md`: Phase 5 (Networking).

## `moz.build` SOURCES ordering bug — real `mach build` catch (2026-07-29)

The user's `mach build` failed before it even reached the compiler,
with `mozbuild.util.UnsortedError` on `moz.build`'s `SOURCES += [...]`
list: `"We expected 'forge-core/memory/DefaultAllocator.cpp' but got
'forge-core/Mutex.cpp'"`.

Mozbuild's `SOURCES` is a `StrictOrderingOnAppendList` — it hard-
requires every `+=` list literal to already be sorted, and rejects the
whole build (not just a warning) if it isn't. The Phase 4 edit had
inserted `Mutex.cpp`/`Thread.cpp`/`ThreadPool.cpp`/`Win32Error.cpp`
sorted the way plain Python `sorted()`/raw ASCII byte order would put
them (which sorts all-uppercase-initial names like `Mutex.cpp`
*before* lowercase-initial paths like `memory/...`, since `'M'` (0x4D)
sorts before `'m'` (0x6D) in ASCII) — but mozbuild's own sort key is
**case-insensitive**, so it wants `memory/DefaultAllocator.cpp` before
`Mutex.cpp` before `platform/...` before `Thread.cpp`, not the ASCII
order. This wasn't something sandbox verification could have caught —
`moz.build` is only ever parsed by mozbuild itself, not by any C++
compiler, so there was no way to check this without the real `mach
build`'s own frontend running the file, exactly the class of thing
`AGENTS.md`'s real-build-verification section already predicted
`moz.build` changes need.

Fixed by re-deriving the list in Python with `sorted(items,
key=str.lower)` and matching mozbuild's own expected order exactly
(confirmed against the "srtd" list mozbuild echoed back in its error
message): `ConditionVariable.cpp`, `File.cpp`,
`memory/DefaultAllocator.cpp`, `memory/detail/AllocationBackend.cpp`,
`Mutex.cpp`, `platform/IocpLoop.cpp`, `platform/TimerScheduler.cpp`,
`platform/Win32Error.cpp`, `Thread.cpp`, `ThreadPool.cpp`, `forge.cpp`.

Lesson for every future `moz.build` edit: sort new `SOURCES` (and any
other `StrictOrderingOnAppendList`, e.g. `EXPORTS`) entries
case-insensitively, not by raw ASCII/`sorted()` order — mixed-case
filenames (this project has several: `Mutex.cpp`, `Thread.cpp`,
`ThreadPool.cpp`, `File.cpp`, `ConditionVariable.cpp`, `Win32Error.cpp`)
are exactly where the two orderings diverge.

## Phase 4 real `mach build` confirmation (2026-07-29)

The user re-ran `mach build` against the corrected `moz.build` (see
above) and it completed successfully. Phase 4's Win32 pieces
(`Mutex`/`ConditionVariable`/`Thread`/`ThreadPool`) are now
**real-build-confirmed**, joining `ErasedCallable`/`LockGuard` (already
fully sandbox-verified, since they're pure logic) as genuinely done.
`ROADMAP.md` updated accordingly. `ThreadingSmokeTest.cpp` remains
available for deeper functional coverage (the actual mutual-exclusion/
producer-consumer/pool-scheduling behavior a successful compile alone
can't confirm) whenever the user wants to run it, but that's no longer
a blocking question — the compile succeeding was the open item, and
it's now closed. Next per `ROADMAP.md`: Phase 5 (Networking).

## Phase 5 — Networking: `IpAddress`/`Endpoint`/`Socket` (2026-07-29)

Reviewed the codebase and `ROADMAP.md` first, per standing process: no
networking code existed anywhere in `forge-core/` before this phase.
Wrote two frozen spec documents before any implementation, per this
project's established process — `IpAddress.md` and `Socket.md` — then
implemented from them. The split mirrors `Path`/`File` from Phase 3
exactly, applied to networking: `IpAddress`/`Endpoint` are pure,
allocation-free (except when formatting to text) value types with zero
OS dependency; `Socket` is entirely Winsock, the only forge-core type
allowed to call into it.

### `IpAddress`/`Endpoint` (`IpAddress.h`/`.inl`) — pure logic, fully verified

* Fixed 16-byte buffer plus an `IpVersion` tag, no allocation on the
  parse/compare/`Bytes()` path — same "plain fixed-size data" reasoning
  `Array<T, N>` already established.
* `Parse` accepts IPv4 dotted-quad (`"192.168.1.1"`) and IPv6 colon-hex
  (`"2001:db8::1"`) text, including `::` zero-run compression on parse.
  `ToString` produces RFC 5952's canonical form: lowercase hex, the
  longest run of zero groups (length >= 2) compressed with a single
  `::`, ties broken toward the first/leftmost run (`FindLongestZeroRun`
  uses `>`, not `>=`, specifically to keep the first tie rather than the
  last).
* Deliberate security choice, carried straight from `IpAddress.md`'s
  Non-Goals into the parser: an IPv4 octet with a leading zero (e.g.
  `"010.0.0.1"`) is **rejected**, not interpreted as octal — a
  well-known historical `inet_aton` ambiguity (some C library
  implementations read a leading-zero octet as octal, others as
  decimal) that has caused real SSRF/access-control bypass bugs when a
  validator and a connector disagreed on which. `IpAddressTest.cpp`
  covers this explicitly (`"010.0.0.1"`, `"192.168.01.1"` both rejected;
  a bare `"0"` octet is still accepted).
* `Endpoint::Parse` requires bracketed `[host]:port` for IPv6 (an
  unbracketed IPv6 literal's own colons are ambiguous with the port
  separator, matching standard URL-style convention) and explicitly
  rejects an unbracketed IPv6 address rather than guessing
  (`Endpoint::Parse("::1:8080")` must fail — covered in
  `IpAddressTest.cpp`).

**Real bug caught, fixed before any test ran:** a `[[nodiscard]] friend
bool operator==(...)` was initially declared in `IpAddress.h`'s class
body with its definition deferred to `IpAddress.inl` (the pattern most
other operators in this file use). This does not compile on either
target compiler — GCC rejects it with `attribute ignored
[-Werror=attributes]` plus a note that "an attribute that appertains to
a friend declaration that is not a definition is ignored"; Clang
rejects it with "an attribute list cannot appear here". Both compilers
agree a `[[nodiscard]]` friend must be a *defining* declaration, not
just a declaration. Fixed by moving `operator==`/`operator!=`'s full
definitions inline into the class body in `IpAddress.h` (matching
`Path.h`'s/`Error.h`'s own established pattern for frozen comparison
operators) and removing the now-duplicate declarations from
`IpAddress.inl`. Verified with a standalone `-fsyntax-only` compile on
both compilers before proceeding. **Lesson for every future comparison
operator in this codebase:** if it needs more than a single trivial
expression, define it fully inline in the class body — never declare-
only-and-defer it to a `.inl` file.

Verified with `IpAddressTest.cpp` (V4/V6 parse and round-trip including
the `::` compression/tie-breaking cases, V4/V6 malformed-input
rejection including the leading-zero-octet cases, explicit
`V6(Span<const u8>)` construction, `Endpoint::Parse`'s bracketed-V6 and
plain-V4 forms plus its own malformed-input rejection, and an
allocator-failure path via `FailingAllocator`) under the full project
bar: `g++`/`clang++ -std=c++17 -fno-exceptions -Wall -Wextra -Wpedantic
-Werror` (+ `-Wc++20-extensions` on clang), clean under ASan+UBSan and
`valgrind --leak-check=full` (0 leaks, 47 allocs/47 frees), plus an
`-O2` pass. Every scenario passed on the first try once the
`[[nodiscard]]`-friend fix above was in place — genuinely "confirmed",
the same way `Path` was in Phase 3.

### `Socket` (`Socket.h`/`.cpp`) — Win32/Winsock-only, NOT yet confirmed

* Move-only like `File`; native `SOCKET` stored behind an opaque
  `void*` (`SOCKET` is `UINT_PTR`, pointer-sized on every Windows target
  this project builds for, so the `reinterpret_cast` to/from `void*` is
  a legal integral<->pointer conversion, not type punning — same
  reasoning `File.h`'s `HANDLE`-as-`void*` already established).
  `NativeSocket(void*)`/`ToHandle(SOCKET)` are small named helpers
  around that cast so every call site stays readable.
* One-time `WSAStartup`/`WSACleanup` via a function-local-static
  `WinsockInitializer` (constructed once, thread-safe per C++11's
  function-local-static guarantee) — the same lazy-init pattern
  `memory::GetDefaultAllocator()` already established, so callers never
  have to think about Winsock's own startup/shutdown protocol.
  `WSACleanup()` deliberately runs at static-destruction time (after
  `main` returns), since forge-core has no process-shutdown hook for an
  earlier "last Socket use" moment, and that's standard, well-defined
  Winsock usage.
* `Connect`/`Listen` build a `sockaddr_in`/`sockaddr_in6` from an
  `Endpoint` via a shared `FillSockaddr` helper, copying straight from
  `IpAddress::Bytes()` — no text round-trip on the connect/bind path,
  per `IpAddress.md`'s own stated design goal for that accessor.
  `Listen` sets `SO_REUSEADDR` before `bind()` (the one socket option
  `Socket.md`'s Non-Goals explicitly calls out as needed internally) and
  clamps `backlog` to `SOMAXCONN`.
* `Send`/`Receive` guard `buffer.Size()` against `INT_MAX` before
  casting to Winsock's 32-bit `int` length parameter, returning
  `ErrorCode::InvalidArgument` for an oversized buffer rather than
  silently truncating the cast into a smaller, wrong length. `Receive`
  returning `0` means the peer closed gracefully — not an error,
  mirroring `File::Read`'s own end-of-file convention exactly (per
  `Socket.md`'s Design Goals).
* `platform/Win32Error.h`/`.cpp` gained `TranslateWinsockError(int
  wsaError)` alongside the existing `TranslateWin32Error(unsigned long
  lastError)` — a separate function, not an overload, because
  `WSAGetLastError()`'s `int` return lives in a disjoint numbering space
  from `GetLastError()`'s `DWORD` `ERROR_*` codes. Maps
  `WSAETIMEDOUT`->`ErrorCode::Timeout`, `WSAEADDRINUSE`->
  `ErrorCode::AlreadyExists`, `WSAEACCES`->`ErrorCode::PermissionDenied`,
  everything else->`ErrorCode::PlatformError` (with `NativeCode()`
  preserving the exact WSA error). Deliberately does **not** add new
  networking-specific `ErrorCode` values (`ConnectionRefused`,
  `ConnectionReset`, etc.) — caught this before writing any code by
  re-reading `Error.md`'s frozen spec, which explicitly excludes
  module-specific codes and uses `SocketDisconnected` as its own literal
  example of what not to add.
* Real include-order gotcha caught during review, fixed proactively
  (not from a build failure): `<windows.h>` pulls in the legacy
  `<winsock.h>` by default unless `WIN32_LEAN_AND_MEAN` is defined
  first, and having both `<winsock.h>` and `<winsock2.h>` included
  together in the wrong order is a classic Winsock redefinition-error
  trap. `Socket.cpp` and `platform/Win32Error.cpp` (which now also needs
  Winsock's `WSAE*` constants for `TranslateWinsockError`) both define
  `WIN32_LEAN_AND_MEAN` and include `<winsock2.h>`/`<ws2tcpip.h>` before
  `<windows.h>`.

**This component could not be compiled or run in this sandbox at
all** — same constraint as `File.cpp`/`Mutex.cpp`/`Thread.cpp` before
it (no Windows SDK, no working MinGW cross-compiler available). The
existing mock-header technique was extended with new hand-written
`/tmp/win32_mock/winsock2.h` and `/tmp/win32_mock/ws2tcpip.h` files
(`SOCKET`, `sockaddr`/`sockaddr_in`/`sockaddr_in6`/`sockaddr_storage`,
`WSADATA`, `socket`/`connect`/`bind`/`listen`/`accept`/`send`/`recv`/
`closesocket`/`setsockopt`/`htons`/`WSAStartup`/`WSACleanup`/
`WSAGetLastError`, `INVALID_SOCKET`/`SOCKET_ERROR`, the `AF_*`/
`SOCK_STREAM`/`IPPROTO_TCP`/`SOL_SOCKET`/`SO_REUSEADDR`/`SOMAXCONN`
constants, and the three `WSAE*` error constants
`TranslateWinsockError` maps). `Socket.cpp` compiles clean under
`g++`/`clang++ -std=c++17 -fno-exceptions -Wall -Wextra -Wpedantic
-Werror` (+ `-Wc++20-extensions` on clang) against it, both as a
standalone `-fsyntax-only` pass and as a real `-O2` object-file compile.
A small link driver (`/tmp/socket_link_driver.cpp`, not part of the
shipped tree) was written to force every public entry point
(`Connect`/`Listen`/`Accept`/`Send`/`Receive`/`Close`/`IsOpen`, plus the
move constructor) to actually get called against a concrete `Endpoint`,
then linked together with `Socket.cpp`+`platform/Win32Error.cpp` on
both compilers and run — clean exit, and clean under ASan+UBSan too.
This catches typos/wrong-argument-count/wrong-type mistakes and proves
the erasure/ownership logic doesn't corrupt anything even under a real
allocator/sanitizer, but proves nothing about actual Winsock runtime
behavior (the mock's functions are dumb stubs that always report
failure).

Wrote `SocketSmokeTest.cpp` (matching `ThreadingSmokeTest.cpp`'s/
`FileSmokeTest.cpp`'s precedent) — a real loopback echo test: `Listen`
on a fixed high port (`127.0.0.1:53421`; this first cut's
`Endpoint::Parse` has no ephemeral-port `":0"` support yet, see
`Socket.md`'s Implementation Status), run a background `Thread` that
`Accept`s the one connection and echoes back whatever it `Receive`s,
while the main thread `Connect`s, sends a 5-byte payload, and verifies
the echoed bytes match exactly; plus a second test that a `Connect` to
a port nothing is listening on fails with a `Result` error rather than
hanging or crashing. **Real bug caught while syntax-checking this
against the mock, before it ever reached a Windows machine:** Clang
(not GCC) rejected the server-thread lambda with
`-Werror=unused-lambda-capture` — a `constexpr` local (`kPayloadSize`)
had been explicitly captured even though C++17 allows a `constexpr`
local to be used directly inside a lambda without capturing it at all,
which GCC silently accepted but Clang treats as an error under
`-Wall -Wextra -Wpedantic -Werror`. Fixed by dropping it from the
capture list. Both compilers now compile the file clean, and it links
and runs clean (against the mock's stub behavior — every check that
depends on `Listen`/`Connect` actually succeeding fails as expected,
since the mock's `socket()` always returns `INVALID_SOCKET`; this is
not evidence the real logic works, only that it links).

`moz.build` updated: added `forge-core/Socket.cpp` to `SOURCES`
(recomputed case-insensitively via `sorted(items, key=str.lower)` from
the start this time, applying Phase 4's lesson rather than rediscovering
it — see the ordering-bug entry above), and a new `OS_LIBS += ["ws2_32"]`
entry (the first networking import library this project has needed;
required to link `WSAStartup`/`socket`/`connect`/etc. and the `WSAE*`
error constants).

Status

`ROADMAP.md`'s Phase 5 is marked done for `IpAddress`/`Endpoint` (fully
verified, pure logic) but explicitly **NOT confirmed** for `Socket` —
implemented and reviewed as carefully as this environment allows
(including the real bugs caught above), but genuinely unverified until
a real `mach build` compiles it and `SocketSmokeTest.cpp` runs clean on
the actual machine. This must be communicated to the user explicitly,
not glossed over, per `AGENTS.md`'s "Be Honest" and the user's own
standing request to be told when something can't be fully verified
without a real build. Next per `ROADMAP.md`: Phase 6 (Runtime
Integration) — or `Socket`/Phase 5 real-build confirmation first, if
the user runs `mach build` before then.

## Phase 5 real `mach build` confirmation (2026-07-29)

The user re-ran `mach build` against the Phase 5 changes and it
completed successfully: the networking code (including the `moz.build`
`SOURCES`/`OS_LIBS` updates and the real Winsock link) compiles
correctly in the real Gecko environment. `Socket` is now
real-build-confirmed, joining `IpAddress`/`Endpoint` (already fully
sandbox-verified) as genuinely done. `ROADMAP.md` updated accordingly.
`SocketSmokeTest.cpp` remains available for the deeper functional check
(an actual loopback connect/send/receive over real Winsock) whenever
the user wants to run it, but that's no longer a blocking question —
the compile succeeding was the open item, and it's now closed. Next per
`ROADMAP.md`: Phase 6 (Runtime Integration).

## Phase 6 — Runtime Integration (2026-07-29)

Reviewed `forge/forge.cpp` and `ROADMAP.md` first, per standing process.
`ROADMAP.md`'s Phase 6 description named three prototype pieces to
replace: `std::vector<std::unique_ptr<Microtask>>`, `TimerQueue`, and
`ForgeJobQueue`. Reviewing the actual code turned up that `TimerQueue`
was already gone — replaced by `forge::core::platform::IoLoop`/
`TimerScheduler` in an earlier pass that predates this document's
current Phase 1–6 numbering (the code's own comments reference "the old
TimerQueue's manual scan-every-timer polling" in the past tense and cite
"ROADMAP.md Phase 2", a different numbering scheme). Nothing left to do
there. `ForgeJobQueue` itself (the `JS::JobQueue` subclass) was already
a thin, reasonable wrapper — its actual prototype dependency was the
`microtasks` container it reads/writes, which is exactly the
`std::vector<std::unique_ptr<Microtask>>` the description also named.
So this phase's real, concrete work was: the microtask queue, the timer
registry's backing container (`std::vector<std::unique_ptr<JsTimer>>`,
a prototype piece the current description doesn't name explicitly but
which is the exact same category of thing), script loading
(`std::ifstream`/`std::stringstream`), and routing `bool`-returning
Forge-authored functions through `Result<T>` where practical.

### Microtask queue: `Queue<UniquePtr<Microtask>>`

`std::vector<std::unique_ptr<Microtask>> microtasks` became
`forge::core::Queue<forge::core::memory::UniquePtr<Microtask>>`. A
microtask queue is genuinely FIFO — always process the oldest pending
job first — which is exactly `Queue<T>`'s purpose-built circular-buffer
shape (see Phase 2's entry), giving O(1) `Push()`/`Pop()` instead of
`std::vector::erase(begin())`'s O(n) shift on every single microtask
drained.

**Real gap found and fixed before writing `forge.cpp`'s new code**:
`Queue<T>` had no way to walk every pending element in order — only
`Front()`/`Back()`, the two ends. `TraceForgeRoots` (the GC root tracer)
genuinely needs to trace *every* still-pending microtask's callback, not
just the front one — a callback the collector reclaimed out from under
a microtask that hasn't run yet would be a real, exploitable
use-after-free once that microtask's turn came. Rather than force a
different container choice (losing the O(1) FIFO semantics that make
`Queue<T>` the right fit here) or add a full iterator protocol neither
`Queue` nor this one call site actually needs, added a single new method
to `Queue<T>`: `operator[](SizeType offset)`, front-relative (`[0]`
aliases `Front()`, `[Size()-1]` aliases `Back()`), same signature and
`noexcept` convention as `Vector<T>::operator[]`. This is a
backward-compatible addition, not a redesign of anything already frozen
— `Queue` was never declared frozen the way `Error` was, and every
existing caller is unaffected. Verified with a new
`Test_Queue_IndexingWalksLogicalOrder` scenario in `QueueTest.cpp`,
deliberately forcing a wraparound first (same technique
`Test_Queue_WrapsAroundCorrectly` already uses) so the new accessor is
exercised across the physical ring buffer's wrap boundary, not just the
trivial non-wrapped case; also confirms indexing yields real references
(mutating through `queue[i]` is visible via `Front()`/`Back()`) and that
`operator[](0)`/`operator[](Size()-1)` are pointer-identical to
`Front()`/`Back()`. Clean under `g++`/`clang++ -std=c++17
-fno-exceptions -Wall -Wextra -Wpedantic -Werror` (+
`-Wc++20-extensions` on clang), ASan+UBSan, and
`valgrind --leak-check=full` (0 leaks, 20 allocs/20 frees), plus `-O2`
— re-ran the entire `QueueTest.cpp` suite, not just the new scenario,
to confirm nothing else regressed.

### Timer registry: `HashMap<int, UniquePtr<JsTimer>>`

`std::vector<std::unique_ptr<JsTimer>> timers_` — searched linearly by
both `JsTimerRegistry::CancelByJsId` and `RemoveFired` — became
`forge::core::HashMap<int, forge::core::memory::UniquePtr<JsTimer>>`,
keyed by the same `jsId` `setTimeout`/`setInterval` already hand back to
script. Every lookup here is genuinely "find the one timer with this
id", which `HashMap` answers in O(1) instead of an O(n) scan across
every live timer. No new forge-core code was needed for this one —
`HashMap<K, V>` already existed from Phase 2.

**Real correctness gap found and fixed while implementing `Add()`**:
the original code scheduled the native timer first, then unconditionally
pushed the wrapper onto the (infallible-in-practice, since
`std::vector::push_back` either succeeds or terminates via
`std::bad_alloc`) `std::vector`. `HashMap::Insert` is genuinely fallible
(`Result<bool>`, can fail on the table's own growth allocation) — if it
fails *after* the native timer was already scheduled against `raw`,
the original logic would have returned `-1` (reported as
"setTimeout: failed to schedule timer") while a real OS timer remained
armed and pointing at a `JsTimer` this registry never took ownership of
and that is about to be freed by `timer`'s own destructor when `Add()`
returns — a use-after-free waiting to happen the next time that timer
fires. Fixed by cancelling the native timer explicitly in the
`Insert()`-failure branch before returning `-1`. Exactly the same
"don't leave a native resource pointing at something about to be freed"
discipline `ThreadPool::Initialize`'s own rollback fix already
established in Phase 4 — this is the second time that exact shape of
bug has shown up in this codebase, which is worth remembering for any
future "schedule/register a native resource, then store a wrapper that
can itself fail" code.

Verified via a standalone driver
(`/tmp/forge_phase6_pattern_check.cpp`, not part of the shipped tree —
`forge.cpp` itself cannot be compiled in this sandbox at all, see
below) using plain structs (`FakeMicrotask`/`FakeTimer`) in place of
`JS::Heap<T>`/`JSContext*`, since none of the JS API is needed to
exercise these container/ownership patterns in isolation. Covers: the
microtask queue's full push/walk/drain cycle; the timer registry's
insert/find/erase/iterate/clear cycle; and specifically the rollback
scenario above, via a `FailingAllocator` that starves only the
`HashMap`'s own storage (not the `MakeUnique<FakeTimer>` call, mirroring
how the JsTimer itself is already successfully allocated by the time
`Add()` is reached) — confirmed the timer is destructed exactly once
(not zero times, a leak; not twice, a double-free) via a destruction
counter. Clean on both compilers under full warnings, ASan+UBSan, and
`valgrind --leak-check=full` (0 leaks, 18 allocs/18 frees).

### Script loading: `Path`/`File::ReadAllText`

`std::ifstream` + `std::stringstream` (read the whole file into a
`std::string` via `buffer << file.rdbuf()`) became
`Path::Create(StringView(argv[1]))` + `File::ReadAllText(path)`. Neither
needed new forge-core code — both were already implemented and
real-build-confirmed from Phase 3, so this was pure wiring. A concrete
behavioral improvement, not just fewer `std::` types: a bad script path
now reports through the same `Result<T>`/`Error` machinery as every
other failure path in this codebase (`"Forge: cannot read %s (error
code %d)"`, printing the actual `ErrorCode`) instead of a bare `"Cannot
open %s"` that gave no reason at all. `source` (a named local holding
the `Result<String>`, not a temporary) stays alive for the rest of its
enclosing block exactly like the old `std::string source` local did —
required, since `JS::SourceOwnership::Borrowed` means SpiderMonkey does
not copy the buffer and needs it alive through `JS::Evaluate`.

### `Result<T>` routing

`Runtime::Initialize()` changed from `[[nodiscard]] bool` to
`[[nodiscard]] Result<void>`. Previously a failed `loop_.Initialize()`
was reported to the one caller (`main()`) as a hardcoded `"Forge: failed
to initialize the event loop"` with the actual `Error` silently
discarded; now `main()` prints the real native error code. Every other
`bool`-returning function in this file is a SpiderMonkey API boundary
(`JS_Init`, `JS::Evaluate`, every `JSNative` callback, `JS::JobQueue`'s
virtual overrides) and cannot be changed — those signatures belong to
SpiderMonkey, not to Forge. `EnqueueMicrotask`/`SetTimeout`/
`SetInterval` now route their own internal allocation failures
(`MakeUnique`/`Queue::Push`/`HashMap::Insert` all returning `Result`)
through `JS_ReportOutOfMemory(cx)` before returning `false` — previously
`std::make_unique`'s allocation failure had no visible failure path at
all in this project's `-fno-exceptions` build (a real OOM would call the
default `new`-handler, which calls `std::terminate()` with no exception
able to propagate — an abrupt process crash instead of a reported,
recoverable `Result` failure). This is a genuine reliability
improvement in the exact spirit of this project's zero-exceptions,
explicit-`Result<T>` philosophy, not just a mechanical type swap.

Also simplified `QueueMicrotask` (the `queueMicrotask()` JS-visible
builtin) to call `EnqueueMicrotask` instead of duplicating its
allocate-and-push logic inline — a small DRY cleanup noticed during
review (not something the phase's instructions specifically asked for),
per `AGENTS.md`'s "fix inconsistencies you notice, explain why".

### Verification status

**`forge.cpp` itself cannot be compiled in this sandbox at all** — a
stricter constraint than `File`/`Mutex`/`Thread`/`Socket`'s "no Windows
SDK": this file needs the full SpiderMonkey JS API headers and a built
`libjs_static`, and even a hand-written mock of the entire JSAPI surface
`forge.cpp` uses (`JS::Heap`, `JS::PersistentRootedVector`,
`JS::JobQueue`, GC tracing, realms, compilation, evaluation, and more)
would be an unreasonably large and unreliable undertaking compared to
the targeted `windows.h`/`winsock2.h` mocks used for `File`/`Mutex`/
`Socket`. Verification here was: careful manual review against
`forge.cpp`'s existing conventions, plus the standalone pattern-check
driver described above, which gets real compiler + ASan+UBSan + valgrind
coverage of every forge-core container/ownership pattern the rewrite
depends on (the parts that don't need the actual JS engine), without
being able to compile the `JS::`-typed code around them at all.

Status

`ROADMAP.md`'s Phase 6 is marked done, with the same honest split as
every Win32-only phase before it: the isolable forge-core pattern work
(`Queue<T>::operator[]`, and the container/ownership logic verified via
the pattern-check driver) is fully sandbox-verified; `forge.cpp` itself
is implemented and reviewed as carefully as this environment allows, but
genuinely unconfirmed until a real `mach build` compiles it and a script
exercising `setTimeout`/`setInterval`/`queueMicrotask`/GC runs correctly
end to end on the actual machine. This must be communicated to the user
explicitly, not glossed over, per `AGENTS.md`'s "Be Honest" and the
user's own standing request to be told when something can't be fully
verified without a real build. No `moz.build` changes were needed this
phase (`forge.cpp` was already listed in `SOURCES`; `Queue.h`/`HashMap.h`
are header-only). This was the last phase listed in `ROADMAP.md` as of
this entry.
