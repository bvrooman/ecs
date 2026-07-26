// ecs/world/res.hpp
//
// Res<T> / ResMut<T>: typed resource access for system parameters. Split out
// of world.hpp -- they are a system-parameter concern built on World's
// resource registry, not part of World itself.

#pragma once

#include <ecs/world/world.hpp>

namespace ecs {

// Typed resource access for system parameters: Res<T> is a read of resource T,
// ResMut<T> a write. They resolve the resource once at construction. The
// scheduler derives a system's declared resource access from these parameter
// types (see schedule.hpp), so the declaration cannot drift from actual use.
template <class T>
class Res {
public:
    explicit Res(World& w)
        : ptr_(&w.resource<T>()) {}
    T const& operator*() const noexcept { return *ptr_; }
    T const* operator->() const noexcept { return ptr_; }
    T const& get() const noexcept { return *ptr_; }

private:
    T const* ptr_;
};

template <class T>
class ResMut {
public:
    explicit ResMut(World& w)
        : ptr_(&w.resource<T>()) {}
    T& operator*() noexcept { return *ptr_; }
    T* operator->() noexcept { return ptr_; }
    T& get() noexcept { return *ptr_; }

private:
    T* ptr_;
};

} // namespace ecs
