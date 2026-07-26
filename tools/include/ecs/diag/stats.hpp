// ecs/diag/stats.hpp
//
// Header-only timing diagnostics: collect per-tick durations and periodically
// report the distribution -- mean, standard deviation, p50, p99, max. The
// distribution (not just the mean) is what reveals stutter: a low mean with a
// high p99/max means occasional hitches the eye reads as jank.
//
// No engine dependency (just <chrono>). Like the schedule visualizer it lives in
// tools/, not the core ecs headers, so the library carries no diagnostics code;
// examples and apps that want timing stats link ecs::diag and decide *where* the
// report goes (stderr, the browser console, a DOM HUD) and *when* it is on (an
// env var, a URL param, always).
//
//   ecs::diag::TickStats stats;                       // reports every ~2s
//   { auto _ = stats.time(); schedule.run(world); }   // RAII-time one tick
//   if (auto s = stats.due())                          // ~every 2s, else nullopt
//       std::fprintf(stderr, "%s\n", ecs::diag::TickStats::format(*s, "sim
//       tick").c_str());

#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ecs::diag {

// True if the named environment variable is set -- a convenience for native apps
// that gate diagnostics on e.g. ECS_STATS=1. (Meaningless in a browser build;
// gate on a URL param or a flag there.)
inline bool env_enabled(char const* name) { return std::getenv(name) != nullptr; }

class TickStats {
public:
    using clock = std::chrono::steady_clock;

    struct Summary {
        std::size_t n;                  // sample count
        double mean, sd, p50, p99, max; // all in the unit you sample (microseconds
                                        // by convention; see format()'s "us")
    };

    explicit TickStats(double report_every_s = 2.0)
        : period_(std::chrono::duration_cast<clock::duration>(
              std::chrono::duration<double>(report_every_s)))
        , last_report_(clock::now()) {}

    // Record one sample (microseconds, by convention -- the unit is yours, but
    // format() prints "us").
    void sample(double microseconds) { samples_.push_back(microseconds); }

    // RAII timer: samples the elapsed wall time (us) when the returned object is
    // destroyed. Use as `{ auto _ = stats.time(); work(); }`.
    class Scope {
    public:
        explicit Scope(TickStats& s)
            : owner_(&s)
            , t0_(clock::now()) {}
        Scope(Scope&& o) noexcept
            : owner_(o.owner_)
            , t0_(o.t0_) {
            o.owner_ = nullptr;
        }
        Scope(Scope const&)            = delete;
        Scope& operator=(Scope const&) = delete;
        Scope& operator=(Scope&&)      = delete;
        ~Scope() {
            if (owner_) {
                using duration = std::chrono::duration<double, std::micro>;
                owner_->sample(duration(clock::now() - t0_).count());
            }
        }

    private:
        TickStats* owner_;
        clock::time_point t0_;
    };
    [[nodiscard]]
    Scope time() {
        return Scope(*this);
    }

    // If at least report_every_s has elapsed since the last report (and there are
    // samples), reduce the samples to a Summary, clear them, and return it. Call
    // every tick; it is cheap (one clock read) when not yet due.
    std::optional<Summary> due() {
        auto const now = clock::now();
        if (now - last_report_ < period_ || samples_.empty())
            return std::nullopt;
        last_report_ = now;
        return flush();
    }

    // Reduce the collected samples to a Summary and clear them; nullopt if none.
    std::optional<Summary> flush() {
        if (samples_.empty())
            return std::nullopt;
        std::ranges::sort(samples_);
        double sum = 0;
        for (double x : samples_)
            sum += x;
        double const mean = sum / static_cast<double>(samples_.size());
        double var        = 0;
        for (double x : samples_)
            var += (x - mean) * (x - mean);
        auto const pct = [&](double p) {
            return samples_[std::min(
                samples_.size() - 1,
                static_cast<std::size_t>(p * static_cast<double>(samples_.size())))];
        };
        Summary const s {samples_.size(),
                         mean,
                         std::sqrt(var / static_cast<double>(samples_.size())),
                         pct(0.50),
                         pct(0.99),
                         samples_.back()};
        samples_.clear();
        return s;
    }

    [[nodiscard]]
    std::size_t pending() const {
        return samples_.size();
    }

    // "<label>: n=... mean ... sd ... p50 ... p99 ... max ... us"
    static std::string format(Summary const& s, std::string_view label = "tick") {
        char buf[256];
        int const k = std::snprintf(
            buf,
            sizeof(buf),
            "%.*s: n=%4zu  mean %6.1f  sd %6.1f  p50 %6.1f  p99 %7.1f  max %8.1f us",
            static_cast<int>(label.size()),
            label.data(),
            s.n,
            s.mean,
            s.sd,
            s.p50,
            s.p99,
            s.max);
        return std::string(buf, k > 0 ? static_cast<std::size_t>(k) : 0);
    }

private:
    std::vector<double> samples_;
    clock::duration period_;
    clock::time_point last_report_;
};

} // namespace ecs::diag
