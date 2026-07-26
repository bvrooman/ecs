// Minimal microbenchmark harness.
//
// bench(name, entities, body) times `body()` (one sweep over the data), running
// it `iters` times per sample and keeping the best of `repeats` samples -- best
// is the most stable estimator for an in-memory kernel (it discards scheduler
// and frequency-scaling noise). Results are reported as ns per entity per sweep.
//
// Machine-readable results: when ECS_BENCH_OUT=<path> is set, every reported
// number is also appended to that CSV, one row per (benchmark, metric), wrapped
// in a run-metadata envelope so a row is a reproducible claim, not just a
// number:
//
//   suite,name,metric,value,unit,better,entities,lanes,samples,
//   timestamp,commit,build,compiler,cpu,threads_hw
//
// `better` says which direction improves ("lower"/"higher") so comparison
// tools need no metric knowledge. The envelope repeats on every row
// (denormalized, same convention as ecs/diag/schedule_trace.hpp) so offline
// tools need no joins. Append mode: several suites can share one file, and
// the header is written only when the file starts empty. `commit` comes from
// the ECS_GIT_COMMIT env var (or GITHUB_SHA) -- the process cannot know its
// own checkout -- so runners/CI should export it; "unknown" otherwise.
#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace bench {

using clock_type = std::chrono::steady_clock;

// --- configuration ----------------------------------------------------------
// Every suite reads its knobs from ECS_* env vars (ECS_ENTITIES, ECS_TICKS,
// ECS_ITERS, ECS_REPEATS -- same convention as the demos), so sweep scripts
// and the A/B driver configure all of them uniformly. Positional args, where
// a suite historically took them, still win over the env.

inline long env_long(char const* key, long const dflt) {
    if (char const* v = std::getenv(key))
        if (long n = std::atol(v); n > 0)
            return n;
    return dflt;
}

inline std::size_t env_size(char const* key, std::size_t const dflt) {
    return static_cast<std::size_t>(env_long(key, static_cast<long>(dflt)));
}

// The lane sweep for pool-scaling suites: the WorkerPool sizes to time at.
// Parsed from ECS_LANES ("1,2,4,8" -- commas and/or spaces), deduped and
// sorted ascending. When unset it defaults to a power-of-two ladder up to
// hardware_concurrency() *plus hw itself*, so the sweep REACHES the
// oversubscription point: hardware_concurrency() counts logical (SMT) cores
// and the dispatch thread is an extra runnable thread on top of the pool, so
// peak throughput is usually back at the physical-core count or hw-1. Sweeping
// to hw makes that turnover show in the curve instead of hiding behind a
// single (often past-peak) hw data point. The smallest lane in the set is the
// speedup baseline -- keep 1 in the set to anchor it to serial.
inline std::vector<unsigned> lane_set() {
    std::set<unsigned> s;
    if (char const* v = std::getenv("ECS_LANES")) {
        unsigned n = 0;
        bool have  = false;
        for (char const* p = v;; ++p) {
            if (*p >= '0' && *p <= '9') {
                n    = n * 10 + unsigned(*p - '0');
                have = true;
            } else {
                if (have && n > 0)
                    s.insert(n);
                n    = 0;
                have = false;
                if (!*p)
                    break;
            }
        }
    }
    if (s.empty()) {
        unsigned const hw = std::max(2u, std::thread::hardware_concurrency());
        for (unsigned n = 1; n < hw; n *= 2)
            s.insert(n);
        s.insert(hw);
    }
    return {s.begin(), s.end()};
}

// Defeat dead-code elimination: force `v` to be treated as observed/clobbered.
// "+r,m" (read-write) rather than the old input-only "r,m" form: an input-only
// constraint lets the compiler hand the asm a stale copy and keep using the
// registerized value, which is exactly what this must prevent.
template <class T>
inline void keep(T& v) {
    asm volatile("" : "+r,m"(v) : : "memory");
}
template <class T>
inline void keep(T const& v) {
    asm volatile("" : : "r,m"(v) : "memory");
}

inline int g_iters   = 50;
inline int g_repeats = 5;

// --- machine-readable results sink (ECS_BENCH_OUT) -------------------------

namespace detail {

    // RFC-4180 quoting: every string field is quoted so labels may contain commas
    // (e.g. "integrate (1 RW, 1 RO)"); inner quotes double.
    inline std::string csv_quote(std::string_view s) {
        std::string out;
        out.reserve(s.size() + 2);
        out += '"';
        for (char c : s) {
            if (c == '"')
                out += '"';
            out += c;
        }
        out += '"';
        return out;
    }

    inline std::string compiler_id() {
        char buf[64];
#if defined(__clang__)
        std::snprintf(buf,
                      sizeof(buf),
                      "clang %d.%d.%d",
                      __clang_major__,
                      __clang_minor__,
                      __clang_patchlevel__);
#elif defined(__GNUC__)
        std::snprintf(buf,
                      sizeof(buf),
                      "gcc %d.%d.%d",
                      __GNUC__,
                      __GNUC_MINOR__,
                      __GNUC_PATCHLEVEL__);
#else
        std::snprintf(buf, sizeof(buf), "unknown");
#endif
        return buf;
    }

    inline std::string build_id() {
        std::string b =
#ifdef NDEBUG
            "release";
#else
            "debug";
#endif
#if defined(ECS_USE_P2996) && ECS_USE_P2996
        b += ",p2996";
#else
        b += ",portable";
#endif
        return b;
    }

    // "model name" from /proc/cpuinfo (Linux); "unknown" elsewhere -- the field
    // exists so results from different machines are never mistaken for each other.
    inline std::string cpu_model() {
        if (std::FILE* f = std::fopen("/proc/cpuinfo", "r")) {
            char line[256];
            while (std::fgets(line, sizeof(line), f)) {
                if (std::string_view(line).starts_with("model name")) {
                    std::fclose(f);
                    std::string_view v = line;
                    v.remove_prefix(v.find(':') + 1);
                    while (!v.empty() && v.front() == ' ')
                        v.remove_prefix(1);
                    while (!v.empty() && (v.back() == '\n' || v.back() == ' '))
                        v.remove_suffix(1);
                    return std::string(v);
                }
            }
            std::fclose(f);
        }
        return "unknown";
    }

    inline std::string timestamp_utc() {
        std::time_t const t = std::time(nullptr);
        std::tm tm {};
        gmtime_r(&t, &tm);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
        return buf;
    }

    inline std::string commit_id() {
        for (char const* var : {"ECS_GIT_COMMIT", "GITHUB_SHA"})
            if (char const* v = std::getenv(var); v && *v)
                return v;
        return "unknown";
    }

    // The one sink per process. Opened lazily on the first emit; the envelope
    // (everything identical for the whole run) is captured once. Not thread-safe:
    // benchmarks report from the main thread.
    class Sink {
    public:
        static Sink& instance() {
            static Sink s;
            return s;
        }

        void set_suite(std::string_view s) { suite_ = csv_quote(s); }

        void emit(std::string_view name,
                  std::string_view metric,
                  double value,
                  std::string_view unit,
                  std::string_view better,
                  std::size_t entities,
                  unsigned lanes,
                  std::size_t samples) {
            if (!opened_)
                open();
            if (!f_)
                return;
            std::fprintf(f_,
                         "%s,%s,%s,%.6g,%s,%s,%zu,%u,%zu,%s\n",
                         suite_.c_str(),
                         csv_quote(name).c_str(),
                         csv_quote(metric).c_str(),
                         value,
                         csv_quote(unit).c_str(),
                         csv_quote(better).c_str(),
                         entities,
                         lanes,
                         samples,
                         envelope_.c_str());
            std::fflush(f_); // a crashed or ^C'd run keeps the rows written so far
        }

    private:
        void open() {
            opened_          = true;
            char const* path = std::getenv("ECS_BENCH_OUT");
            if (!path || !*path)
                return;
            f_ = std::fopen(path, "a");
            if (!f_) {
                std::fprintf(stderr, "bench: cannot open ECS_BENCH_OUT=%s\n", path);
                return;
            }
            if (std::ftell(f_) == 0)
                std::fprintf(f_,
                             "suite,name,metric,value,unit,better,entities,lanes,"
                             "samples,timestamp,commit,build,compiler,cpu,"
                             "threads_hw\n");
            envelope_ = csv_quote(timestamp_utc()) + "," + csv_quote(commit_id()) + "," +
                        csv_quote(build_id()) + "," + csv_quote(compiler_id()) + "," +
                        csv_quote(cpu_model()) + "," +
                        std::to_string(std::thread::hardware_concurrency());
        }

        bool opened_       = false;
        std::FILE* f_      = nullptr;
        std::string suite_ = "\"\"";
        std::string envelope_;
    };

} // namespace detail

// Name the suite once at the top of main() -- it labels every row this
// process emits (several suites can append to the same ECS_BENCH_OUT file).
inline void set_suite(std::string_view suite) {
    detail::Sink::instance().set_suite(suite);
}

// Record one (benchmark, metric) result row. No-op unless ECS_BENCH_OUT is
// set. `better` is "lower" or "higher"; entities/lanes/samples are 0 when not
// meaningful for the metric.
inline void emit(std::string_view name,
                 std::string_view metric,
                 double value,
                 std::string_view unit,
                 std::string_view better,
                 std::size_t entities = 0,
                 unsigned lanes       = 0,
                 std::size_t samples  = 0) {
    detail::Sink::instance()
        .emit(name, metric, value, unit, better, entities, lanes, samples);
}

// Best-of-`repeats` ns per element for one body() sweep over n elements, with
// one untimed warmup sweep. The shared low-level helper, so every benchmark in
// this directory uses the same estimator (see the header comment).
template <class Body>
double min_ns_per(std::size_t n, int repeats, Body&& body) {
    body(); // warm caches / page in
    double best = 1e300;
    for (int r = 0; r < repeats; ++r) {
        auto const t0 = clock_type::now();
        body();
        auto const t1 = clock_type::now();
        best = std::min(best,
                        std::chrono::duration<double, std::nano>(t1 - t0).count() /
                            static_cast<double>(n));
    }
    return best;
}

// Best-of-`repeats` ns per element for a STRUCTURAL one-shot (spawn a
// population, sort rows, ...): `setup()` rebuilds the precondition untimed
// before each timed `op()`. The one-sweep cousin of min_ns_per for operations
// that consume their input.
template <class Setup, class Op>
double best_ns_of(std::size_t n, int repeats, Setup&& setup, Op&& op) {
    double best = 1e300;
    for (int r = 0; r < repeats; ++r) {
        setup();
        auto const t0 = clock_type::now();
        op();
        auto const t1 = clock_type::now();
        best = std::min(best,
                        std::chrono::duration<double, std::nano>(t1 - t0).count() /
                            static_cast<double>(n));
    }
    return best;
}

// --- per-tick distributions -------------------------------------------------
// The shared distribution type for tick-loop suites (schedule_bench, churn,
// primitives): jitter and tails, not best-case.
struct Dist {
    double mean = 0, sd = 0, p50 = 0, p99 = 0, mx = 0;
    std::size_t n = 0;

    static Dist of(std::vector<double> v) {
        Dist s;
        if (v.empty())
            return s;
        std::sort(v.begin(), v.end());
        s.n        = v.size();
        s.mx       = v.back();
        double sum = 0;
        for (double x : v)
            sum += x;
        s.mean     = sum / static_cast<double>(v.size());
        double var = 0;
        for (double x : v)
            var += (x - s.mean) * (x - s.mean);
        s.sd     = std::sqrt(var / static_cast<double>(v.size()));
        auto pct = [&](double p) {
            return v[std::min(v.size() - 1, std::size_t(p * v.size()))];
        };
        s.p50 = pct(0.50);
        s.p99 = pct(0.99);
        return s;
    }
};

// Run `run_one` `n` times untimed (reach steady state before measuring, and
// before counting allocations -- warmup churn must not land in a steady-state
// allocs/tick number).
template <class RunOne>
void warmup(int n, RunOne&& run_one) {
    for (int i = 0; i < n; ++i)
        run_one();
}

// Warm to steady state untimed, then time `n` runs of `run_one`, in us.
template <class RunOne>
Dist measure_ticks(int warm, int n, RunOne&& run_one) {
    for (int i = 0; i < warm; ++i)
        run_one();
    std::vector<double> us;
    us.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        auto const t0 = clock_type::now();
        run_one();
        auto const t1 = clock_type::now();
        us.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
    }
    return Dist::of(std::move(us));
}

template <class Body>
void run(char const* name, std::size_t entities, Body&& body) {
    double best_ms = 1e300;
    for (int r = 0; r < g_repeats; ++r) {
        auto const t0 = clock_type::now();
        for (int it = 0; it < g_iters; ++it)
            body();
        auto const t1 = clock_type::now();
        best_ms =
            std::min(best_ms, std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    double const ns_per = best_ms * 1e6 / (g_iters * static_cast<double>(entities));
    std::printf("  %-46s %8.2f ns/entity   (best %.1f ms / %d sweeps)\n",
                name,
                ns_per,
                best_ms,
                g_iters);
    emit(name,
         "ns_per_entity",
         ns_per,
         "ns",
         "lower",
         entities,
         1,
         static_cast<std::size_t>(g_repeats));
}

// The machine-readable mirror of a per-tick distribution report line: the
// standard metric rows every tick-loop suite shares.
inline void emit_dist(std::string_view name,
                      unsigned lanes,
                      Dist const& d,
                      std::size_t entities,
                      std::size_t ticks) {
    emit(name, "mean_us_per_tick", d.mean, "us", "lower", entities, lanes, ticks);
    emit(name, "p50_us_per_tick", d.p50, "us", "lower", entities, lanes, ticks);
    emit(name, "p99_us_per_tick", d.p99, "us", "lower", entities, lanes, ticks);
    emit(name, "max_us_per_tick", d.mx, "us", "lower", entities, lanes, ticks);
}

} // namespace bench
