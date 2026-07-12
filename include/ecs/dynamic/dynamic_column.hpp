// ecs/dynamic/dynamic_column.hpp
//
// DynamicColumn: the runtime, type-erased SoA column -- the dynamic twin of
// ecs::Column<T> / soa_storage<T>. It implements the same IColumn interface the
// archetype machinery already drives, so a JS-defined component slots into an
// Archetype exactly like a native one. Storage is one contiguous byte buffer per
// field (structure of arrays), so each field is a single contiguous run -- the
// thing a JS typed-array view aliases for per-chunk kernels in Phase B.

#pragma once

#include "../archetype.hpp" // ecs::IColumn
#include "component_desc.hpp"
#include <cassert>
#include <cstddef>
#include <cstring>
#include <memory>
#include <span>
#include <vector>

namespace ecs::dynamic {

class DynamicColumn final : public ecs::IColumn {
public:
    explicit DynamicColumn(ComponentDesc const& desc)
        : desc_(&desc)
        , fields_(desc.fields.size()) {}

    // --- IColumn ----------------------------------------------------------
    void reserve(std::size_t n) override {
        for (std::size_t i = 0; i < fields_.size(); ++i)
            fields_[i].reserve(n * desc_->fields[i].size);
    }

    std::size_t emplace_default() override {
        for (std::size_t i = 0; i < fields_.size(); ++i)
            fields_[i].resize(fields_[i].size() + desc_->fields[i].size, std::byte {0});
        return count_++;
    }

    std::size_t swap_remove(std::size_t row) override {
        assert(count_ > 0 && row < count_ && "DynamicColumn::swap_remove: bad row");
        auto const last = count_ - 1;
        for (std::size_t i = 0; i < fields_.size(); ++i) {
            auto const sz = desc_->fields[i].size;
            auto& b       = fields_[i];
            if (row != last)
                std::memcpy(b.data() + row * sz, b.data() + last * sz, sz);
            b.resize(last * sz);
        }
        --count_;
        return last;
    }

    void apply_permutation(std::span<std::uint32_t const> perm) override {
        assert(perm.size() == count_ &&
               "DynamicColumn::apply_permutation: perm size != row count");
        for (std::size_t i = 0; i < fields_.size(); ++i) {
            auto const sz = desc_->fields[i].size;
            auto& b       = fields_[i];
            std::vector<std::byte> next(b.size());
            for (std::size_t r = 0; r < perm.size(); ++r)
                std::memcpy(next.data() + r * sz, b.data() + perm[r] * sz, sz);
            b = std::move(next);
        }
    }

    void move_row_to(ecs::IColumn& dst_, std::size_t row) override {
        auto& dst = static_cast<DynamicColumn&>(dst_);
        for (std::size_t i = 0; i < fields_.size(); ++i) {
            auto const sz = desc_->fields[i].size;
            auto& src     = fields_[i];
            dst.fields_[i].insert(dst.fields_[i].end(),
                                  src.begin() + static_cast<std::ptrdiff_t>(row * sz),
                                  src.begin() +
                                      static_cast<std::ptrdiff_t>((row + 1) * sz));
        }
        ++dst.count_;
    }

    [[nodiscard]]
    std::unique_ptr<ecs::IColumn> clone_empty() const override {
        return std::make_unique<DynamicColumn>(*desc_);
    }

    [[nodiscard]]
    std::size_t size() const override {
        return count_;
    }

    // --- raw row access (used by the host binding and future runtime queries) -
    // Scatter a packed AoS blob (`stride` bytes) into the per-field buffers.
    void scatter(std::size_t row, void const* blob) {
        auto const* p = static_cast<std::byte const*>(blob);
        for (std::size_t i = 0; i < fields_.size(); ++i) {
            auto const& f = desc_->fields[i];
            std::memcpy(fields_[i].data() + row * f.size, p + f.offset, f.size);
        }
    }
    // Gather a row back into a packed AoS blob.
    void gather(std::size_t row, void* blob) const {
        auto* p = static_cast<std::byte*>(blob);
        for (std::size_t i = 0; i < fields_.size(); ++i) {
            auto const& f = desc_->fields[i];
            std::memcpy(p + f.offset, fields_[i].data() + row * f.size, f.size);
        }
    }
    // Append a row from a packed blob; returns its index.
    std::size_t push(void const* blob) {
        auto const row = emplace_default();
        scatter(row, blob);
        return row;
    }

    // Base of the I-th field's contiguous buffer -- what a typed-array view wraps.
    [[nodiscard]]
    void* field_base(std::size_t field_index) override {
        return fields_[field_index].data();
    }
    [[nodiscard]]
    ComponentDesc const& desc() const {
        return *desc_;
    }

private:
    ComponentDesc const* desc_;
    std::vector<std::vector<std::byte>> fields_; // one contiguous buffer per field
    std::size_t count_ = 0;
};

} // namespace ecs::dynamic
