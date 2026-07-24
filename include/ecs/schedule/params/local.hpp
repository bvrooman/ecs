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

#include "protocol.hpp"

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

    // The imperative-parameter protocol: how each parameter type of an
    // add()/add_once() system declares access, owns per-system state, and
    // binds for a run. The primary template covers every ordinary system
    // parameter (stateless, binds through system_param); Local<T> is the one
    // stateful face.
    template <class P>
    struct imperative_param {
        static constexpr bool allowed = SystemParam<P>;
        using state                   = no_state;
        static void declare(SystemAccess& a) { system_param<P>::declare(a); }
        static P bind(state&, World& w, Commands& c, WorkItem const& item) {
            return system_param<P>::bind(w, c, item);
        }
    };

    template <class T>
    struct imperative_param<Local<T>> {
        static constexpr bool allowed = true;
        struct state {
            T value {};
        };
        static void declare(SystemAccess&) {} // private state: no access
        static Local<T> bind(state& s, World&, Commands&) { return Local<T>(s.value); }
    };

    // Pack helpers over an imperative system's full parameter tuple.
    template <class Args>
    struct imperative_params_info;
    template <class... A>
    struct imperative_params_info<std::tuple<A...>> {
        static constexpr bool all_allowed = (imperative_param<A>::allowed && ...);
        static constexpr bool any_stateful =
            (!std::is_same_v<typename imperative_param<A>::state, no_state> || ...);
        using states = std::tuple<typename imperative_param<A>::state...>;
    };

    // Whole-parameter-list drivers, folded over the system's Args tuple
    // (plain function templates, like the kernel drivers in protocol.hpp).
    template <class Args, std::size_t... I>
    void imperative_declare(SystemAccess& a, std::index_sequence<I...>) {
        (imperative_param<std::tuple_element_t<I, Args>>::declare(a), ...);
    }
    template <class Args, class Fn, class States, std::size_t... I>
    void imperative_invoke(Fn& fn,
                           States& st,
                           World& w,
                           Commands& c,
                           WorkItem const& item,
                           std::index_sequence<I...>) {
        fn(imperative_param<std::tuple_element_t<I, Args>>::bind(std::get<I>(st),
                                                                 w,
                                                                 c,
                                                                 item)...);
    }

} // namespace detail
} // namespace ecs
