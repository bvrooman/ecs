// ecs/detail/handle.hpp
//
// A generational handle: {index, generation}, tagged by a phantom Tag type so
// handles from different containers are distinct types (Handle<EntityTag> cannot
// be passed where Handle<OtherTag> is expected). Handles are minted only by the
// matching HandleVector<Payload, Tag>; from_raw() is the sole exception, for FFI
// / deserialization boundaries.

#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>

namespace ecs::detail {
template <class Payload, class Tag>
class HandleVector;
template <class Payload, class Tag>
class SparseHandleVector;

template <class Tag>
class Handle {
public:
    static constexpr uint32_t invalid_index = std::numeric_limits<uint32_t>::max();

    auto index() const { return index_; }
    auto generation() const { return generation_; }

    static constexpr Handle null() noexcept {
        return Handle {invalid_index, invalid_index};
    }

    // Reconstruct a handle from raw fields. For FFI / deserialization boundaries
    // where an index+generation pair arrives from outside the process (a host
    // passing an entity id back in, say). Normal code obtains handles only from
    // a HandleVector; a raw pair is validated on use (is_alive/get) like any
    // other handle, so a bogus pair simply reads as dead.
    static constexpr Handle from_raw(uint32_t const index,
                                     uint32_t const generation) noexcept {
        return Handle {index, generation};
    }

    // True when this handle is not the null() sentinel.
    [[nodiscard]]
    explicit constexpr operator bool() const noexcept {
        return !(*this == null());
    }

    friend bool operator==(Handle, Handle)  = default;
    friend auto operator<=>(Handle, Handle) = default;

private:
    template <class, class>
    friend class HandleVector;
    template <class, class>
    friend class SparseHandleVector;

    constexpr Handle(uint32_t const index, uint32_t const generation)
        : index_(index)
        , generation_(generation) {}

    uint32_t index_;
    uint32_t generation_;
};
} // namespace ecs::detail

template <class Tag>
struct std::hash<ecs::detail::Handle<Tag>> {
    std::size_t operator()(ecs::detail::Handle<Tag> const h) const noexcept {
        // Pack into 64 bits first: std::size_t is 32-bit on ILP32 targets (e.g.
        // wasm32), where `generation << 32` would be undefined and drop the
        // generation. Narrow to size_t only for the bucket index.
        std::uint64_t const key =
            (static_cast<std::uint64_t>(h.generation()) << 32) ^ h.index();
        return static_cast<std::size_t>(key);
    }
};
