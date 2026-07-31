// web/dynamic/bindings.cpp
//
// Phase A interop spine: an embind facade exposing the runtime component path to
// JavaScript. JS defines components (schemas), creates entities, and adds / gets
// / sets / removes components by id -- all backed by the same World + archetype
// machinery the C++ engine uses, via ecs::dynamic::WorldOps. Component values
// cross the boundary as plain JS number arrays (one per field, declaration
// order); fieldView() hands back a zero-copy typed-array view aliasing a
// component's SoA field buffer -- the Phase B per-chunk kernel foundation.

#include <ecs/diag/stats.hpp> // ecs::diag::TickStats (via the ecs::diag target)
#include <ecs/dynamic/dynamic_column.hpp>
#include <ecs/dynamic/registry.hpp>
#include <ecs/dynamic/world_ops.hpp>
#include <ecs/schedule.hpp>
#include <ecs/world.hpp>

#include "js_system.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <emscripten.h>
#include <emscripten/bind.h>
#include <emscripten/val.h>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace ecs;
using namespace ecs::dynamic;
using emscripten::val;

namespace {

FieldType parse_type(std::string const& s) {
    if (s == "f32")
        return FieldType::f32;
    if (s == "f64")
        return FieldType::f64;
    if (s == "i32")
        return FieldType::i32;
    if (s == "u32")
        return FieldType::u32;
    throw std::runtime_error("unknown field type: " + s);
}

void write_scalar(std::byte* dst, FieldType t, double v) {
    switch (t) {
    case FieldType::f32: {
        auto f = static_cast<float>(v);
        std::memcpy(dst, &f, 4);
        break;
    }
    case FieldType::f64: {
        std::memcpy(dst, &v, 8);
        break;
    }
    case FieldType::i32: {
        auto i = static_cast<std::int32_t>(v);
        std::memcpy(dst, &i, 4);
        break;
    }
    case FieldType::u32: {
        auto u = static_cast<std::uint32_t>(v);
        std::memcpy(dst, &u, 4);
        break;
    }
    }
}

double read_scalar(std::byte const* src, FieldType t) {
    switch (t) {
    case FieldType::f32: {
        float f;
        std::memcpy(&f, src, 4);
        return f;
    }
    case FieldType::f64: {
        double d;
        std::memcpy(&d, src, 8);
        return d;
    }
    case FieldType::i32: {
        std::int32_t i;
        std::memcpy(&i, src, 4);
        return i;
    }
    case FieldType::u32: {
        std::uint32_t u;
        std::memcpy(&u, src, 4);
        return u;
    }
    }
    return 0;
}

} // namespace

class DynamicWorld {
public:
    DynamicWorld() {
#if defined(__EMSCRIPTEN_PTHREADS__)
        // -pthread build: a resident pool so defineParallelSystem fans out across
        // Web Worker lanes. (Single-threaded: no pool; parallel systems run
        // serially via the 1-lane pool Schedule::run(World&) makes.)
        unsigned const hw = std::thread::hardware_concurrency();
        pool_ = std::make_unique<WorkerPool>(std::max(2u, std::min(4u, hw ? hw : 2u)));
#endif
        // ?stats in the page URL -> time each tick() so JS can poll the per-tick
        // distribution via pollStats() (no env var in a browser). Guarded so the
        // node smoke modules, where `location` is undefined, just see it off.
        // The marker below must stay bare: clang-format only honours a comment
        // whose text is exactly "clang-format off", so trailing prose silently
        // disables it -- and this EM_ASM body is JS, which clang-format mangles
        // (`!==` becomes `!= =`).
        // clang-format off
        stats_on_ = EM_ASM_INT(
            { return (typeof location !== 'undefined'
                      && location.search.indexOf('stats') >= 0) ? 1 : 0; });
        // clang-format on
    }

    // fields: JS array of { name: string, type: 'f32'|'f64'|'i32'|'u32' }.
    int defineComponent(std::string name, val fields) {
        std::vector<std::pair<std::string, FieldType>> fs;
        unsigned const n = fields["length"].as<unsigned>();
        for (unsigned i = 0; i < n; ++i) {
            val f = fields[i];
            fs.emplace_back(f["name"].as<std::string>(),
                            parse_type(f["type"].as<std::string>()));
        }
        return static_cast<int>(registry().define(std::move(name), fs).value);
    }

    val createEntity() { return to_val(WorldOps::create_entity(world_)); }
    void destroyEntity(val e) { WorldOps::destroy(world_, from_val(e)); }
    bool hasComponent(val e, int id) {
        return WorldOps::has(world_, from_val(e), static_cast<ComponentId>(id));
    }

    void addComponent(val e, int id, val values) {
        auto const blob = pack(static_cast<ComponentId>(id), values);
        WorldOps::add(world_, from_val(e), static_cast<ComponentId>(id), blob.data());
    }
    void setComponent(val e, int id, val values) {
        auto const blob = pack(static_cast<ComponentId>(id), values);
        WorldOps::set(world_, from_val(e), static_cast<ComponentId>(id), blob.data());
    }
    void removeComponent(val e, int id) {
        WorldOps::remove(world_, from_val(e), static_cast<ComponentId>(id));
    }

    // --- deferred ops, safe to call from inside a system (a JS kernel) --------
    // Unlike createEntity/addComponent/destroyEntity (immediate, host-setup only),
    // these record into the command buffer and apply at the wave barrier, so they
    // don't disturb a running iteration.

    // bundle: array of [componentId, [field values...]].
    void spawn(val bundle) {
        WorldOps::Bundle packed;
        unsigned const n = bundle["length"].as<unsigned>();
        packed.reserve(n);
        for (unsigned i = 0; i < n; ++i) {
            val pair      = bundle[i];
            auto const id = static_cast<ComponentId>(pair[0].as<unsigned>());
            packed.emplace_back(id, pack(id, pair[1]));
        }
        WorldOps::spawn_deferred(world_, std::move(packed));
    }
    void destroy(unsigned index, unsigned generation) {
        WorldOps::destroy_deferred(world_, Entity::from_raw(index, generation));
    }

    // -> JS array of field values, or null if the entity lacks the component.
    val getComponent(val e, int id) {
        auto const& d = registry().desc(static_cast<ComponentId>(id));
        std::vector<std::byte> blob(d.stride);
        if (!WorldOps::get(world_,
                           from_val(e),
                           static_cast<ComponentId>(id),
                           blob.data()))
            return val::null();
        val out = val::array();
        for (std::size_t i = 0; i < d.fields.size(); ++i)
            out.set(i, read_scalar(blob.data() + d.fields[i].offset, d.fields[i].type));
        return out;
    }

    int entityCount() { return static_cast<int>(world_.size()); }

    // Zero-copy typed-array view over field `fieldIndex` of component `id`,
    // across the first archetype that has it (Phase A: arrange one such
    // archetype). NOTE: the view is invalidated by any heap growth or structural
    // edit -- re-acquire it; never cache across those.
    val fieldView(int id, int fieldIndex) {
        auto* column = WorldOps::column(world_, static_cast<ComponentId>(id));
        if (!column)
            return val::null();
        auto const count = column->size();
        void* base       = column->field_base(static_cast<std::size_t>(fieldIndex));
        return web::make_view(column->desc().fields[fieldIndex].type, base, count);
    }

    // Register a JS-defined system. `spec` declares its access --
    // { write?: id[], read?: id[], commands?: bool }. With a query it runs as
    // kernel(count, views, entities) once per matching archetype -- views[
    // component][field] = a zero-copy typed array, entities = [index, generation,
    // ...] per row. With no query it is an emitter: kernel() fires once per tick
    // and spawns via this world's spawn(). Runs main-thread. See js_system.hpp.
    int defineSystem(std::string name, val spec, val kernel) {
        return static_cast<int>(
            web::add_js_system(schedule_, std::move(name), spec, kernel).value);
    }

    // Register a *parallel* JS system. Same access spec { write?, read? } as
    // defineSystem (so it still levels), plus `params` -- a plain JS object the
    // kernel reads as `p`, snapshotted to shared memory each tick(). The kernel
    // (lo, hi, c, p) must be PURE (shipped as source via toString(), eval'd per
    // worker lane): no closures, data-only. Each lane runs it over its slice
    // [lo,hi); on a single-threaded build the 1-lane pool runs it serially.
    int defineParallelSystem(std::string name, val spec, val kernel) {
        SystemAccess access;
        std::vector<ComponentId> query;
        auto collect = [&](char const* key, std::vector<ComponentId>& into) {
            val arr = spec[key];
            if (arr.isUndefined() || arr.isNull())
                return;
            unsigned const n = arr["length"].as<unsigned>();
            for (unsigned i = 0; i < n; ++i) {
                auto const id = static_cast<ComponentId>(arr[i].as<unsigned>());
                into.push_back(id);
                query.push_back(id);
            }
        };
        collect("write", access.writes);
        collect("read", access.reads);
        std::ranges::sort(query);
        query.erase(std::ranges::unique(query).begin(), query.end());

        // Params: hold the JS object + its key names; one shared f64 snapshot buffer.
        ParallelSys ps;
        ps.params     = spec["params"];
        val pnamesArr = val::array();
        if (!ps.params.isUndefined() && !ps.params.isNull()) {
            auto keys    = val::global("Object").call<val>("keys", ps.params);
            auto const k = keys["length"].as<unsigned>();
            for (unsigned i = 0; i < k; ++i) {
                auto key = keys[i].as<std::string>();
                ps.names.push_back(key);
                pnamesArr.call<void>("push", key);
            }
        }

        ps.values    = std::make_shared<std::vector<double>>(ps.names.size(), 0.0);
        auto values  = ps.values;
        int const np = static_cast<int>(ps.names.size());
        parallel_.push_back(ps);

        // Setup blob (parsed + cached once per worker): source, param names, and the
        // view layout (component + field names, in query order).
        auto const src = kernel.call<std::string>("toString");
        auto blob      = val::object();
        blob.set("src",
                 "(" + src + ")"); // wrap so the worker's eval(blob.src) yields the fn
        blob.set("params", pnamesArr);
        val layout = val::array();
        for (auto const cid : query) {
            auto const& d = registry().desc(cid);
            val comp      = val::object();
            comp.set("n", d.name);
            val f = val::array();
            for (auto const& fld : d.fields) {
                // Ship the field TYPE alongside its name: the worker picks the
                // matching typed-array constructor (an f64/i32/u32 field viewed
                // as Float32Array would silently reinterpret its bits).
                val fo = val::object();
                fo.set("n", fld.name);
                switch (fld.type) {
                case FieldType::f64:
                    fo.set("t", std::string("f64"));
                    break;
                case FieldType::i32:
                    fo.set("t", std::string("i32"));
                    break;
                case FieldType::u32:
                    fo.set("t", std::string("u32"));
                    break;
                case FieldType::f32:
                    fo.set("t", std::string("f32"));
                    break;
                }
                f.call<void>("push", fo);
            }
            comp.set("f", f);
            layout.call<void>("push", comp);
        }
        blob.set("layout", layout);
        auto const blobStr = val::global("JSON").call<std::string>("stringify", blob);

        static int next_sid = 0;
        int const sid       = next_sid++;

        auto run = [query, blobStr, values, sid, np](World& w,
                                                     Commands&,
                                                     detail::WorkItem const& item) {
            // The scheduler hands this parallel system ONE row-range slice per
            // call (see Schedule::add_dynamic_parallel); process exactly that
            // slice. Field views span the whole archetype column (base = row 0)
            // while the kernel runs over [lo, hi) only, so slices dispatched to
            // different lanes touch disjoint rows -- no double work, and no
            // dispatch on the pool the scheduler is already fanning us across.
            for (auto const& u : item.units) {
                auto& arch       = WorldOps::archetype_at(w, u.archetype);
                auto const count = arch.size();
                if (count == 0)
                    continue;
                std::vector<std::uint32_t> bases; // field bases, in query order
                for (auto const cid : query) {
                    auto const& d = registry().desc(cid);
                    auto& col     = arch.column_at(cid);
                    for (std::size_t fi = 0; fi < d.fields.size(); ++fi)
                        bases.push_back(static_cast<std::uint32_t>(
                            reinterpret_cast<std::uintptr_t>(col.field_base(fi))));
                }
                char const* const blobC = blobStr.c_str();
                auto const bptr         = static_cast<std::uint32_t>(
                    reinterpret_cast<std::uintptr_t>(bases.data()));
                auto const nb   = static_cast<std::uint32_t>(bases.size());
                auto const vptr = static_cast<std::uint32_t>(
                    reinterpret_cast<std::uintptr_t>(values->data()));
                auto const cnt = static_cast<std::uint32_t>(count);
                auto const lo  = static_cast<int>(u.begin);
                auto const hi  = static_cast<int>(u.end);
                // EM_ASM body must have NO top-level commas (the preprocessor
                // splits macro args on them) -- commas live only inside ( ).
                // clang-format off
                    EM_ASM({
                        var sid=$0;
                        var blobPtr=$1;
                        var lo=$2;
                        var hi=$3;
                        var count=$4;
                        var bptr=$5;
                        var nb=$6;
                        var vptr=$7;
                        var np=$8;
                        if (!globalThis.__psys) globalThis.__psys = {};
                        var R = globalThis.__psys;
                        var S = R[sid];
                        if (!S) {
                            S = {};
                            var blob = JSON.parse(UTF8ToString(blobPtr));
                            S.fn = eval(blob.src);
                            S.layout = blob.layout;
                            S.params = blob.params;
                            R[sid] = S;
                        }
                        var bases = new Uint32Array(HEAPU32.buffer, bptr, nb);
                        var c = {};
                        var k = 0;
                        for (var ci=0; ci<S.layout.length; ci++) {
                            var comp = S.layout[ci];
                            var o = {};
                            for (var fi=0; fi<comp.f.length; fi++) {
                                var fld = comp.f[fi];
                                var base = bases[k++];
                                var view;
                                if (fld.t === 'f64') view = new Float64Array(HEAPF64.buffer, base, count);
                                else if (fld.t === 'i32') view = new Int32Array(HEAP32.buffer, base, count);
                                else if (fld.t === 'u32') view = new Uint32Array(HEAPU32.buffer, base, count);
                                else view = new Float32Array(HEAPF32.buffer, base, count);
                                o[fld.n] = view;
                            }
                            c[comp.n] = o;
                        }
                        var p = {};
                        if (np > 0) {
                            var pv = new Float64Array(HEAPF64.buffer, vptr, np);
                            for (var pi=0; pi<S.params.length; pi++) p[S.params[pi]] = pv[pi];
                        }
                        S.fn(lo, hi, c, p);
                    }, sid, blobC, lo, hi, cnt, bptr, nb, vptr, np);
                // clang-format on
            }
        };
        return static_cast<int>(schedule_
                                    .add_dynamic_parallel(std::move(name),
                                                          std::move(access),
                                                          Signature(query),
                                                          std::move(run))
                                    .value);
    }

    // Run the schedule once. First snapshot every parallel system's params object
    // into its shared buffer (here, on the main thread) so the worker lanes read a
    // stable copy. Under -pthread the schedule runs on the resident pool (data
    // systems fan out); otherwise the 1-lane pool runs everything serially.
    void tick() {
        for (auto& ps : parallel_) {
            if (ps.params.isUndefined() || ps.params.isNull())
                continue;
            for (std::size_t i = 0; i < ps.names.size(); ++i)
                (*ps.values)[i] = ps.params[ps.names[i]].as<double>();
        }
        double const t0 = stats_on_ ? emscripten_get_now() : 0.0;
#if defined(__EMSCRIPTEN_PTHREADS__)
        schedule_.run(world_, *pool_);
#else
        schedule_.run(world_);
#endif
        if (stats_on_)
            tick_stats_.sample((emscripten_get_now() - t0) * 1000.0); // ms -> us
    }

    // Number of sequential waves -- proof the scheduler leveled the systems by
    // their declared access.
    int waveCount() { return static_cast<int>(schedule_.level_count()); }

    // Worker-lane count of the resident pool (1 = single-threaded build / no pool).
    int laneCount() {
#if defined(__EMSCRIPTEN_PTHREADS__)
        return pool_ ? static_cast<int>(pool_->lanes()) : 1;
#else
        return 1;
#endif
    }

    // True when the page was opened with ?stats. JS queries this once to decide
    // whether to poll; tick() only times itself (and pollStats() only reports)
    // when on, so there is no cost otherwise.
    bool statsOn() const { return stats_on_; }

    // The per-tick timing distribution as one formatted line, ~every 2s; "" the
    // rest of the time. JS: `const s = w.pollStats(); if (s) console.log(s);`
    std::string pollStats() {
        if (auto const s = tick_stats_.due())
            return diag::TickStats::format(*s, "sim tick");
        return {};
    }

private:
    std::vector<std::byte> pack(ComponentId id, val values) {
        auto const& d = registry().desc(id);
        auto blob     = std::vector<std::byte>(d.stride);
        for (std::size_t i = 0; i < d.fields.size(); ++i)
            write_scalar(blob.data() + d.fields[i].offset,
                         d.fields[i].type,
                         values[i].as<double>());
        return blob;
    }

    static val to_val(Entity e) {
        val o = val::object();
        o.set("index", static_cast<unsigned>(e.index()));
        o.set("generation", static_cast<unsigned>(e.generation()));
        return o;
    }
    static Entity from_val(val e) {
        return Entity::from_raw(e["index"].as<unsigned>(),
                                e["generation"].as<unsigned>());
    }

    // Per parallel system: the JS params object (read each tick), its key names,
    // and the shared f64 buffer the snapshot lands in (and the kernel reads as p).
    struct ParallelSys {
        val params;
        std::vector<std::string> names;
        std::shared_ptr<std::vector<double>> values;
    };
    std::vector<ParallelSys> parallel_;

    World world_;
    Schedule schedule_;
#if defined(__EMSCRIPTEN_PTHREADS__)
    std::unique_ptr<WorkerPool> pool_;
#endif
    diag::TickStats tick_stats_; // per-tick timing distribution -> JS (?stats)
    bool stats_on_ = false;
};

EMSCRIPTEN_BINDINGS(ecs_dynamic) {
    using namespace emscripten;
    class_<DynamicWorld>("DynamicWorld")
        .constructor<>()
        .function("defineComponent", &DynamicWorld::defineComponent)
        .function("createEntity", &DynamicWorld::createEntity)
        .function("destroyEntity", &DynamicWorld::destroyEntity)
        .function("hasComponent", &DynamicWorld::hasComponent)
        .function("addComponent", &DynamicWorld::addComponent)
        .function("setComponent", &DynamicWorld::setComponent)
        .function("removeComponent", &DynamicWorld::removeComponent)
        .function("getComponent", &DynamicWorld::getComponent)
        .function("entityCount", &DynamicWorld::entityCount)
        .function("fieldView", &DynamicWorld::fieldView)
        .function("defineSystem", &DynamicWorld::defineSystem)
        .function("defineParallelSystem", &DynamicWorld::defineParallelSystem)
        .function("spawn", &DynamicWorld::spawn)
        .function("destroy", &DynamicWorld::destroy)
        .function("tick", &DynamicWorld::tick)
        .function("waveCount", &DynamicWorld::waveCount)
        .function("laneCount", &DynamicWorld::laneCount)
        .function("statsOn", &DynamicWorld::statsOn)
        .function("pollStats", &DynamicWorld::pollStats);
}
