// ecs/schedule/params/bin.hpp
//
// Bin<V>: group-by for parallel systems -- emit (bucket, value) pairs during
// iteration, read back a Bins<V> resource of contiguous per-bucket spans.
// Each item appends into a private partial; the barrier counting-sorts the
// partials into the resource (count -> prefix-sum -> scatter, walking the
// partials in ordinal order), so within every bucket the values sit in
// canonical serial-walk order -- the same Bins at 1 lane and N. The classic
// use is spatial hashing: bucket = cell index, value = entity or position.

#pragma once

#include <ecs/schedule/params/parallel.hpp>
#include <ecs/schedule/params/protocol.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace ecs {

// The group-by target resource: values grouped by bucket index, each bucket a
// contiguous span, rebuilt every run by the Bin<V> parameter that declares a
// write on it. Bucket count = max emitted bucket + 1, floored at
// reserve_buckets() (so a fixed grid keeps its empty cells).
template <class V>
class Bins {
public:
    [[nodiscard]]
    std::size_t bucket_count() const noexcept {
        return offsets_.empty() ? 0 : offsets_.size() - 1;
    }
    [[nodiscard]]
    std::span<V const> bucket(std::size_t const b) const noexcept {
        return {items_.data() + offsets_[b], offsets_[b + 1] - offsets_[b]};
    }
    // Every value, grouped by bucket, buckets ascending.
    [[nodiscard]]
    std::span<V const> all() const noexcept {
        return {items_.data(), items_.size()};
    }
    // Guarantee at least n buckets exist (empty ones included) regardless of
    // the keys actually emitted -- set once for a fixed-size grid.
    void reserve_buckets(std::size_t const n) { min_buckets_ = n; }

private:
    template <class P>
    friend struct detail::parallel_param;
    std::vector<V> items_;
    std::vector<std::size_t> offsets_; // bucket b: items_[offsets_[b], offsets_[b+1])
    std::size_t min_buckets_ = 0;
};

// Parallel-side write face of Bins<V>: emits into THIS ITEM's private partial;
// the barrier counting-sorts all partials into the resource.
// Precondition, checked at prepare: the world owns a Bins<V> resource.
// Precondition, checked at the barrier: every emitted bucket is below
// UINT32_MAX. That one index needs a bucket COUNT (index + 1) that does not
// fit in 32 bits, so whether it could work would turn on the target's
// std::size_t -- it is not a bucket on any target. It is also the usual "no
// cell" sentinel, so emitting it is a missed skip: the run aborts with
// std::length_error, thrown by the merge before it sizes or indexes anything.
// Schedule::run() unwinds as for a throwing system (recorded edits discarded,
// TickAbort) and the Bins<V> is left empty.
template <class V>
class Bin {
public:
    void emit(std::uint32_t const bucket, V value) {
        partial_->emplace_back(bucket, std::move(value));
    }

private:
    template <class P>
    friend struct detail::parallel_param;
    using Entry = std::pair<std::uint32_t, V>;
    explicit Bin(std::vector<Entry>& partial) noexcept
        : partial_(&partial) {}
    std::vector<Entry>* partial_;
};

namespace detail {

    template <class V>
    struct parallel_param<Bin<V>> {
        static constexpr bool allowed = true;
        using Entry                   = std::pair<std::uint32_t, V>;
        // Bucket indices are below this: UINT32_MAX itself is not one (see Bin).
        static constexpr std::uint32_t kBucketLimit = ~std::uint32_t {0};
        struct state {
            slot_array<std::vector<Entry>> parts;
            std::vector<std::size_t> cursors; // scatter scratch, reused
        };

        static void declare(SystemAccess& a) {
            a.res_writes.push_back(resource_id<Bins<V>>);
        }

        static void prepare(state& s,
                            World& w,
                            std::span<std::uint32_t const> rows,
                            WaveContext const&) {
            s.parts.prepare(rows.size());
            // Rebuilt every run: reset now so an aborted wave leaves the
            // target empty rather than stale (same contract as Reduce).
            auto& target = w.resource<Bins<V>>();
            target.items_.clear();
            target.offsets_.clear();
        }

        static void finish(state& s, World& w) {
            auto& target = w.resource<Bins<V>>();
            // The largest emitted key sets the bucket count, so find it (and
            // the entry count) before any table is sized or indexed.
            std::uint32_t max_key = 0;
            std::size_t total     = 0;
            for (std::size_t i = 0; i < s.parts.active; ++i) {
                for (auto const& [k, v] : s.parts[i])
                    max_key = std::max(max_key, k);
                total += s.parts[i].size();
            }
            // Bin's bucket precondition. Checked here rather than in emit():
            // this scan reads every key anyway, so it costs one compare a run
            // instead of one a row, and it fails the same way at any lane
            // count. Nothing is touched yet -- prepare() emptied the target
            // and it stays empty.
            if (max_key >= kBucketLimit)
                throw std::length_error(
                    "Bin::emit: bucket index UINT32_MAX is out of range -- bucket "
                    "indices run 0 .. UINT32_MAX - 1 (an \"invalid\" sentinel "
                    "emitted as a bucket?); skip the row instead of emitting it");
            // Bucket count: max emitted key + 1, floored at the reservation.
            // max_key < UINT32_MAX, so the + 1 cannot wrap even where
            // std::size_t is 32 bits; with no entries there is no key to count.
            std::size_t nb = target.min_buckets_;
            if (total != 0)
                nb = std::max(nb, std::size_t {max_key} + 1);
            // offsets_ holds one entry more than there are buckets. Refuse a
            // count that leaves no room for it: past max_size() there is no
            // such table, and at SIZE_MAX -- reserve_buckets(~0uz), or bucket
            // UINT32_MAX - 1 where std::size_t is 32 bits -- nb + 1 would wrap
            // to 0 and the loops below would index an empty one.
            if (nb >= target.offsets_.max_size())
                throw std::length_error(
                    "Bins: the bucket count (largest emitted bucket + 1, floored "
                    "at reserve_buckets()) is more than an offset table can hold "
                    "on this target");
            // Count into offsets_[k + 1], then prefix-sum into start offsets.
            target.offsets_.assign(nb + 1, 0);
            for (std::size_t i = 0; i < s.parts.active; ++i)
                for (auto const& [k, v] : s.parts[i])
                    ++target.offsets_[std::size_t {k} + 1];
            for (std::size_t b = 1; b <= nb; ++b)
                target.offsets_[b] += target.offsets_[b - 1];
            // Scatter in ordinal order: stable, so each bucket's values sit
            // in canonical serial-walk order.
            target.items_.resize(total);
            s.cursors.assign(target.offsets_.begin(), target.offsets_.end() - 1);
            for (std::size_t i = 0; i < s.parts.active; ++i)
                for (auto& [k, v] : s.parts[i])
                    target.items_[s.cursors[k]++] = std::move(v);
        }

        static Bin<V> bind(state& s, World&, Commands&, WorkItem const& item) {
            auto ordinal = item.ordinal;
            return Bin<V>(s.parts[ordinal]);
        }
    };

} // namespace detail
} // namespace ecs
