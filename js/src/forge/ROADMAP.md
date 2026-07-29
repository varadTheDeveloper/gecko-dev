# Forge Core Roadmap

Order of remaining work. Do not start a later phase's component ahead of an
earlier one unless a specific runtime need pulls it forward — note the
reason in `HISTORY.md` if that happens.

Completed components (Types, Error, Failure, Result/ResultVoid, Assert,
Allocator/DefaultAllocator, UniquePtr/MakeUnique, Vector) are out of scope
here — see `PROJECT_CONTEXT.md` → Current Progress and `HISTORY.md` for
their status.

---

## Phase 1 — Core containers, continued — DONE (2026-07-27)

- String — done
- StringView — done
- Span — done

String follows the same rule as everything else: constructors never
allocate; allocation-requiring construction goes through `String::Create(...)`.
UTF-8 internally per the project philosophy.

Implemented in `forge/forge-core/Span.h`, `StringView.h`, `String.h`/
`String.inl`, mirroring `Vector<T>`'s own growth/reserve/copy-truncates-
on-OOM conventions exactly, but always null-terminated for C interop.
Verified with `StringTest.cpp` (13 scenarios, including allocator-failure
paths via a `FailingAllocator`) under the real project constraints —
`g++`/`clang++ -std=c++17 -fno-exceptions -Wall -Wextra -Wpedantic
-Werror` (plus `-Wc++20-extensions` on clang) — and clean under
ASan+UBSan and `valgrind --leak-check=full`. See `HISTORY.md` for the two
real bugs this verification pass caught (a missing `Result.h` include,
and a hidden-friend/ADL gap in `String`'s comparison operators) before
this could be called done. Not yet retested via a real `mach build` —
these are pure header/`.inl` files with no SpiderMonkey dependency, so
the sandbox verification here is materially more trustworthy than the
`forge.cpp`/event-loop work was, but per `AGENTS.md` this still isn't
"confirmed" until it's gone through the actual build.

## Phase 2 — Collections — DONE (2026-07-29)

- Array — done
- HashMap — done
- HashSet — done
- Queue — done
- Stack — done

Implemented in `forge/forge-core/Array.h`, `Hash.h`, `Stack.h`/`.inl`,
`Queue.h`/`.inl`, `HashMap.h`/`.inl`, `HashSet.h`/`.inl`. Design notes:

- `Array<T, N>` is a plain aggregate (public `T data_[N]`) rather than
  following every other container's private-members-plus-accessors
  convention — unlike Vector/String/Queue/HashMap, a fixed-size array has
  no size/capacity invariant to protect, so there's nothing an accessor
  would be guarding. `N == 0` is a separate specialization (a zero-length
  C array isn't valid C++).
- `Stack<T>` is a thin LIFO adapter directly over `Vector<T>` — no new
  storage logic, just a restricted API (push/pop/top only) that documents
  intent at the call site.
- `Queue<T>` is a real circular buffer (not Vector-backed) — a
  Vector-backed FIFO would need an O(n) shift on every pop. Same
  Allocator/Reserve/copy-truncates-on-OOM conventions as Vector<T>, plus
  a `head_` index and modulo-based wraparound.
- `Hash<T>` (new, `Hash.h`) is the trait `HashMap`/`HashSet` hash keys
  through. The primary template is deliberately left undefined — a key
  type without an explicit specialization fails to compile rather than
  silently hashing raw bytes (including padding) or failing with a
  confusing error deep inside the table. Integer specializations run
  through a splitmix64-style finalizer before masking to a table index,
  so sequential keys (0, 1, 2, ...) don't cluster under the power-of-two
  table sizes below.
- `HashMap<K, V>` is open addressing with linear probing and tombstones
  for deletion — the classic, well-understood design over something
  cleverer (Robin Hood / SwissTable-style), per PROJECT_CONTEXT.md's
  "readability over cleverness". Table size is always a power of two
  (masking, not modulo, for the index); grows (doubles) once
  `size + tombstones` crosses 75% of capacity, counting tombstones
  because they cost probe length exactly like real entries do. No custom
  hasher/key-equality template parameter and no const iteration yet —
  both are easy to add later behind a real use case; left out to keep
  the first version's surface small.
- `HashSet<K>` is a thin composition wrapper around
  `HashMap<K, detail::Unit>`, not a second independent table
  implementation — any future fix to HashMap's probing/growth logic
  applies to HashSet for free.

Verified with five new test files (`ArrayTest.cpp`, `StackTest.cpp`,
`QueueTest.cpp`, `HashMapTest.cpp`, `HashSetTest.cpp` — 34 scenarios
total, including a 1000-entry HashMap growth/erase/rehash stress test and
allocator-failure paths via `FailingAllocator`) under the same real
project constraints as Phase 1: `g++`/`clang++ -std=c++17 -fno-exceptions
-Wall -Wextra -Wpedantic -Werror` (plus `-Wc++20-extensions` on clang),
clean under ASan+UBSan and `valgrind --leak-check=full` (0 leaks, 0
errors across all five), plus an `-O2` pass. Same caveat as Phase 1: not
yet retested via a real `mach build` — pure header/`.inl` files, no
SpiderMonkey dependency, so this sandbox verification is materially more
trustworthy than the `forge.cpp`/event-loop work's was, but per
`AGENTS.md` that's still not "confirmed" until it's gone through the
actual build.

## Phase 3 — Filesystem — DONE and real-build-confirmed (2026-07-29)

- `Path.md` (spec) — done
- `File.md` (spec) — done
- `Path` (`Path.h`/`.inl`) — done, fully verified
- `File` (`File.h`/`.cpp`) — done, **real-build-confirmed**. The
  `CreateDirectory`→`MakeDirectory` rename (see below and `HISTORY.md`)
  compiled clean on the user's real `mach build`; Phase 3 is complete.

The earlier `forge/platform/` groundwork was deliberately removed (see
`HISTORY.md`) — this phase started from a clean slate. Two frozen spec
docs were written first, per this phase's own requirement:
`Path.md` (pure, portable path manipulation — no OS calls) and
`File.md` (actual filesystem I/O — Win32-only, synchronous). The split
mirrors `StringView`/`String`: one type never touches the OS, the other
is entirely the OS.

`Path` is pure `String`/`StringView` manipulation (Join, Parent,
FileName, Stem, Extension, Normalize's `.`/`..`/repeated-separator
handling, IsAbsolute for Windows drive-letter/UNC roots) with zero OS
dependency, so it got the exact same verification treatment as every
other Phase 1/2 container: `g++`/`clang++ -std=c++17 -fno-exceptions
-Wall -Wextra -Wpedantic -Werror` (+ `-Wc++20-extensions` on clang),
clean under ASan+UBSan and `valgrind --leak-check=full`, plus `-O2`.
Added `StringView::RFind` (last-occurrence search) as a backward-
compatible extension — needed for finding the final path separator,
and StringView was never declared frozen the way `Error` was.

`File` (`Open`/`Read`/`Write`/`Seek`/`Tell`/`SizeInBytes`/`Close`, plus
static `Exists`/`MakeDirectory`/`CreateDirectories`/`Remove`/
`ReadAllBytes`/`ReadAllText`) is Win32-only and synchronous by design —
see `File.md`'s Non-Goals for why async is deliberately out of scope for
now. **This component could not be compiled or run in the sandbox this
work was done in at all** — there is no Windows SDK, and no MinGW
cross-compiler could be installed (this sandbox's network access is
allowlisted to package registries, not the Ubuntu `universe` component
MinGW ships in). Verification here was: careful manual review against
the real Win32 API shape, plus a hand-written, type-check-only mock
`windows.h` (same technique used for `IocpLoop` last phase) that let a
real compiler at least confirm `File.cpp` compiles AND links cleanly
against realistic Win32 function signatures under the full `-std=c++17
-fno-exceptions -Wall -Wextra -Wpedantic -Werror` bar — this catches
typos/wrong-argument-count/wrong-type mistakes, but proves nothing about
actual runtime behavior (the mock's functions are dumb stubs).

The user then ran a real `mach build`, which is exactly the kind of
verification the mock couldn't provide, and it caught a real bug the
mock missed entirely: the original method name `File::CreateDirectory`
collided with `<windows.h>`'s `fileapi.h` macro
(`#define CreateDirectory CreateDirectoryA`), a classic Win32
ANSI/Wide dispatch macro that silently rewrites any symbol with that
exact name regardless of namespace/class scope. Fixed by renaming to
`File::MakeDirectory` everywhere (header, implementation, the internal
call site in `CreateDirectories()`, and `File.md`). See `HISTORY.md`
for the full build error and reasoning. The user then re-ran `mach
build` and it completed successfully — the fix, and the `moz.build`
`File.cpp` SOURCES entry, are now real-build-confirmed. Phase 3 is
done. `FileSmokeTest.cpp` (matching `IocpSmokeTest.cpp`'s precedent) is
still available to build and run on the real machine whenever the user
wants the deeper functional coverage (round-tripping content through
`Open`/`Write`/`Read`, `Append`, `Seek`/`Tell`, `CreateNew`/`NotFound`
error paths) beyond what a successful compile alone confirms, but this
is no longer blocking — the compile succeeding was the open question.

## Phase 4 — Threading — DONE and real-build-confirmed (2026-07-29)

- `Sync.md` (spec: Mutex, ConditionVariable, LockGuard<T>) — done
- `Thread.md` (spec: Thread, ThreadPool, ErasedCallable) — done
- `ErasedCallable`/`LockGuard<T>` — done, fully verified (pure logic, no
  OS dependency)
- `Mutex`/`ConditionVariable`/`Thread`/`ThreadPool` — implemented,
  **NOT yet confirmed** (see below)

Reviewed the codebase first: no threading/synchronization code existed
anywhere in `forge-core/` before this phase, so there was nothing to
duplicate. Two frozen specs were written before implementation, per
this project's established process: `Sync.md` and `Thread.md`.

`Mutex` wraps a Win32 `SRWLOCK`, `ConditionVariable` wraps a
`CONDITION_VARIABLE`, both stored behind an opaque `void*` in their
headers (both are documented, ABI-stable, one-pointer-sized values
valid when zero-initialized) so `<windows.h>` stays confined to their
`.cpp` files, mirroring `File.h`'s `HANDLE`-as-`void*` precedent.
Neither can fail to construct or lock, so only `Wait`/`WaitFor` return
`Result` — everything else is `void`. `LockGuard<Lockable>` is a
template on the lockable type specifically so its RAII logic could be
verified against a fake test double in this sandbox even though `Mutex`
itself cannot be compiled here — same Path/File-style split as Phase 3.

`Thread` wraps `CreateThread`, move-only like `File`. Every `Thread`
that was ever successfully created must be `Join()`'d or `Detach()`'d
before destruction/move-assignment, enforced with `FORGE_ASSERT`
(matching `std::thread`'s contract) rather than silently
auto-detaching/auto-joining, which would hide a real bug instead of
surfacing it.

`ErasedCallable` (new, `ErasedCallable.h`) is the type-erasure helper
`Thread::Create`/`ThreadPool::Submit` both use to accept an arbitrary
`Callable` and hand it across an OS boundary that only accepts a fixed
`void(*)(void*)` shape — pure C++, zero OS dependency, so like `Path`
and `LockGuard` it got full sandbox verification (`ErasedCallableTest.cpp`:
invoke-runs-and-frees, destroy-without-invoking, a move-only capture, an
OOM path via `FailingAllocator` — clean under both compilers'
`-std=c++17 -fno-exceptions -Wall -Wextra -Wpedantic -Werror`,
ASan+UBSan, `valgrind --leak-check=full`, `-O2`).

`ThreadPool` is a fixed-size worker pool over a `Queue<Task>` guarded by
a `Mutex`+`ConditionVariable`. Deliberately **non-movable**, following
`IocpLoop`'s "default-construct, then `Initialize()`" shape rather than
a `Create()` factory returning the pool by value — every worker's loop
captures a pointer back to the pool, so it needs a stable address from
the moment the first worker spawns, which a value-returning factory
requiring movability cannot safely provide. See `Thread.md`'s Design
Goals for the full reasoning; this was worked out *before* writing code
that would have needed to move a non-movable type through `Result<T>`.

Two real bugs were caught during self-review (see `HISTORY.md` for the
full detail): a friend-access mistake in an early `ConditionVariable.cpp`
draft (private access doesn't extend to free functions, only to the
actual friended class's own members), and a rollback-path deadlock risk
in `ThreadPool::Initialize` (joining an orphaned worker thread before
telling it to stop). Also extracted `platform::TranslateWin32Error` out
of `File.cpp` (which had its own private copy) into a shared
`platform/Win32Error.h`/`.cpp`, since `ConditionVariable.cpp`/`Thread.cpp`
needed the identical mapping and a third independent copy would have
been real duplication.

**This component could not be compiled or run in the sandbox this work
was done in at all** — same constraint as `File`/Phase 3. Verification
here was: careful manual review, plus the hand-written mock
`windows.h` (extended with `SRWLOCK`/`CONDITION_VARIABLE`/
`CreateThread`/`WaitForSingleObject` and friends) confirming every new
`.cpp` compiles AND links cleanly, including a small driver that forces
every template (`Thread::Create<Callable>`, `ThreadPool::Submit<Callable>`,
`MakeErasedCallable<Callable>`) to actually instantiate rather than just
compiling each file in isolation — that driver also ran clean under
ASan+UBSan and `valgrind --leak-check=full` (0 leaks), which is a real
signal about the erasure/cleanup logic even though the mock's
`CreateThread` is a dumb stub. **This must be verified on the real
machine before being called "done"** — `ThreadingSmokeTest.cpp`
(matching `FileSmokeTest.cpp`'s precedent) is ready to build and run
there: a `Mutex` mutual-exclusion stress test (8 threads × 20000
increments each), a `ConditionVariable` producer/consumer handoff and
`WaitFor` timeout check, and a `ThreadPool` test (4 workers, 500 tasks,
verifying every one ran exactly once, plus `Submit` after `Shutdown()`
correctly failing).

The first real `mach build` attempt against the Phase 4 `moz.build`
change didn't even reach the compiler — it failed parsing `moz.build`
itself with `mozbuild.util.UnsortedError`. Mozbuild's `SOURCES` list
requires case-insensitive sorted order; the new mixed-case entries
(`Mutex.cpp`, `Thread.cpp`, `ThreadPool.cpp`, `Win32Error.cpp`) had been
placed in raw-ASCII sorted order instead, which disagrees with
case-insensitive order exactly where an uppercase-initial filename
sits next to a lowercase-initial path (e.g. `Mutex.cpp` vs. `memory/...`).
Fixed by re-sorting the list case-insensitively — see `HISTORY.md` for
the full error and the corrected order. The user then re-ran a real
`mach build` against the corrected `moz.build`, and it completed
successfully: Phase 4's Win32 pieces (`Mutex`/`ConditionVariable`/
`Thread`/`ThreadPool`) are now real-build-confirmed, not just
mock-verified. `ThreadingSmokeTest.cpp` remains available for deeper
functional coverage beyond a successful compile whenever the user wants
it, but that is no longer a blocking open question.

## Phase 5 — Networking — DONE and real-build-confirmed (2026-07-29)

- `IpAddress.md` (spec: `IpAddress`, `Endpoint`) — done
- `Socket.md` (spec: `Socket`) — done
- `IpAddress`/`Endpoint` (`IpAddress.h`/`.inl`) — done, **fully verified**
  (pure logic, no OS dependency — same bar as `Path`)
- `Socket` (`Socket.h`/`.cpp`) — implemented, **NOT yet confirmed** (see
  below)

Reviewed the codebase and `ROADMAP.md` first: no networking code existed
anywhere in `forge-core/` before this phase. Two frozen specs were
written before implementation, per this project's established process:
`IpAddress.md` and `Socket.md`. The split mirrors `Path`/`File` from
Phase 3 exactly, applied to networking: `IpAddress`/`Endpoint` are pure,
allocation-free (except when formatting to text) value types with zero
OS dependency; `Socket` is entirely Winsock.

`IpAddress` stores a fixed 16-byte buffer plus an `IpVersion` tag — no
allocation on the parse/compare/`Bytes()` path, matching `Array<T, N>`'s
own "plain fixed-size data" precedent. `Parse` accepts IPv4 dotted-quad
and IPv6 colon-hex text (including `::` zero-run compression per
RFC 5952, ties broken toward the first/leftmost run); `ToString`
produces the RFC 5952 canonical form (lowercase hex, longest zero run
compressed). A deliberate security choice carried over from
`IpAddress.md`'s Non-Goals: an IPv4 octet with a leading zero (e.g.
`"010.0.0.1"`) is rejected outright rather than guessed at as octal or
decimal — a well-known historical `inet_aton` ambiguity that has caused
real SSRF/access-control bypass bugs when a validator and a connector
disagreed. `Endpoint` pairs an `IpAddress` with a port; `Endpoint::Parse`
requires bracketed `[host]:port` for IPv6 (an unbracketed IPv6 literal's
own colons are ambiguous with the port separator) and rejects the
unbracketed form rather than guessing. Verified with `IpAddressTest.cpp`
(covering both address families' parse/round-trip and malformed-input
paths, the leading-zero-octet rejection, and `Endpoint::Parse`'s
bracketed/plain forms) under the full project bar: `g++`/
`clang++ -std=c++17 -fno-exceptions -Wall -Wextra -Wpedantic -Werror`
(+ `-Wc++20-extensions` on clang), clean under ASan+UBSan and
`valgrind --leak-check=full` (0 leaks), plus an `-O2` pass — all
scenarios passed on the first try after one real syntax bug (see
`HISTORY.md`: a `[[nodiscard]]` friend declared-but-not-defined isn't
legal on either compiler) was caught and fixed before any test even ran.

`Socket` wraps Winsock (`socket`/`connect`/`bind`/`listen`/`accept`/
`send`/`recv`/`closesocket`), move-only like `File`, storing its native
`SOCKET` behind an opaque `void*` (`SOCKET` is `UINT_PTR`, pointer-sized
on every Windows target this project builds for, so the
`reinterpret_cast` to/from `void*` is a legal integral<->pointer
conversion — same reasoning `File.h`'s `HANDLE`-as-`void*` already
established). One-time `WSAStartup`/`WSACleanup` is handled internally
via a function-local-static `WinsockInitializer`, the same lazy-init
pattern `memory::GetDefaultAllocator()` already uses, so callers never
have to think about Winsock's own startup protocol. `platform/Win32Error.h`/
`.cpp` gained a second function, `TranslateWinsockError`, alongside the
existing `TranslateWin32Error` — Winsock's `WSAGetLastError()` returns a
plain `int` in its own disjoint numbering space, not a `GetLastError()`
`DWORD`, so this is a separate function rather than an overload.
Deliberately does **not** introduce new networking-specific `ErrorCode`
values (`ConnectionRefused`, `ConnectionReset`, etc.) — `Error.md`'s
frozen spec explicitly excludes module-specific codes, using
`SocketDisconnected` as its own literal example of what not to add, so
Winsock failures map onto the existing generic categories
(`ErrorCode::Timeout`/`AlreadyExists`/`PermissionDenied`) with
`ErrorCode::PlatformError` as the catch-all, `NativeCode()` still
carrying the exact WSA error. `Send`/`Receive` guard against a buffer
larger than `INT_MAX` (Winsock's length parameter is a 32-bit `int`)
before casting, mapping an oversized buffer to
`ErrorCode::InvalidArgument` rather than silently truncating the length.

**This component could not be compiled or run in the sandbox this work
was done in at all** — same constraint as `File`/Phase 3 and
`Mutex`/`ConditionVariable`/`Thread`/Phase 4 (no Windows SDK, no working
MinGW cross-compiler available). Verification here was: careful manual
review against the real Winsock API shape, plus a hand-written,
type-check-only mock `winsock2.h`/`ws2tcpip.h` (same technique as the
existing mock `windows.h`) that let real compilers confirm `Socket.cpp`
compiles AND links cleanly — both in isolation and linked together with
a small driver exercising every public entry point
(`Connect`/`Listen`/`Accept`/`Send`/`Receive`/`Close`/`IsOpen`, including
the move constructor) — under the full `-std=c++17 -fno-exceptions
-Wall -Wextra -Wpedantic -Werror` bar on both compilers (+
`-Wc++20-extensions` on clang), plus a clean ASan+UBSan pass over that
same driver. This catches typos/wrong-argument-count/wrong-type
mistakes, but proves nothing about actual runtime behavior (the mock's
functions are dumb stubs that always fail). **This must be verified on
the real machine before being called "done"** — `SocketSmokeTest.cpp`
is ready to build and run there: a real loopback echo test (listen on a
fixed high port, accept, echo back whatever is received, verified
byte-for-byte on the client side) run on a background `Thread` while
the main thread plays client, plus a connection-refused test against a
port nothing is listening on. Writing `SocketSmokeTest.cpp` against the
mock headers caught one more real bug before it ever reached a Windows
machine: Clang (not GCC) rejected an unused lambda capture of a
`constexpr` local under `-Werror=unused-lambda-capture` — see
`HISTORY.md`.

`moz.build`'s `SOURCES` list gained `forge-core/Socket.cpp`, and a new
`OS_LIBS += ["ws2_32"]` entry (the first networking import library this
project has needed) — both computed with the case-insensitive sort
lesson from Phase 4 applied from the start this time, not discovered via
a failed build.

The user then ran a real `mach build` against these changes, and it
completed successfully: the networking code (including the `moz.build`
updates and the Winsock link) compiles correctly in the real Gecko
environment. Phase 5 is done and real-build-confirmed —
`IpAddress`/`Endpoint` were already fully sandbox-verified; `Socket` is
now additionally confirmed to actually compile against real Winsock
headers, which the mock could only approximate. `SocketSmokeTest.cpp`
remains available for the deeper functional check (a real loopback
connect/send/receive) whenever the user wants to run it, but that is no
longer a blocking open question.

## Phase 6 — Runtime Integration — DONE, portable pieces fully verified (2026-07-29)

- Microtask queue (`std::vector<std::unique_ptr<Microtask>>` →
  `Queue<UniquePtr<Microtask>>`) — done
- Timer registry (`std::vector<std::unique_ptr<JsTimer>>` →
  `HashMap<int, UniquePtr<JsTimer>>`, keyed by `jsId`) — done
- Script loading (`std::ifstream`/`std::stringstream` → `Path`/
  `File::ReadAllText`) — done
- `Runtime::Initialize()` routed through `Result<void>` instead of `bool`
  — done
- `Queue<T>::operator[]` (new, front-relative indexed access) — done,
  fully verified

Reviewed `forge/forge.cpp` first, per standing process. Found that
`TimerQueue` (the third prototype piece this phase's original
description named) had already been replaced by
`forge::core::platform::IoLoop`/`TimerScheduler` in an earlier,
undocumented-under-the-current-phase-numbering pass — the code's own
comments reference "the old TimerQueue's manual scan-every-timer
polling" in the past tense and cite "ROADMAP.md Phase 2", a numbering
scheme that predates this document's current Phase 1–6 layout. Nothing
left to do there; this phase's real remaining work was the microtask
queue, the timer registry's backing container, and the raw `bool`/
`printf` error paths the description also called out.

`std::vector<std::unique_ptr<Microtask>> microtasks` became
`forge::core::Queue<forge::core::memory::UniquePtr<Microtask>>` — a
microtask queue is genuinely FIFO (always process the oldest pending
job first), which is exactly `Queue<T>`'s purpose-built shape (see
Phase 2), giving O(1) `Push()`/`Pop()` instead of
`std::vector::erase(begin())`'s O(n) shift on every single microtask
drained. `Queue<T>` had no way to walk every pending element in
order, though — only `Front()`/`Back()` — which `TraceForgeRoots` (GC
root tracing) genuinely needs: it must trace *every* still-pending
microtask's callback, not just the front one, or the collector could
reclaim a callback a later `Pop()` still needs to run. Added
`Queue<T>::operator[](offset)` (front-relative, same shape and
`noexcept` convention as `Vector<T>::operator[]`) rather than reaching
for a different container or a full iterator protocol neither `Queue`
nor this call site actually needs — a minimal, additive extension,
not a redesign of anything frozen. Fully sandbox-verified with a new
`Test_Queue_IndexingWalksLogicalOrder` scenario in `QueueTest.cpp`
(exercised across a wraparound, confirming logical front-to-back order
survives the physical ring buffer's wrap, and that indexing yields real
references, not copies) under the full project bar: `g++`/
`clang++ -std=c++17 -fno-exceptions -Wall -Wextra -Wpedantic -Werror`
(+ `-Wc++20-extensions` on clang), clean under ASan+UBSan and
`valgrind --leak-check=full` (0 leaks, 20 allocs/20 frees), plus `-O2`.

`std::vector<std::unique_ptr<JsTimer>> timers_` (searched linearly by
both `CancelByJsId` and `RemoveFired`) became
`forge::core::HashMap<int, forge::core::memory::UniquePtr<JsTimer>>`,
keyed by the same `jsId` script already sees from `setTimeout`/
`setInterval` — every lookup here is genuinely "find the one timer with
this id", which a `HashMap` answers in O(1) instead of an O(n) scan
(Phase 2 already built `HashMap<K, V>`; no new container code was
needed, just applying it). `JsTimerRegistry::Add()` gained an explicit
rollback path: if scheduling the native timer succeeds but storing the
wrapper in the `HashMap` then fails (OOM), the native timer is now
cancelled before returning failure — otherwise it would eventually fire
into a `JsTimer` the registry never actually took ownership of and was
about to free. Same "don't leave a native resource pointing at
something about to be freed" discipline as `ThreadPool::Initialize`'s
own rollback fix (see Phase 4's entry). Verified via a standalone
pattern-check driver (`/tmp/forge_phase6_pattern_check.cpp`, not part of
the shipped tree — `forge.cpp` itself cannot be compiled in this
sandbox at all, see below) using plain structs in place of
`JS::Heap<T>`/`JSContext*`, exercising the microtask queue's full
push/walk/drain cycle, the timer registry's insert/find/erase/iterate/
clear cycle, and specifically the `Insert()`-failure rollback path (via
a `FailingAllocator`, confirming the timer is freed exactly once — not
leaked, not double-freed) — clean on both compilers under full
warnings, ASan+UBSan, and `valgrind --leak-check=full` (0 leaks, 18
allocs/18 frees).

Script loading (`std::ifstream` + `std::stringstream` reading the whole
file into a `std::string`) became `Path::Create` + `File::ReadAllText`
— both already existed and were real-build-confirmed from Phase 3, so
this needed no new forge-core code, just wiring. A concrete win beyond
just "fewer `std::` types": a bad script path now reports through the
same `Result<T>`/`Error` machinery as everything else in this codebase
(`Forge: cannot read %s (error code %d)`) instead of a bare `"Cannot
open %s"` with no actual reason. `Runtime::Initialize()` was similarly
changed from `[[nodiscard]] bool` to `Result<void>`, so the one caller
in `main()` can now report the actual native error code from a failed
`loop_.Initialize()` instead of a hardcoded message with no detail.
`QueueMicrotask` (the `queueMicrotask()` JS-visible builtin) was also
simplified to call `EnqueueMicrotask` instead of duplicating its
allocate-and-push logic inline — a small DRY cleanup noticed during
review, not something the phase's instructions specifically asked for.

**`forge.cpp` itself cannot be compiled in this sandbox at all** — it
needs the full SpiderMonkey JS API headers and a built `libjs_static`,
neither of which exist here (this is a different, stricter constraint
than `File`/`Mutex`/`Socket`'s "no Windows SDK" — even a hand-written
mock of the entire JSAPI surface `forge.cpp` uses would be an
unreasonably large and unreliable undertaking). Every change was
therefore verified by: careful manual review against `forge.cpp`'s
existing conventions, and the standalone pattern-check driver above,
which gets real compiler/ASan/UBSan/valgrind coverage of every
forge-core container/ownership pattern the rewrite depends on, without
being able to touch the actual JS::-typed code around it. **This must
be verified on the real machine before being called fully "done"** — a
real `mach build` is the only thing that can confirm `forge.cpp` itself
(the `JS::`-typed glue code) still compiles and links, and that a
script exercising `setTimeout`/`setInterval`/`queueMicrotask`/GC still
runs correctly end to end.

No `moz.build` changes were needed this phase — `forge.cpp` was already
listed in `SOURCES`, and `Queue.h`/`HashMap.h` are header-only, no new
`.cpp` to add.
