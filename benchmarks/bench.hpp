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
// (denormalized, same convention as tools/schedule_trace.hpp) so offline
// tools need no joins. Append mode: several suites can share one file, and
// the header is written only when the file starts empty. `commit` comes from
// the ECS_GIT_COMMIT env var (or GITHUB_SHA) -- the process cannot know its
// own checkout -- so runners/CI should export it; "unknown" otherwise.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <string_view>
#include <thread>

namespace bench {

using clock_type = std::chrono::steady_clock;

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

inline int g_iters = 50;
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
    if (c == '"') out += '"';
    out += c;
  }
  out += '"';
  return out;
}

inline std::string compiler_id() {
  char buf[64];
#if defined(__clang__)
  std::snprintf(buf, sizeof(buf), "clang %d.%d.%d", __clang_major__,
                __clang_minor__, __clang_patchlevel__);
#elif defined(__GNUC__)
  std::snprintf(buf, sizeof(buf), "gcc %d.%d.%d", __GNUC__, __GNUC_MINOR__,
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
        while (!v.empty() && v.front() == ' ') v.remove_prefix(1);
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
    if (char const* v = std::getenv(var); v && *v) return v;
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

  void emit(std::string_view name, std::string_view metric, double value,
            std::string_view unit, std::string_view better,
            std::size_t entities, unsigned lanes, std::size_t samples) {
    if (!opened_) open();
    if (!f_) return;
    std::fprintf(f_, "%s,%s,%s,%.6g,%s,%s,%zu,%u,%zu,%s\n", suite_.c_str(),
                 csv_quote(name).c_str(), csv_quote(metric).c_str(), value,
                 csv_quote(unit).c_str(), csv_quote(better).c_str(), entities,
                 lanes, samples, envelope_.c_str());
    std::fflush(f_); // a crashed or ^C'd run keeps the rows written so far
  }

private:
  void open() {
    opened_ = true;
    char const* path = std::getenv("ECS_BENCH_OUT");
    if (!path || !*path) return;
    f_ = std::fopen(path, "a");
    if (!f_) {
      std::fprintf(stderr, "bench: cannot open ECS_BENCH_OUT=%s\n", path);
      return;
    }
    if (std::ftell(f_) == 0)
      std::fprintf(f_, "suite,name,metric,value,unit,better,entities,lanes,"
                       "samples,timestamp,commit,build,compiler,cpu,"
                       "threads_hw\n");
    envelope_ = csv_quote(timestamp_utc()) + "," + csv_quote(commit_id()) +
                "," + csv_quote(build_id()) + "," + csv_quote(compiler_id()) +
                "," + csv_quote(cpu_model()) + "," +
                std::to_string(std::thread::hardware_concurrency());
  }

  bool opened_ = false;
  std::FILE* f_ = nullptr;
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
inline void emit(std::string_view name, std::string_view metric, double value,
                 std::string_view unit, std::string_view better,
                 std::size_t entities = 0, unsigned lanes = 0,
                 std::size_t samples = 0) {
  detail::Sink::instance().emit(name, metric, value, unit, better, entities,
                                lanes, samples);
}

// Best-of-`repeats` ns per element for one body() sweep over n elements, with
// one untimed warmup sweep. The shared low-level helper, so every benchmark in
// this directory uses the same estimator (see the header comment).
template <class Body>
double min_ns_per(std::size_t n, int repeats, Body&& body) {
  body(); // warm caches / page in
  double best = 1e300;
  for (int r = 0; r < repeats; ++r) {
    const auto t0 = clock_type::now();
    body();
    const auto t1 = clock_type::now();
    best = std::min(
        best, std::chrono::duration<double, std::nano>(t1 - t0).count() /
                  static_cast<double>(n));
  }
  return best;
}

template <class Body>
void run(const char* name, std::size_t entities, Body&& body) {
  double best_ms = 1e300;
  for (int r = 0; r < g_repeats; ++r) {
    const auto t0 = clock_type::now();
    for (int it = 0; it < g_iters; ++it) body();
    const auto t1 = clock_type::now();
    best_ms = std::min(
        best_ms, std::chrono::duration<double, std::milli>(t1 - t0).count());
  }
  const double ns_per = best_ms * 1e6 / (g_iters * static_cast<double>(entities));
  std::printf("  %-46s %8.2f ns/entity   (best %.1f ms / %d sweeps)\n", name,
              ns_per, best_ms, g_iters);
  emit(name, "ns_per_entity", ns_per, "ns", "lower", entities, 1,
       static_cast<std::size_t>(g_repeats));
}

} // namespace bench
