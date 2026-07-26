// ecs/schedule/params/local.hpp
//
// Local<T>: per-SYSTEM state that persists across ticks, for imperative
// (add/add_once) systems -- plus the imperative-parameter protocol behind it,
// the serial-side mirror of the kernel_param protocol in protocol.hpp.
//
// An imperative system runs as ONE opaque work item, so a single private T
// per system is race-free by construction. A kernel system's items run
// concurrently and their count varies with the world, so "the system's one T"
// has no safe, deterministic meaning there: Local is rejected in add_kernel
// (per-ITEM state in a kernel is Scratch<T>; state that must merge is
// Reduce/Collect).

#pragma once

#include <ecs/schedule/params/protocol.hpp>
#include <cstdint>
#include <span>

namespace ecs {

namespace detail {
    template <class P>
    struct imperative_param; // defined below; forward for friending
}

// Per-system state across ticks: `*state` / `state->field` reaches this
// SYSTEM's private T, default-constructed when the system is registered and
// persisting until it is removed (an add_once system's Local dies with it).
// No declared access, no cross-system sharing -- the moral equivalent of a
// `static` local in the system body, without the global it would imply
// (each registered system gets its own T, even from the same callable).
template <class T>
class Local {
public:
    [[nodiscard]]
    T& operator*() const noexcept {
        return *state_;
    }
    T* operator->() const noexcept { return state_; }

private:
    template <class P>
    friend struct detail::imperative_param;
    explicit Local(T& state) noexcept
        : state_(&state) {}
    T* state_;
};

namespace detail {

    template <class T>
    struct imperative_param<Local<T>> {
        static constexpr bool allowed = true;
        struct state {
            T value {};
        };
        static void declare(SystemAccess&) {} // private state: no access
        static void prepare(state&,
                            World&,
                            std::span<std::uint32_t const>,
                            KernelWaveContext const&) {}
        static void finish(state&, World&) {}
        static Local<T> bind(state& s, World&, Commands&, WorkItem const&) {
            return Local<T>(s.value);
        }
    };

} // namespace detail
} // namespace ecs
