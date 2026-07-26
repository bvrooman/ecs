// ecs/detail/compat/atomic_shared_ptr.hpp
//
// Portability shim for std::atomic<std::shared_ptr<T>> (C++20, P0718).
//
// SnapshotChannel publishes an immutable shared_ptr<const T> through an atomic so
// any number of consumer threads can load the latest snapshot locklessly.
// libstdc++ implements the std::atomic<shared_ptr> specialization; libc++ --
// including the LLVM 21 build Emscripten bundles -- does not, so there
// std::atomic<shared_ptr> instead matches the primary template and hard-errors
// (shared_ptr is not trivially copyable). For that toolchain we substitute a
// mutex-guarded equivalent exposing the same load()/store() surface the channel
// uses. Under Emscripten the engine is single-threaded, so the lock is never
// contended; on any other libc++ it is a correct, if not lock-free, fallback.
//
// Selected automatically by the __cpp_lib_atomic_shared_ptr feature macro; the
// public type is ecs::detail::atomic_shared_ptr<T> in both cases.

#pragma once

#include <memory>
#include <version>

#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L

#include <atomic>
namespace ecs::detail {
template <class T>
using atomic_shared_ptr = std::atomic<std::shared_ptr<T>>;
} // namespace ecs::detail

#else

#include <atomic> // std::memory_order
#include <mutex>
#include <utility>

namespace ecs::detail {

// Mutex-guarded stand-in for std::atomic<std::shared_ptr<T>>; implements only the
// operations SnapshotChannel needs (default construct, load, store). The
// memory_order arguments are accepted for signature compatibility and ignored --
// the mutex already establishes the necessary happens-before ordering.
template <class T>
class atomic_shared_ptr {
public:
    atomic_shared_ptr() noexcept = default;
    atomic_shared_ptr(std::shared_ptr<T> desired) noexcept
        : value_(std::move(desired)) {}

    atomic_shared_ptr(atomic_shared_ptr const&)            = delete;
    atomic_shared_ptr& operator=(atomic_shared_ptr const&) = delete;

    std::shared_ptr<T> load(std::memory_order = std::memory_order_seq_cst) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return value_;
    }

    void store(std::shared_ptr<T> desired,
               std::memory_order = std::memory_order_seq_cst) {
        std::lock_guard<std::mutex> lock(mutex_);
        value_ = std::move(desired);
    }

private:
    mutable std::mutex mutex_;
    std::shared_ptr<T> value_;
};

} // namespace ecs::detail

#endif
