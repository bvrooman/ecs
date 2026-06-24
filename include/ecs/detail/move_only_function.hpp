// ecs/detail/move_only_function.hpp
//
// Portability shim for std::move_only_function (C++23, P0288).
//
// Schedule stores each bound system as a move-only, type-erased callable so a
// system may capture a move-only value. libstdc++ (GCC 12+) ships
// std::move_only_function, so native builds alias straight to it with no change
// in behaviour. libc++ -- including the LLVM 21 build Emscripten bundles -- does
// not implement it yet (not even under -fexperimental-library), so for that
// toolchain we provide a minimal heap-erased equivalent supporting exactly what
// the engine needs: construct from a callable, move, and invoke.
//
// Selected automatically by the __cpp_lib_move_only_function feature macro; the
// public type is ecs::detail::move_only_function regardless of which backend is
// active, so call sites never branch.

#pragma once

#include <version>

#if defined(__cpp_lib_move_only_function) && __cpp_lib_move_only_function >= 202110L

#include <functional>
namespace ecs::detail {
using std::move_only_function;
} // namespace ecs::detail

#else

#include <functional> // std::invoke
#include <memory>
#include <type_traits>
#include <utility>

namespace ecs::detail {

template <class Signature>
class move_only_function; // primary template intentionally undefined

// Minimal move-only, type-erased callable for a plain (non-cv, non-ref) call
// signature R(Args...). Heap-allocates the target; that allocation happens once
// per system at schedule-build time, never on the per-tick hot path.
template <class R, class... Args>
class move_only_function<R(Args...)> {
public:
    move_only_function() noexcept = default;
    move_only_function(std::nullptr_t) noexcept {}

    template <class F,
              class DF = std::decay_t<F>,
              class    = std::enable_if_t<!std::is_same_v<DF, move_only_function>
                                          && std::is_invocable_r_v<R, DF&, Args...>>>
    move_only_function(F&& f)
        : target_(std::make_unique<Model<DF>>(std::forward<F>(f))) {}

    move_only_function(move_only_function&&) noexcept            = default;
    move_only_function& operator=(move_only_function&&) noexcept = default;
    move_only_function(move_only_function const&)                = delete;
    move_only_function& operator=(move_only_function const&)     = delete;

    move_only_function& operator=(std::nullptr_t) noexcept {
        target_.reset();
        return *this;
    }

    explicit operator bool() const noexcept { return target_ != nullptr; }

    R operator()(Args... args) {
        return target_->call(std::forward<Args>(args)...);
    }

private:
    struct Concept {
        virtual ~Concept()      = default;
        virtual R call(Args...) = 0;
    };
    template <class F>
    struct Model final : Concept {
        F f;
        explicit Model(F&& fn) : f(std::move(fn)) {}
        explicit Model(F const& fn) : f(fn) {}
        R call(Args... args) override {
            return std::invoke(f, std::forward<Args>(args)...);
        }
    };
    std::unique_ptr<Concept> target_;
};

} // namespace ecs::detail

#endif
