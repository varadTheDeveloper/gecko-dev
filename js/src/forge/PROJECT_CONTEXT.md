# Forge Core

## Source of truth

`C:\spidermonkey-dev\gecko-dev\js\src\forge\` is the **only** tracked
copy of this project as of 2026-07-29 — it's the real Gecko/SpiderMonkey
tree `mach build` compiles from. `C:\forge-beta\forge-runtime-beta\`
(the original working copy) is retired; do not develop in or sync
changes to it unless explicitly asked to. This file, `HISTORY.md`,
`ROADMAP.md`, and `AGENTS.md` all now live directly under
`js\src\forge\` (not `.claude\`, which rejects remote writes) —
alongside `forge-core/`, `forge.cpp`, and `moz.build`.

## Vision

Forge Core is a modern C++ systems library being built as the foundation of
the Forge JavaScript Runtime. Written to compile as C++17, not C++20 — the
real build (`mach build` inside the Gecko/SpiderMonkey tree) rejects
C++20-only constructs; see `AGENTS.md` → "Coding Standards" and
`HISTORY.md` → "Real build environment discovered" for what that ruled
out and why.

The goal is to build a production-quality runtime that can eventually
compete with Node.js, Deno, and Bun.

Forge Core is the low-level platform library that provides memory,
containers, filesystem, threading, synchronization, networking and platform
abstractions. The runtime itself (`forge/forge.cpp`, `forge/main.cpp`) embeds
SpiderMonkey and is expected to be rebuilt on top of Forge Core over time,
rather than using raw `std::` containers and the event-loop code currently
prototyped there.

Everything must be production quality.

---

# Philosophy

- Zero exceptions
- Explicit error handling
- Cross-platform
- RAII
- UTF-8 internally
- Small, clean API
- No unnecessary dependencies
- Performance first
- Readability over cleverness

---

# Development Rules

Every component follows this workflow.

1. Design API
2. Review API
3. Freeze API
4. Implement in small parts
5. Review implementation
6. Freeze implementation

Once frozen, APIs are NOT redesigned except for:

- Bugs
- Security issues
- Incorrect behaviour
- Fundamental design flaws

Never redesign frozen components. See `HISTORY.md` for what is already
frozen before touching any existing header. See `AGENTS.md` for how an AI
agent specifically should approach a task in this repo (review checklist,
"be honest about compiling", the include/naming pitfalls already hit
once).

---

# Error Handling

Constructors never fail.

Functions that may fail return `Result<T>` (or `ResultVoid` for functions
with no value to return).

Objects requiring allocation during creation use a static factory:

```
Create(...)
```

Example: `String::Create(...)` instead of `String(...)`.

`Error` is a small value type (`ErrorCode` + native platform code), storing
no message, no stack trace, no formatting. It never allocates and never
throws. See `forge/forge-core/Error.md` for the full frozen spec and
`forge/forge-core/Error.h` for the implementation.

`Failure` exists purely to disambiguate constructing a failed `Result<T>`
from a `Result<T>` that legitimately holds an `Error` as its value type.

---

# Memory

All allocations go through `memory::Allocator`.

Never use:

- `new`
- `delete`
- `malloc`
- `free`

Allocator contract:

- `Allocate(size, alignment)` returns `nullptr` on failure.
- `Deallocate(memory, size, alignment)` must receive the same size/alignment
  used during the matching `Allocate()` call.

`DefaultAllocator` is the current concrete allocator. `UniquePtr`/`MakeUnique`
and `Vector` are built on top of the `Allocator` interface, not on
`std::allocator`.

---

# Current Progress

Completed (API + implementation present in `forge/forge-core/`):

- Types (`Types.h` — fixed-width integer/float aliases, `Size`, `Offset`, `Byte`)
- Error (`Error.h`, spec in `Error.md`)
- Failure (`Failure.h`)
- Result / ResultVoid (`Result.h`, `Result.inl`, `ResultStorage.h`,
  `ResultVoid.h`, `ResultVoid.inl`, `ResultFwd.h`)
- Assert (`Assert.h`)
- Allocator / DefaultAllocator (`memory/Allocator.h`,
  `memory/DefaultAllocator.h/.cpp`, `memory/detail/AllocationBackend.h/.cpp`)
- UniquePtr / MakeUnique (`memory/UniquePtr.h/.inl`, `memory/MakeUnique.h/.inl`)
- Vector (`memory/Vector.h/.inl`)
- String / StringView / Span (`Span.h`, `StringView.h`, `String.h/.inl`)
- Array / Stack / Queue / Hash / HashMap / HashSet (`Array.h`, `Stack.h/.inl`,
  `Queue.h/.inl`, `Hash.h`, `HashMap.h/.inl`, `HashSet.h/.inl`)
- Path (`Path.h/.inl`, spec in `Path.md`) — fully verified, no OS dependency
- File (`File.h/.cpp`, spec in `File.md`) — **done, real-build-confirmed**.
  A real `mach build` on 2026-07-29 caught a genuine bug
  (`CreateDirectory` colliding with `<windows.h>`'s
  `CreateDirectory`→`CreateDirectoryA`/`W` macro), fixed by renaming the
  method to `MakeDirectory` — see `HISTORY.md`. A second real `mach
  build` afterward compiled clean, confirming the fix. Phase 3 is
  complete.
- Sync — Mutex / ConditionVariable / LockGuard (`Mutex.h/.cpp`,
  `ConditionVariable.h/.cpp`, `LockGuard.h`, spec in `Sync.md`) — **done,
  real-build-confirmed**. `LockGuard` fully verified (pure logic, no OS
  dependency); `Mutex`/`ConditionVariable` compiled clean on a real
  `mach build` on 2026-07-29 (after fixing a `moz.build` `SOURCES`
  case-insensitive-ordering bug — see `HISTORY.md`).
- Thread / ThreadPool / ErasedCallable (`Thread.h/.inl/.cpp`,
  `ThreadPool.h/.inl/.cpp`, `ErasedCallable.h`, spec in `Thread.md`) —
  **done, real-build-confirmed**, same `mach build` pass as
  `Mutex`/`ConditionVariable` above. `ThreadingSmokeTest.cpp` remains
  available for deeper functional coverage beyond a successful compile,
  whenever the user wants it.
- IpAddress / Endpoint (`IpAddress.h/.inl`, spec in `IpAddress.md`) —
  fully verified, no OS dependency, same bar as `Path`.
- Socket (`Socket.h/.cpp`, spec in `Socket.md`) — **done,
  real-build-confirmed**. Could not be compiled in this sandbox (no
  Windows SDK / working MinGW available), verified by manual review
  plus a mock-`<winsock2.h>`/`<ws2tcpip.h>` compile+link pass; a real
  `mach build` on 2026-07-29 then confirmed it compiles and links
  against real Winsock headers. `SocketSmokeTest.cpp` remains available
  for the deeper functional check (a real loopback connect/send/receive)
  whenever the user wants to run it.
- Runtime Integration (`forge/forge.cpp`'s microtask queue, timer
  registry, and script loading) — **portable pieces done and fully
  verified; `forge.cpp` itself implemented but not yet confirmed**.
  `std::vector<std::unique_ptr<Microtask>>` → `Queue<UniquePtr<Microtask>>`;
  `std::vector<std::unique_ptr<JsTimer>>` → `HashMap<int, UniquePtr<JsTimer>>`
  keyed by `jsId`; `std::ifstream`/`std::stringstream` script loading →
  `Path`/`File::ReadAllText`; `Runtime::Initialize()` routed through
  `Result<void>` instead of `bool`. `Queue<T>` gained a new
  `operator[](offset)` (front-relative indexed access, needed for GC
  root tracing to walk every pending microtask, not just the front one)
  — fully sandbox-verified. `forge.cpp` itself cannot be compiled in
  this sandbox at all (needs the full SpiderMonkey JS API + a built
  `libjs_static`, unlike `File`/`Mutex`/`Socket`'s "just" missing a
  Windows SDK); verified instead by manual review plus a standalone
  driver exercising every forge-core container/ownership pattern the
  rewrite depends on (see `HISTORY.md`'s Phase 6 entry for the two real
  bugs this caught: a GC-root-tracing gap in `Queue<T>`, and a
  use-after-free-on-OOM rollback gap in the timer registry's `Add()`).
  Needs a real `mach build` before this can say "verified" the way
  everything else here can.

All of the above compiled cleanly and passed a runtime test (including
allocator-failure paths) as of 2026-07-26 — see `HISTORY.md` for the list
of bugs that pass fixed. Before that date this had never actually been
compiled end-to-end. String/StringView/Span were added and verified
2026-07-27, Array/Stack/Queue/Hash/HashMap/HashSet 2026-07-29, Path/File
2026-07-29 (File real-build-confirmed the same day after a rename fix),
Sync/Thread/ThreadPool 2026-07-29 (real-build-confirmed the same day,
after fixing a `moz.build` case-insensitive-ordering bug),
IpAddress/Endpoint/Socket 2026-07-29 (IpAddress/Endpoint fully verified;
Socket real-build-confirmed the same day), and Runtime Integration
2026-07-29 (portable pieces fully verified; `forge.cpp` itself pending a
real `mach build`), all under the corrected C++17/no-exceptions
constraints (see `AGENTS.md`) rather than the earlier, wrong C++20
assumption.

`forge/platform/` (Windows path handling groundwork) existed earlier in
the project but was deliberately removed; the filesystem layer (Phase 3)
was designed from scratch rather than building on it, starting from two
frozen spec docs (`Path.md`, `File.md`) per this phase's own requirement.
See `HISTORY.md`.

`ROADMAP.md`'s Phase 6 (Runtime Integration) was the last phase listed
as of 2026-07-29; nothing is currently "not started".

See `ROADMAP.md` for the intended order of any future work, and
`HISTORY.md` for decisions already frozen on the completed components.

---

# Coding Style

- Header-only implementations use `.inl`
- Public API in `.h`
- Inline function bodies in `.inl`
- `constexpr` whenever possible
- `noexcept` whenever possible
- `[[nodiscard]]` where appropriate
- Namespace: `forge::core` for core library types
- Includes: relative, directory-correct paths (`"Types.h"`, `"../Error.h"`,
  `"memory/ResultFwd.h"`) — never a `forge/core/...`-style rooted path.
  The physical directory is `forge/forge-core/` (hyphenated), and relative
  includes work regardless of how the surrounding build's include roots
  are configured. See `HISTORY.md` for why this matters (most of the
  codebase used the wrong convention until the 2026-07-26 pass).
- If a member function's name matches a type visible in the enclosing
  namespace (e.g. `Error()` returning `Error&`, `Size()` returning `Size`),
  GCC hard-errors (`-Wchanges-meaning`) rather than just warning. Fix with
  the `class Error` elaborated-type-specifier for real class types, or by
  fully qualifying the alias target (`forge::core::Size`) when the type is
  a `using`-alias. Do not rename the accessor to work around this — see
  `AGENTS.md` for the full explanation.

Component specs (like `Error.md`) follow this shape: Purpose, Responsibilities,
Non-Goals, Design Goals, Public API, Memory Layout, Ownership, Error Handling
Policy, Thread Safety, Dependencies, Extensibility, Acceptance Criteria,
Implementation Status. Write a matching spec doc before freezing any new
component's API.

---

# Long-Term Goal

Forge Core is not intended to be merely another container library.

It is the complete systems foundation for the Forge JavaScript Runtime.

Every design decision should consider:

- Performance
- Maintainability
- Explicit APIs
- Consistency
- Future runtime requirements
