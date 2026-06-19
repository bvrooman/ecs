// ecs/resource.hpp
//
// Resources are singletons that don't belong to any entity: the renderer, the
// audio engine, the physics world, the frame clock, command buffers, event
// queues, config. Exactly one instance of a given type lives in the registry
// and systems borrow it by reference.
//
// Resources are identified the same way components are (a per-type id), so the
// scheduler can analyse read/write access to resources with the same machinery
// it uses for components. A system's resource access is derived from its
// Res<T> (read) and ResMut<T> (write) parameters (see schedule.hpp).
//
// Unlike components, resources are stored as single type-erased values rather
// than columns, and they may be move-only (a GPU device, an audio stream), so
// the registry never requires copyability.

#pragma once

#include <cstdint>
#include <memory>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace ecs {

using ResourceId = std::uint32_t;

namespace detail {
inline ResourceId next_resource_id() noexcept {
  static ResourceId counter = 0;
  return counter++;
}
} // namespace detail

// Stable, process-local id assigned to each resource type on first use. This is
// a separate id space from component_id; a component and a resource may share a
// numeric id without conflict because they are tracked independently.
template <class T>
inline const ResourceId resource_id = detail::next_resource_id();

class ResourceRegistry {
public:
  // Construct (or replace) the resource of type T in place and return it.
  template <class T, class... Args>
  T& emplace(Args&&... args) {
    auto holder = std::make_unique<Holder<T>>(std::forward<Args>(args)...);
    T& ref = holder->value;
    map_[resource_id<T>] = std::move(holder);
    return ref;
  }

  template <class T>
  bool contains() const {
    return map_.contains(resource_id<T>);
  }

  // Access the resource. Precondition: contains<T>() (debug-checked via at()).
  // One definition serves const and mutable callers; the returned reference's
  // constness follows that of the registry.
  template <class T, class Self>
  auto& get(this Self&& self) {
    using H = std::conditional_t<
        std::is_const_v<std::remove_reference_t<Self>>, const Holder<T>,
        Holder<T>>;
    return static_cast<H&>(*self.map_.at(resource_id<T>)).value;
  }

  // Pointer form: nullptr when absent.
  template <class T, class Self>
  auto* try_get(this Self&& self) {
    using H = std::conditional_t<
        std::is_const_v<std::remove_reference_t<Self>>, const Holder<T>,
        Holder<T>>;
    auto it = self.map_.find(resource_id<T>);
    return it == self.map_.end() ? nullptr
                                 : &static_cast<H&>(*it->second).value;
  }

  template <class T>
  void remove() {
    map_.erase(resource_id<T>);
  }

private:
  struct Base {
    virtual ~Base() = default;
  };
  template <class T>
  struct Holder final : Base {
    T value;
    // Parenthesized init constructs aggregates and non-aggregates alike
    // (C++20 P0960) and never requires T to be copyable.
    template <class... Args>
    explicit Holder(Args&&... args) : value(std::forward<Args>(args)...) {}
  };

  std::unordered_map<ResourceId, std::unique_ptr<Base>> map_;
};

} // namespace ecs
