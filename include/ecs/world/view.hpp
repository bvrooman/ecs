// ecs/world/view.hpp
//
// WorldView: the read-only system parameter over a World. A façade like
// Commands (deferred writes) and Res/ResMut (typed resources) -- every method
// forwards straight to World.
//
// It lived in query.hpp until the Query-as-pure-iterator refactor, because it
// used to expose a query() returning a Query<Cs const...>. That method is gone
// -- ad-hoc iteration now delegates to World's own count/for_each/for_each_chunk
// -- and with it the reason to sit next to Query.

#pragma once

#include <ecs/entity.hpp>
#include <ecs/world/world.hpp>

#include <cstddef>
#include <type_traits>

namespace ecs {

// A read-only view of the world for systems that need ad-hoc reads (size, get,
// has, alive, iteration) beyond what Query/Res express. Everything it exposes
// is read-only -- count/for_each/for_each_chunk force every component to const,
// so nothing can be written through it. As a system parameter it therefore
// declares "reads everything": it runs concurrently with other readers but is
// serialized against any writer (unlike a raw World&, which is fully
// exclusive).
class WorldView {
public:
    explicit WorldView(World& world)
        : world_(&world) {}

    [[nodiscard]]
    auto size() const {
        return world_->size();
    }
    [[nodiscard]]
    auto alive(Entity const e) const {
        return world_->alive(e);
    }
    template <class C>
    [[nodiscard]]
    auto has(Entity const e) const {
        return world_->has<C>(e);
    }
    template <class C>
    auto get(Entity const e) const {
        return world_->get<C>(e);
    }
    // Read-only iteration: delegates to World's ad-hoc read-only iteration
    // (which forces every component const, so nothing can be written through it).
    template <class... Cs>
    [[nodiscard]]
    std::size_t count() const {
        return world_->count<std::remove_const_t<Cs>...>();
    }
    template <class... Cs, class F>
    void for_each(F&& fn) const {
        world_->for_each<std::remove_const_t<Cs>...>(fn);
    }
    template <class... Cs, class F>
    void for_each_chunk(F&& fn) const {
        world_->for_each_chunk<std::remove_const_t<Cs>...>(fn);
    }

private:
    World* world_;
};

} // namespace ecs
