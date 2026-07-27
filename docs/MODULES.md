# C++ modules: feasibility findings

Results of an empirical spike (July 2026) into shipping ecs as C++20 modules.
Recorded because the conclusion turns on toolchain behaviour that is expensive
to rediscover and that will change: this is a snapshot, not a permanent verdict.
Supersedes the speculative parts of §5 in `IMPROVEMENTS.md`.

**Verdict: viable on Clang, blocked on GCC.** Not because of anything in this
library — the layout is already close to module-shaped — but because GCC's
module implementation cannot carry global-module-fragment declarations into
cross-translation-unit template instantiation. For a library that is templates
over standard-library types, that is fatal.

That matters more than it sounds, because P2996 static reflection currently
*requires* GCC 16 or the Bloomberg clang fork. The compiler the reflection
backend is built around is the one whose module support cannot carry the
library. The two features are in tension, and the tension is not ours to fix.

## The blocker

A module whose function body mentions a type from its global module fragment
(GMF), instantiated from an importing TU. Fifteen lines:

```cpp
// m.cppm
module;
#include <vector>                      // GMF only -- not exported
export module probe;
namespace probe::detail {
template <class T> consteval auto members() { return std::vector<int>(sizeof(T) / 4); }
template <class T> consteval std::size_t member_count() { return members<T>().size(); }
}
export namespace probe {
template <class T> inline constexpr std::size_t field_count_v = detail::member_count<T>();
}

// u.cpp -- zero textual includes
import probe;
struct Position { float x; float y; };
static_assert(probe::field_count_v<Position> == 2);
```

| compiler | result |
| --- | --- |
| clang-18 | builds |
| clang-20 | builds |
| gcc-14 (Linux) | fails — `no matching function for call to 'operator new(sizetype, void*)'`, and `std::_Construct@probe` shows the std entity attached to the module |
| gcc-16 (macOS, P2996) | fails — `couldn't look up 'std::vector'` |

The `@probe` suffix in GCC's diagnostics is the tell: the standard-library
entity has been attached to module `probe` rather than staying at global module
attachment, so the importing TU cannot complete the instantiation. The exact
message varies with what the template body touches — `operator new`,
`construct_at`, `operator-`, or an outright `couldn't look up` — but the `@`
marker is common to all of them.

Reproducing on gcc-14/Linux is the load-bearing part: it rules out macOS, the
SDK, reflection, and the spike harness. It is GCC.

The same root cause has a milder form that *is* workable: a consumer that
textually includes the std headers a module's templates mention will often
compile. That is not a contract a library can offer — it would make every
change to a template body a breaking change for users, and it collides with the
second GCC issue below.

## Second GCC issue: GMF vs textual include (macOS)

On Homebrew GCC 16 + the macOS SDK, the same declaration arriving both through
a module's GMF and textually produces:

```
conflicting imported declaration 'typedef union __mbstate_t __mbstate_t'
    MacOSX26.sdk/usr/include/arm/_types.h
```

Removing every textual std include from the consumer removes this error class
entirely. Overlapping GMF includes across two modules are fine on Linux with
both gcc-14 and clang-18, so this one is specific to GCC + the Apple SDK.

The documented escape from both issues is `import std;`, which removes GMFs
altogether. It did not work on gcc 16.1 + CMake 4.3.4 — CMake failed building
libstdc++'s own std module:

```
CMake Error: .../bits/std.cc.o is of type `CXX_MODULES` but does not provide
             a module interface unit or partition
```

It does work on clang-18 + libc++ + CMake 4.4 (verified, all probes green).

## The design that works

Verified end to end on clang-18 and clang-20 — build, install, `find_package`,
`import` from a fresh project.

**Separate top-level modules (`ecs.world`, `ecs.schedule`, …) under ONE CMake
target.** One target per module breaks downstream consumption: CMake 3.28
synthesizes a target per imported target but never wires their modmaps
together, so a consumer dies with `module 'ecs.core' not found` while compiling
the umbrella. Linking every module target explicitly does not help. A single
target owning all the `.cppm` files works, and consumers can still
`import ecs.world;` on its own — the subset-import property survives.

`target_compile_features(ecs PUBLIC cxx_std_23)` is **mandatory**. Without it
every downstream *configure* fails with "has C++ sources that use modules, but
does not include cxx_std_20"; `CMAKE_CXX_STANDARD` on the consumer does not
reach the synthesized target.

CMake installs the `.cppm` **source**, not the BMI, so an installed package
stays compiler-portable (gcc consumed a clang-built package successfully).

`ecs::detail` in a module that the umbrella does not re-export becomes
genuinely unreachable — a hard error rather than a naming convention. This is
the single largest win on offer.

An *interface* partition must be re-exported by the primary module interface
unit; GCC enforces this, Clang does not. Internals want *implementation*
partitions.

## Structure

All headers are self-contained, so the header graph is a DAG. Quotiented into
candidate module units, 20 of 22 are independently partitionable. Two
strongly-connected groups must share a unit:

- `{detail, chunk.hpp}` — trivial.
- `{schedule, schedule/params}` — internal to the scheduler and expected;
  `params.hpp` is already an implementation layout, not an API. The params
  policies genuinely need `system.hpp` (for the `SystemParam` / `ParallelParam`
  concepts and `SystemAccess`), verified by deleting the include and compiling.
  Breaking it would mean relocating the parameter-binding protocol.

A third group, `{query.hpp, schedule, schedule/params}`, was a real circular
dependency; it was removed by moving `work_item.hpp` to `detail/` (#52).

## Practical gotchas

- **GCC does not recognise a `.cppm` suffix as C++.** Without `-x c++` it
  silently treats the file as a linker input, writes no BMI, and the consumer
  later fails with `failed to read compiled module: No such file or directory`.
- **Module GMFs need stricter IWYU than headers.** A header picks up
  `<vector>` transitively; a module's GMF must name it.
- **`import std` is behind a CMake experimental gate** whose UUID changes every
  release (4.3.4 and 4.4 differ) and is published nowhere stable.
- **That gate propagates to consumers.** A package whose modules use
  `import std` forces every downstream project to set the same experimental
  variable before `project()`. For a library consumed via `find_package`, that
  is a real distribution cost, not an implementation detail.
- Modules require Ninja; the `dev`/`release` presets pin `Unix Makefiles`.
  `cmake_minimum_required` needs 3.24 → 3.28.
- `<meta>` is not part of `import std`, so a reflection module keeps a GMF for
  it regardless.

## The duplicated-counter hazard — confirmed, with a twist

Four inline function-local statics exist today:

| | |
| --- | --- |
| `strong_id.hpp:79` | `StrongId::next()` — mints component/resource ids |
| `command_buffer.hpp:55` | flush-attribution counter |
| `parallel/worker_pool.hpp:216` | `serial_pool()` |
| `dynamic/registry.hpp:96` | `dynamic::registry()` |

`component_id<T>` is an inline variable template whose initializer calls
`ComponentId::next()`, and `next()` holds the static. In a headers-only world
every copy has vague linkage and the linker merges them: one counter, one id
per type, process-wide. That is the invariant the archetype signatures rest on.

**Root cause of the risk: module attachment.** A declaration in a module's
*purview* is attached to that module ([basic.link]); the same declaration
reaching another TU textually is attached to the global module. Declarations
attached to different modules declare **different entities**, however identical
their spelling. A function-local `static` belongs to its enclosing function
entity — so two entities means two statics, and two counters. The global module
fragment exists precisely to keep textual includes at global attachment.

Measured, with a header of exactly this shape included in a module's purview,
one TU importing and one including:

| compiler | header in module **purview** | header in **GMF** |
| --- | --- | --- |
| gcc-14 | ids 0 and 0 — **two counters** | ids distinct — one counter |
| clang-18 | ids 0 and 0 — **two counters** | ids distinct — one counter |
| clang-20 | ids distinct — one counter | ids distinct — one counter |

So the hazard is real, and it is *version-dependent* — which is worse than a
consistent bug, because it works on clang-20 and silently corrupts on clang-18
and gcc-14. Colliding `ComponentId`s surface as wrong-column reads, not as a
compile or link error.

Note the trap this sets: GMF placement is what fixes the counter, and GMF
placement is exactly what GCC cannot carry into cross-TU template instantiation
(see the blocker above). On GCC the two problems pull in opposite directions.

The robust fix is neither: move these definitions into a single non-inline TU
owned by the module target, so there is one definition regardless of
attachment. Do it *before* any migration, not after — a staged "modules
alongside headers" rollout is precisely the configuration that triggers it. Pair
it with a test that links an importing TU and an including TU into one binary
and asserts the ids differ.

## Untested

P2996 reflection under clang (upstream clang has no `-freflection`; needs the
Bloomberg fork, not buildable in the environment available), Emscripten (CMake +
emcc module scanning is unsupported upstream; the wasm leg should stay on
headers), and the `config.hpp` / `ECS_USE_P2996` macro problem for importers
noted in `IMPROVEMENTS.md` §5.

Reflection *inside* a module purview does work: `std::meta` compiles in a
module interface unit on g++-16. Only the consumer-side instantiation failed,
for the GMF reason above.

## Re-checking

The blockers are all "not yet" rather than "never". Re-check when GCC ships a
std module that scans cleanly and CMake takes `import std` out of experimental.
The fifteen-line reproducer at the top of this document is the fastest signal:
if it builds on the GCC of the day, the central objection is gone.
