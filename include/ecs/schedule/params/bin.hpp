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
#include <ranges>
#include <span>
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

        // The merge is memory-bound, so lanes only help once it is far too big
        // to stream from cache. Measured on a 4-core box: at 200k entries the
        // tiled path is a wash to 10% WORSE than the serial one; at 2M entries
        // into dense buckets it is 14% better. The threshold sits between,
        // deliberately conservative -- the serial path is the one the rewrite
        // made 20-35% faster, and it is what every ordinary workload takes.
        // Expect this to be pessimistic on a host with more lanes and more
        // memory channels; it is tuned on the only hardware available here.
        static constexpr std::size_t kMinParallelEntries = 1uz << 20;
        // ...and each tile should be substantial for the same reason.
        static constexpr std::size_t kMinTileEntries = 65536;
        // Tiling only pays when the buckets are DENSE. Tiles split each bucket
        // between them, so a tile's slice of a bucket must be worth a cache
        // line or the scatters ping-pong the same lines -- with a 4-byte value
        // that is 16 entries per (tile, bucket). Measured on 200k entries at 4
        // lanes: 30% faster into 64 buckets (3125 per bucket), 80% SLOWER into
        // 20000 (10 per bucket). So a sparse workload keeps the serial merge,
        // which the rewrite made ~25% faster anyway.
        static constexpr std::size_t entries_per_line() {
            return std::max<std::size_t>(1, 64 / sizeof(V));
        }
        // ...and require a healthy multiple of that before tiling at all.
        static constexpr std::size_t kDensityMargin = 4;

        struct state {
            slot_array<std::vector<Entry>> parts;
            // (logical start, partial index) for each NON-EMPTY partial: the
            // concatenation of the partials in ordinal order, which is the
            // sequence a serial walk visits and the order every bucket keeps.
            std::vector<std::pair<std::size_t, std::uint32_t>> runs;
            std::vector<std::size_t> bounds;   // tile boundaries in that sequence
            std::vector<std::uint32_t> hist;   // tiles x nb: counts, then cursors
            std::vector<std::uint32_t> maxima; // per-tile max bucket
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

        // Visit the logical range [lo, hi) of the concatenated partials in
        // order, calling f(bucket, value&). Tiles are contiguous ranges of this
        // sequence, so a tile's entries stay in serial-walk order.
        template <class F>
        static void walk(state& s, std::size_t const lo, std::size_t const hi, F&& f) {
            if (lo >= hi)
                return;
            auto it = std::upper_bound(s.runs.begin(),
                                       s.runs.end(),
                                       lo,
                                       [](std::size_t const v, auto const& r) {
                                           return v < r.first;
                                       });
            --it; // the run containing lo (runs[0] starts at 0)
            for (std::size_t pos = lo; it != s.runs.end() && pos < hi; ++it) {
                auto& vec              = s.parts[it->second];
                std::size_t const base = it->first;
                std::size_t const j1   = std::min(vec.size(), hi - base);
                for (std::size_t j = pos - base; j < j1; ++j)
                    f(vec[j].first, vec[j].second);
                pos = base + j1;
            }
        }

        // The merge: a counting sort of everything the items emitted, tiled so
        // its two O(entries) passes run on the pool. Only the prefix pass --
        // O(tiles x buckets), not O(entries) -- stays serial, and it is what
        // makes the result canonical: offsets are assigned bucket-major with
        // tiles in order, so each bucket's values land in serial-walk order
        // whatever the lane count. One tile IS the serial algorithm, so small
        // merges and 1-lane pools take the same code path.
        static void finish(state& s, World& w, parallel::WorkerPool& pool) {
            auto& target = w.resource<Bins<V>>();

            s.runs.clear();
            std::size_t total = 0;
            for (std::size_t i = 0; i < s.parts.active; ++i) {
                auto const n = s.parts[i].size();
                if (n == 0)
                    continue;
                s.runs.emplace_back(total, static_cast<std::uint32_t>(i));
                total += n;
            }
            if (total == 0) {
                target.offsets_.assign(target.min_buckets_ + 1, 0);
                target.items_.clear();
                return;
            }

            // How many tiles the entries could support, before the bucket count
            // is known. The bucket-dependent cap is applied below.
            std::size_t const want =
                pool.lanes() > 1 && total >= kMinParallelEntries
                    ? std::max<std::size_t>(1,
                                            std::min<std::size_t>(4 * pool.lanes(),
                                                                  total /
                                                                      kMinTileEntries))
                    : 1;
            std::size_t tiles = want;
            auto retile       = [&](std::size_t const nb) {
                auto const per_bucket = total / std::max<std::size_t>(nb, 1);
                tiles = per_bucket < kDensityMargin * entries_per_line()
                            ? 1 // too sparse to tile: the serial merge wins
                            : std::min(tiles,
                                       std::max<std::size_t>(1,
                                                             total / (entries_per_line() *
                                                                      nb)));
                s.bounds.resize(tiles + 1);
                for (std::size_t t = 0; t <= tiles; ++t)
                    s.bounds[t] = total * t / tiles;
            };

            // The bucket count. A reservation usually covers every key a system
            // emits (a fixed grid, one bucket per node), so take it on trust and
            // let the histogram below catch a key outside it -- that saves a
            // whole pass over the entries in the common case. Without a
            // reservation there is nothing to trust and the max must be scanned.
            std::size_t nb   = target.min_buckets_;
            bool scan_needed = nb == 0;
            for (;;) {
                if (scan_needed) {
                    retile(std::max<std::size_t>(nb, 1));
                    s.maxima.assign(tiles, 0);
                    pool.for_each_index(tiles, [&](std::size_t const t) {
                        std::uint32_t m = 0;
                        walk(s,
                             s.bounds[t],
                             s.bounds[t + 1],
                             [&](std::uint32_t const k, V&) { m = std::max(m, k); });
                        s.maxima[t] = m;
                    });
                    for (auto const m : s.maxima)
                        nb = std::max(nb, std::size_t {m} + 1);
                }
                tiles = want;
                retile(nb);
                s.hist.assign(tiles * nb, 0);
                s.maxima.assign(tiles, 0); // reused: 1 = this tile saw an overflow
                pool.for_each_index(tiles, [&](std::size_t const t) {
                    auto* const h = s.hist.data() + t * nb;
                    walk(s, s.bounds[t], s.bounds[t + 1], [&](std::uint32_t const k, V&) {
                        if (k < nb)
                            ++h[k];
                        else
                            s.maxima[t] = 1;
                    });
                });
                bool const overflow =
                    std::ranges::any_of(s.maxima,
                                        [](std::uint32_t const f) { return f; });
                if (!overflow)
                    break;
                scan_needed = true; // a key past the reservation: rescan and redo
            }

            // Counts -> start offsets, bucket-major with tiles in order.
            target.offsets_.assign(nb + 1, 0);
            std::size_t running = 0;
            for (std::size_t b = 0; b < nb; ++b) {
                target.offsets_[b] = running;
                for (std::size_t t = 0; t < tiles; ++t) {
                    auto& slot       = s.hist[t * nb + b];
                    auto const count = slot;
                    slot             = static_cast<std::uint32_t>(running);
                    running += count;
                }
            }
            target.offsets_[nb] = running;

            // Scatter: every tile writes into its own precomputed range, so no
            // two lanes touch the same slot and no atomics are needed.
            target.items_.resize(total);
            pool.for_each_index(tiles, [&](std::size_t const t) {
                auto* const cursor = s.hist.data() + t * nb;
                walk(s, s.bounds[t], s.bounds[t + 1], [&](std::uint32_t const k, V& v) {
                    target.items_[cursor[k]++] = std::move(v);
                });
            });
        }

        static Bin<V> bind(state& s, World&, Commands&, WorkItem const& item) {
            auto ordinal = item.ordinal;
            return Bin<V>(s.parts[ordinal]);
        }
    };

} // namespace detail
} // namespace ecs
