// examples/fmm2d.cpp
//
// An ADAPTIVE FAST MULTIPOLE METHOD n-body solver built on this ECS, as a
// feasibility spike (see docs/FMM_FEASIBILITY.md for the write-up).
//
// The physics is the classical 2D Greengard-Rokhlin FMM for the logarithmic
// kernel: complex potential w(z) = sum_j q_j log(z - z_j), from which the force
// on a unit charge is conj(w'(z)). The tree is a genuinely adaptive quadtree
// (subdivide while a box holds more than `leaf_cap` particles) and the
// interaction lists come from a dual-tree traversal, so source and target
// boxes may sit at different levels -- the adaptive case, not a uniform grid.
//
// What the spike is actually testing is the ECS mapping, not the physics:
//
//   particles are entities         Pos/Charge/Field/Key are components, so the
//                                  SoA columns, the Morton row sort and the
//                                  data-parallel passes are the library's.
//   tree NODES are entities        one per node, in the archetype of its level
//                                  (the Lvl<L> tag), carrying its expansions
//                                  Mp/Lc as components -- so every pass writes
//                                  its OWN rows and the scheduler sees it.
//   gathers are ColumnView         a Query shows a body only its work item's
//                                  rows; ColumnView<Cs...> reads every row of
//                                  named components, declaring a read on
//                                  exactly those. Narrowed with a tag
//                                  (ColumnView<Mp, NodeRef, Lvl<L+1>>) it names
//                                  the level below, so the rows read are
//                                  provably disjoint from the rows written.
//   the tree itself is a resource  variable-length CSR interaction lists have
//                                  no component shape; read via Res<Tree>.
//   ordering is DERIVED            only the pipeline head carries phases
//                                  (morton -> sort -> build, ordered by a
//                                  structural effect). The upward and downward
//                                  passes conflict on Mp/Lc, so the scheduler
//                                  places those barriers itself; registration
//                                  order fixes the direction.
//
// See docs/FMM_FEASIBILITY.md for the write-up, including what this cost the
// library (ColumnView, grain{n}, chunk::row_begin(), and a memoized ad-hoc read
// path) and what is still unsupported (the serial tree build).
//
// Run:  fmm2d [n] [leaf_cap] [ticks] [max_lanes]
// Verifies against a direct O(N^2) sum on a random sample and reports the
// per-lane tick time.

#include <ecs/diag/schedule_report.hpp>
#include <ecs/ecs.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <thread>
#include <vector>

using namespace ecs;

// --- FMM parameters --------------------------------------------------------
using cplx = std::complex<double>;

constexpr int kP        = 12; // expansion order (terms 1..kP plus the monopole)
constexpr int kMaxLevel = 8;  // quadtree depth cap (also the Morton key depth)
// Multipole acceptance, in the max-norm: two boxes interact through expansions
// when a full box separates them (the classic FMM near/far split, generalized
// to the unequal-size pairs a dual-tree traversal produces).
constexpr double kBuffer = 2.0;

// The expansions are COMPONENTS of the node entities below. Reflection stores
// each as one column of fixed-size arrays (a std::array member is a single
// field, and std::complex is opaque to the portable backend), so a level's
// multipoles are contiguous -- which is what a ColumnView hands a gather.
struct Mp {
    std::array<cplx, kP + 1> a {}; // a[0] = total charge, a[1..p] = moments
};
struct Lc {
    std::array<cplx, kP + 1> b {};
};

// Binomials up to 2p+2, built once.
struct Binom {
    static constexpr int N = 2 * kP + 4;
    double c[N][N] {};
    Binom() {
        for (int i = 0; i < N; ++i) {
            c[i][0] = 1.0;
            for (int j = 1; j <= i; ++j)
                c[i][j] = c[i - 1][j - 1] + c[i - 1][j];
        }
    }
};
inline Binom const& binom() {
    static Binom const b;
    return b;
}

// --- components ------------------------------------------------------------
struct Pos {
    double x, y;
};
struct Charge {
    double q;
};
struct Field { // the output: force per unit charge + potential
    double fx, fy, phi;
};
struct Key { // Morton key at kMaxLevel, the row-sort key
    std::uint32_t k;
};
struct LeafId { // which leaf box owns this particle
    std::uint32_t leaf;
};

// Tree nodes are entities too, and they carry the real payload: a node's
// expansions ARE its components (Mp/Lc above), so every FMM pass writes its own
// rows and the scheduler can see it. One node entity per tree node, in the
// archetype of its level -- the Lvl<L> tag is what lets a per-level pass select
// its level, and what lets a ColumnView name the level BELOW it.
struct NodeRef {
    std::uint32_t slot;  // row within this level's archetype
    std::uint32_t level; // which level, so a gather can index by it
};
template <int L>
struct Lvl {};

// --- resources -------------------------------------------------------------
// The tree: topology, geometry and the CSR interaction lists. Nodes are
// numbered level-major (BFS order), so level L owns ids
// [level_begin[L], level_begin[L+1]) and a level pool's slot i is node
// level_begin[L] + i -- which is how a node pass finds its node without ever
// needing its global row index (the executor does not expose one).
struct Tree {
    std::vector<double> cx, cy, half;
    std::vector<std::uint8_t> level, is_leaf;
    std::vector<std::int32_t> child0; // first child id; children are contiguous
    std::vector<std::uint8_t> nchild;
    std::vector<std::int32_t> parent;            // -1 for the root; L2L pulls through it
    std::vector<std::uint32_t> pbegin, pend;     // particle row range (sorted order)
    std::vector<std::uint32_t> key_lo;           // Morton key range of the box
    std::vector<std::uint32_t> leaves;           // leaf node ids
    std::vector<std::uint32_t> leaf_key_lo;      // parallel to leaves, for lookup
    std::vector<std::uint32_t> level_begin;      // level -> first node id
    std::vector<std::uint32_t> m2l_off, m2l_src; // CSR, per node
    std::vector<std::uint32_t> p2p_off, p2p_src; // CSR, per node (leaf targets)
    std::uint32_t node_count = 0;
    int depth                = 0;

    // Scratch reused across ticks so a steady-state build does not allocate.
    std::vector<std::vector<std::uint32_t>> m2l_tmp, p2p_tmp;
};

// Per-tick counters the build fills for the report.
struct BuildStats {
    std::uint32_t nodes = 0, leaves = 0, m2l_pairs = 0, p2p_pairs = 0;
    int depth = 0;
};

// --- Morton key ------------------------------------------------------------
inline std::uint32_t morton(double const x, double const y) {
    auto const q = [](double v) {
        constexpr double kCells = double(1u << kMaxLevel);
        auto i                  = static_cast<std::int64_t>(v * kCells);
        return static_cast<std::uint32_t>(std::clamp<std::int64_t>(i,
                                                                   0,
                                                                   (1 << kMaxLevel) - 1));
    };
    std::uint32_t const xi = q(x), yi = q(y);
    std::uint32_t k = 0;
    for (int b = 0; b < kMaxLevel; ++b)
        k |= ((xi >> b) & 1u) << (2 * b) | ((yi >> b) & 1u) << (2 * b + 1);
    return k;
}

// --- FMM kernels -----------------------------------------------------------
// P2M: moments of a leaf's particles about its center.
inline void p2m(Mp& m,
                cplx const c,
                std::span<double const> px,
                std::span<double const> py,
                std::span<double const> pq,
                std::uint32_t const b,
                std::uint32_t const e) {
    for (std::uint32_t j = b; j < e; ++j) {
        cplx const d   = cplx(px[j], py[j]) - c;
        double const q = pq[j];
        m.a[0] += q;
        cplx dk = 1.0;
        for (int k = 1; k <= kP; ++k) {
            dk *= d;
            m.a[std::size_t(k)] -= q * dk / double(k);
        }
    }
}

// M2M: shift a child multipole (center cc) into its parent (center cp).
inline void m2m(Mp& parent, Mp const& child, cplx const cc, cplx const cp) {
    cplx const d = cc - cp;
    parent.a[0] += child.a[0];
    std::array<cplx, kP + 1> dp {};
    dp[0] = 1.0;
    for (int i = 1; i <= kP; ++i)
        dp[std::size_t(i)] = dp[std::size_t(i - 1)] * d;
    for (int l = 1; l <= kP; ++l) {
        cplx s = -child.a[0] * dp[std::size_t(l)] / double(l);
        for (int k = 1; k <= l; ++k)
            s += child.a[std::size_t(k)] * dp[std::size_t(l - k)] *
                 binom().c[l - 1][k - 1];
        parent.a[std::size_t(l)] += s;
    }
}

// M2L: convert a source multipole (center cs) into a local expansion at ct.
inline void m2l(Lc& loc, Mp const& src, cplx const cs, cplx const ct) {
    cplx const d = cs - ct; // source center relative to the local center
    // a_k / d^k, with the alternating sign folded in.
    std::array<cplx, kP + 1> t {};
    cplx dk = 1.0;
    for (int k = 1; k <= kP; ++k) {
        dk *= d;
        t[std::size_t(k)] = src.a[std::size_t(k)] / dk * ((k & 1) ? -1.0 : 1.0);
    }
    cplx s0 = src.a[0] * std::log(-d);
    for (int k = 1; k <= kP; ++k)
        s0 += t[std::size_t(k)];
    loc.b[0] += s0;
    cplx dl = 1.0;
    for (int l = 1; l <= kP; ++l) {
        dl *= d;
        cplx s = -src.a[0] / (double(l) * dl);
        cplx acc {};
        for (int k = 1; k <= kP; ++k)
            acc += t[std::size_t(k)] * binom().c[l + k - 1][k - 1];
        loc.b[std::size_t(l)] += s + acc / dl;
    }
}

// L2L: shift a parent local expansion (center cp) down to a child (center cc).
inline void l2l(Lc& child, Lc const& parent, cplx const cp, cplx const cc) {
    cplx const d = cc - cp;
    for (int l = 0; l <= kP; ++l) {
        cplx s {};
        cplx dk = 1.0;
        for (int k = l; k <= kP; ++k) {
            s += parent.b[std::size_t(k)] * binom().c[k][l] * dk;
            dk *= d;
        }
        child.b[std::size_t(l)] += s;
    }
}

// L2P: evaluate a local expansion (and its derivative) at a point.
inline void l2p(Lc const& loc, cplx const z, double& fx, double& fy, double& phi) {
    cplx w {}, dw {};
    cplx zk = 1.0; // z^(l-1)
    w += loc.b[0];
    for (int l = 1; l <= kP; ++l) {
        dw += double(l) * loc.b[std::size_t(l)] * zk;
        zk *= z;
        w += loc.b[std::size_t(l)] * zk;
    }
    phi += w.real();
    fx += dw.real();
    fy -= dw.imag();
}

// --- tree build (serial) ---------------------------------------------------
static void build_tree(Tree& t,
                       std::span<std::uint32_t const> keys,
                       std::uint32_t const leaf_cap) {
    auto const n = static_cast<std::uint32_t>(keys.size());
    t.cx.clear();
    t.cy.clear();
    t.half.clear();
    t.level.clear();
    t.is_leaf.clear();
    t.child0.clear();
    t.nchild.clear();
    t.pbegin.clear();
    t.pend.clear();
    t.key_lo.clear();
    t.leaves.clear();
    t.leaf_key_lo.clear();
    t.level_begin.clear();

    auto add_node = [&](double cx,
                        double cy,
                        double half,
                        int level,
                        std::uint32_t kb,
                        std::uint32_t pb,
                        std::uint32_t pe,
                        std::int32_t par) {
        t.cx.push_back(cx);
        t.cy.push_back(cy);
        t.half.push_back(half);
        t.level.push_back(static_cast<std::uint8_t>(level));
        t.is_leaf.push_back(1);
        t.child0.push_back(-1);
        t.nchild.push_back(0);
        t.parent.push_back(par);
        t.key_lo.push_back(kb);
        t.pbegin.push_back(pb);
        t.pend.push_back(pe);
    };

    t.level_begin.push_back(0);
    add_node(0.5, 0.5, 0.5, 0, 0, 0, n, -1);
    std::uint32_t cursor = 0;
    int level            = 0;
    t.depth              = 0;
    while (cursor < t.cx.size()) {
        auto const level_end = static_cast<std::uint32_t>(t.cx.size());
        t.level_begin.push_back(level_end);
        for (std::uint32_t i = cursor; i < level_end; ++i) {
            auto const count = t.pend[i] - t.pbegin[i];
            if (count <= leaf_cap || level >= kMaxLevel)
                continue;
            // Children are contiguous key sub-ranges of a Morton-sorted array.
            int const shift    = 2 * (kMaxLevel - level - 1);
            auto const h       = t.half[i] * 0.5;
            std::int32_t first = -1;
            std::uint8_t nc    = 0;
            for (std::uint32_t c = 0; c < 4; ++c) {
                std::uint32_t const kb = t.key_lo[i] + (c << shift);
                std::uint32_t const ke = kb + (1u << shift);
                auto const lo =
                    static_cast<std::uint32_t>(std::lower_bound(keys.begin() +
                                                                    t.pbegin[i],
                                                                keys.begin() + t.pend[i],
                                                                kb) -
                                               keys.begin());
                auto const hi =
                    static_cast<std::uint32_t>(std::lower_bound(keys.begin() +
                                                                    t.pbegin[i],
                                                                keys.begin() + t.pend[i],
                                                                ke) -
                                               keys.begin());
                if (lo == hi)
                    continue;
                if (first < 0)
                    first = static_cast<std::int32_t>(t.cx.size());
                ++nc;
                add_node(t.cx[i] + ((c & 1u) ? h : -h),
                         t.cy[i] + ((c & 2u) ? h : -h),
                         h,
                         level + 1,
                         kb,
                         lo,
                         hi,
                         static_cast<std::int32_t>(i));
            }
            if (nc > 0) {
                t.is_leaf[i] = 0;
                t.child0[i]  = first;
                t.nchild[i]  = nc;
                t.depth      = level + 1;
            }
        }
        cursor = level_end;
        ++level;
    }
    t.node_count = static_cast<std::uint32_t>(t.cx.size());
    // Pad so level_begin[L + 1] is always in range, even for a shallow tree.
    while (t.level_begin.size() < std::size_t(kMaxLevel) + 2)
        t.level_begin.push_back(t.node_count);
    // Leaves tile the domain, so sorting them by Morton key makes "which leaf
    // owns this key" a binary search (see the assign-leaf system).
    for (std::uint32_t i = 0; i < t.node_count; ++i)
        if (t.is_leaf[i])
            t.leaves.push_back(i);
    std::ranges::sort(t.leaves, {}, [&](std::uint32_t const i) { return t.key_lo[i]; });
    t.leaf_key_lo.reserve(t.leaves.size());
    for (auto const i : t.leaves)
        t.leaf_key_lo.push_back(t.key_lo[i]);
}

// Dual-tree traversal: the adaptive interaction lists. Source and target may
// sit at different levels; every particle pair is covered exactly once, either
// by an M2L or by a direct P2P.
static void traverse(Tree& t, std::uint32_t A, std::uint32_t B) {
    double const dx = std::abs(t.cx[B] - t.cx[A]), dy = std::abs(t.cy[B] - t.cy[A]);
    double const sep = kBuffer * (t.half[A] + t.half[B]);
    if (std::max(dx, dy) >= sep * (1.0 - 1e-9)) {
        t.m2l_tmp[A].push_back(B);
        return;
    }
    bool const al = t.is_leaf[A], bl = t.is_leaf[B];
    if (al && bl) {
        t.p2p_tmp[A].push_back(B);
        return;
    }
    if (bl || (!al && t.half[A] >= t.half[B])) {
        for (std::uint32_t c = 0; c < t.nchild[A]; ++c)
            traverse(t, static_cast<std::uint32_t>(t.child0[A]) + c, B);
    } else {
        for (std::uint32_t c = 0; c < t.nchild[B]; ++c)
            traverse(t, A, static_cast<std::uint32_t>(t.child0[B]) + c);
    }
}

static void build_lists(Tree& t) {
    t.m2l_tmp.resize(t.node_count);
    t.p2p_tmp.resize(t.node_count);
    for (std::uint32_t i = 0; i < t.node_count; ++i) {
        t.m2l_tmp[i].clear();
        t.p2p_tmp[i].clear();
    }
    traverse(t, 0, 0);
    auto flatten = [](auto const& tmp, auto& off, auto& src, std::uint32_t n) {
        off.assign(n + 1, 0);
        for (std::uint32_t i = 0; i < n; ++i)
            off[i + 1] = off[i] + static_cast<std::uint32_t>(tmp[i].size());
        src.resize(off[n]);
        for (std::uint32_t i = 0; i < n; ++i)
            std::copy(tmp[i].begin(), tmp[i].end(), src.begin() + off[i]);
    };
    flatten(t.m2l_tmp, t.m2l_off, t.m2l_src, t.node_count);
    flatten(t.p2p_tmp, t.p2p_off, t.p2p_src, t.node_count);
}

// --- the schedule ----------------------------------------------------------
// Only the pipeline HEAD carries phases: morton -> sort -> build is ordered by
// a structural effect (a row sort recorded as a command), which access analysis
// does not model. Everything downstream is ordered by DERIVED access -- the
// passes write their own rows' Mp/Lc and read other rows through ColumnView, so
// the scheduler places every barrier of the upward and downward passes itself.
// The direction is registration order: deepest-first for M2M, shallowest-first
// for L2L.
namespace ph {
constexpr int kMorton = -3;
constexpr int kSort   = -2;
constexpr int kBuild  = -1;
} // namespace ph

// Heavy per-row kernels over few rows: the default 1024-row item floor would
// pin a level's pass to one item -- one lane -- so ask for a finer split.
constexpr std::size_t kNodeGrain = 64;
// The near field is the opposite problem: plenty of rows, but per-row cost
// tracks local density, so the default item target leaves items that differ by
// ~2x and the heaviest becomes the straggler every lane waits on. Smaller items
// than the default sizing would ever choose.
constexpr std::size_t kParticleGrain = 256;

// Level -> that level's contiguous column, gathered from a ColumnView. Every
// FMM gather is "read node n's expansion", and node ids are level-major, so one
// pass over the view's chunks (one per level archetype) turns a node id into a
// span index: node n at level l is slot n - level_begin[l] of levels[l].
template <class Coeff>
struct LevelSpans {
    std::array<std::span<std::array<cplx, kP + 1> const>, kMaxLevel + 2> level {};

    template <class View>
    void gather(View const& view) {
        // A trailing pack absorbs any tag chunk the view was narrowed with
        // (ColumnView<Mp, NodeRef, Lvl<L>> hands the kernel three chunks).
        view.for_each_chunk([&](std::span<Entity const>,
                                chunk<Coeff const> c,
                                chunk<NodeRef const> nr,
                                auto&&...) {
            if (c.size() == 0)
                return;
            auto const lv = nr.template column<1>()[0]; // level is uniform here
            level[lv]     = c.template column<0>();
        });
    }
};

// Particle columns, read through a ColumnView: full contiguous spans of the
// real SoA storage, no shadow copy. Declares reads on exactly Pos and Charge.
struct ParticleCols {
    std::span<double const> x, y, q;
};
inline ParticleCols particle_cols(ColumnView<Pos, Charge> const& view) {
    ParticleCols c;
    view.for_each_chunk([&](std::span<Entity const>,
                            chunk<Pos const> p,
                            chunk<Charge const> ch) {
        c.x = p.column<0>();
        c.y = p.column<1>();
        c.q = ch.column<0>();
    });
    return c;
}

// M2M for one level. Registered DEEPEST FIRST: consecutive levels conflict on
// Mp, so registration order is what fixes the direction of the chain.
template <int L>
static void register_m2m_level(Schedule& s) {
    // M2M: gather this level's nodes from their children one level down. The
    // view names Lvl<L + 1>, so the rows it reads are provably disjoint from the
    // rows this system writes -- a different archetype, not an argument.
    s.add_parallel(
        "m2m." + std::to_string(L),
        [](Query<Mp, NodeRef const, Lvl<L> const> q,
           ColumnView<Mp, NodeRef, Lvl<L + 1>> below,
           Res<Tree> t) {
            LevelSpans<Mp> child;
            child.gather(below);
            auto const base  = t->level_begin[L];
            auto const end   = t->level_begin[L + 1];
            auto const cbase = t->level_begin[L + 1];
            q.for_each_chunk([&](std::span<Entity const>,
                                 chunk<Mp> m,
                                 chunk<NodeRef const> nr,
                                 chunk<Lvl<L> const>) {
                auto out        = m.template column<0>();
                auto const slot = nr.template column<0>();
                for (std::size_t r = 0; r < out.size(); ++r) {
                    std::uint32_t const node = base + slot[r];
                    if (node >= end || t->is_leaf[node])
                        continue; // pool slack, or a leaf P2M already filled
                    Mp acc {};
                    cplx const cp {t->cx[node], t->cy[node]};
                    for (std::uint32_t c = 0; c < t->nchild[node]; ++c) {
                        auto const ch = static_cast<std::uint32_t>(t->child0[node]) + c;
                        Mp cm {};
                        cm.a = child.level[L + 1][ch - cbase];
                        m2m(acc, cm, cplx {t->cx[ch], t->cy[ch]}, cp);
                    }
                    out[r] = acc.a;
                }
            });
        },
        grain {kNodeGrain});
}

// L2L for one level. Registered SHALLOWEST FIRST, after M2L (which writes Lc).
template <int L>
static void register_l2l_level(Schedule& s) {
    // L2L: PULL the parent's local expansion down, rather than pushing to the
    // children -- a system may only write its own rows, and the children are
    // rows of another archetype. Accumulates onto what M2L already wrote.
    s.add_parallel(
        "l2l." + std::to_string(L),
        [](Query<Lc, NodeRef const, Lvl<L> const> q,
           ColumnView<Lc, NodeRef, Lvl<L - 1>> above,
           Res<Tree> t) {
            LevelSpans<Lc> parent;
            parent.gather(above);
            auto const base  = t->level_begin[L];
            auto const end   = t->level_begin[L + 1];
            auto const pbase = t->level_begin[L - 1];
            q.for_each_chunk([&](std::span<Entity const>,
                                 chunk<Lc> l,
                                 chunk<NodeRef const> nr,
                                 chunk<Lvl<L> const>) {
                auto out        = l.template column<0>();
                auto const slot = nr.template column<0>();
                for (std::size_t r = 0; r < out.size(); ++r) {
                    std::uint32_t const node = base + slot[r];
                    if (node >= end)
                        continue;
                    auto const par = t->parent[node];
                    if (par < 0)
                        continue;
                    Lc acc {};
                    acc.b = out[r];
                    Lc up {};
                    up.b = parent.level[L - 1][static_cast<std::uint32_t>(par) - pbase];
                    l2l(acc,
                        up,
                        cplx {t->cx[par], t->cy[par]},
                        cplx {t->cx[node], t->cy[node]});
                    out[r] = acc.b;
                }
            });
        },
        grain {kNodeGrain});
}

// The two folds. Ls counts up; the level each maps to is what puts the
// registrations in dependency order.
template <int... Ls>
static void register_m2m(Schedule& s, std::integer_sequence<int, Ls...>) {
    (register_m2m_level<kMaxLevel - 1 - Ls>(s), ...); // deepest first
}
template <int... Ls>
static void register_l2l(Schedule& s, std::integer_sequence<int, Ls...>) {
    (register_l2l_level<Ls + 1>(s), ...); // shallowest first
}

// Grow the node pools to cover the current tree. Rows are never destroyed, so a
// steady-state tick records nothing.
struct Pools {
    std::array<std::uint32_t, kMaxLevel + 2> rows {};
};

template <int L>
static void grow_level(Commands& cmd, Pools& pools, Tree const& t) {
    auto const want = t.level_begin[L + 1] - t.level_begin[L];
    for (std::uint32_t i = pools.rows[L]; i < want; ++i)
        cmd.spawn(Mp {}, Lc {}, NodeRef {i, static_cast<std::uint32_t>(L)}, Lvl<L> {});
    pools.rows[L] = std::max(pools.rows[L], want);
}
template <int... Ls>
static void grow_levels(Commands& cmd,
                        Pools& pools,
                        Tree const& t,
                        std::integer_sequence<int, Ls...>) {
    (grow_level<Ls>(cmd, pools, t), ...);
}

static void build_schedule(Schedule& s, std::uint32_t const leaf_cap) {
    // 1. Morton key per particle -- a plain data-parallel map.
    s.add_parallel(
        "morton",
        [](Query<Pos const, Key> q) {
            q.for_each_chunk([](std::span<Entity const>,
                                chunk<Pos const> p,
                                chunk<Key> k) {
                auto const x = p.column<0>();
                auto const y = p.column<1>();
                auto ks      = k.column<0>();
                for (std::size_t i = 0; i < ks.size(); ++i)
                    ks[i] = morton(x[i], y[i]);
            });
        },
        phase {ph::kMorton});

    // 2. Sort the rows by Morton key, so every tree box is a contiguous ROW
    //    RANGE of the particle archetype -- what lets a node pass read "its"
    //    particles as a span. A structural effect, hence the explicit phase.
    s.add_serial(
        "sort",
        [](Commands& c) { c.sort<Key>([](Key const& k) { return k.k; }); },
        phase {ph::kSort});

    // 3. Build the adaptive tree + the dual-tree interaction lists, and grow the
    //    node pools. Serial by nature: a recursive traversal has no row-shaped
    //    parallel form in this executor.
    s.add_serial(
        "build",
        [leaf_cap](ColumnView<Key> keys_view,
                   ResMut<Tree> t,
                   ResMut<Pools> pools,
                   ResMut<BuildStats> st,
                   Commands& cmd) {
            static std::vector<std::uint32_t> keys;
            keys.clear();
            keys_view.for_each_chunk([&](std::span<Entity const>, chunk<Key const> k) {
                auto const ks = k.column<0>();
                keys.insert(keys.end(), ks.begin(), ks.end());
            });
            build_tree(*t, keys, leaf_cap);
            build_lists(*t);
            grow_levels(cmd,
                        *pools,
                        *t,
                        std::make_integer_sequence<int, kMaxLevel + 1> {});

            st->nodes     = t->node_count;
            st->leaves    = static_cast<std::uint32_t>(t->leaves.size());
            st->depth     = t->depth;
            st->m2l_pairs = t->m2l_off.back();
            st->p2p_pairs = t->p2p_off.back();
        },
        phase {ph::kBuild});

    // 4. P2M: one multipole per leaf, from that leaf's contiguous particle rows.
    //    ONE system over every level archetype -- leaves live at many levels, and
    //    a query matches every archetype containing its components.
    s.add_parallel(
        "p2m",
        [](Query<Mp, NodeRef const> q, ColumnView<Pos, Charge> parts, Res<Tree> t) {
            auto const pc = particle_cols(parts);
            q.for_each_chunk([&](std::span<Entity const>,
                                 chunk<Mp> m,
                                 chunk<NodeRef const> nr) {
                auto out         = m.column<0>();
                auto const slot  = nr.column<0>();
                auto const level = nr.column<1>();
                for (std::size_t r = 0; r < out.size(); ++r) {
                    auto const node = t->level_begin[level[r]] + slot[r];
                    if (node >= t->level_begin[level[r] + 1] || !t->is_leaf[node])
                        continue;
                    Mp acc {};
                    p2m(acc,
                        cplx {t->cx[node], t->cy[node]},
                        pc.x,
                        pc.y,
                        pc.q,
                        t->pbegin[node],
                        t->pend[node]);
                    out[r] = acc.a;
                }
            });
        },
        grain {kNodeGrain});

    // 5. M2M, deepest level first: each conflicts with the next on Mp, so the
    //    scheduler chains them in registration order with no phases.
    register_m2m(s, std::make_integer_sequence<int, kMaxLevel> {});

    // 6. M2L: the dominant pass, and ONE system over every level archetype.
    //    Dual-tree M2L has no cross-level dependency, so all levels' items land
    //    in a single wave and one dynamically claimed list -- the best balance
    //    available. Sources come from ColumnView<Mp>: every level's multipoles,
    //    read-only, disjoint from the Lc this writes.
    s.add_parallel(
        "m2l",
        [](Query<Lc, NodeRef const> q, ColumnView<Mp, NodeRef> src, Res<Tree> t) {
            LevelSpans<Mp> mp;
            mp.gather(src);
            q.for_each_chunk([&](std::span<Entity const>,
                                 chunk<Lc> l,
                                 chunk<NodeRef const> nr) {
                auto out         = l.column<0>();
                auto const slot  = nr.column<0>();
                auto const level = nr.column<1>();
                for (std::size_t r = 0; r < out.size(); ++r) {
                    auto const node = t->level_begin[level[r]] + slot[r];
                    if (node >= t->level_begin[level[r] + 1])
                        continue;
                    Lc acc {}; // assigned, not accumulated: no clearing pass
                    cplx const ct {t->cx[node], t->cy[node]};
                    for (std::uint32_t k = t->m2l_off[node]; k < t->m2l_off[node + 1];
                         ++k) {
                        auto const s2 = t->m2l_src[k];
                        Mp sm {};
                        sm.a = mp.level[t->level[s2]][s2 - t->level_begin[t->level[s2]]];
                        m2l(acc, sm, cplx {t->cx[s2], t->cy[s2]}, ct);
                    }
                    out[r] = acc.b;
                }
            });
        },
        grain {kNodeGrain});

    // 7. L2L, shallowest level first (registered after M2L, which writes Lc, so
    //    the chain orders itself).
    register_l2l(s, std::make_integer_sequence<int, kMaxLevel> {});

    // 8. Which leaf owns each particle. Writes LeafId, which `evaluate` reads --
    //    so those two order themselves with no phase.
    s.add_parallel("assign-leaf", [](Query<Key const, LeafId> q, Res<Tree> t) {
        q.for_each_chunk([&](std::span<Entity const>,
                             chunk<Key const> k,
                             chunk<LeafId> l) {
            auto const ks = k.column<0>();
            auto ls       = l.column<0>();
            for (std::size_t i = 0; i < ks.size(); ++i) {
                auto const it =
                    std::upper_bound(t->leaf_key_lo.begin(), t->leaf_key_lo.end(), ks[i]);
                ls[i] = static_cast<std::uint32_t>(it - t->leaf_key_lo.begin() - 1);
            }
        });
    });

    // 9. L2P + P2P, over PARTICLE rows: the near field has to be driven from the
    //    target particles, because a system may only write its own rows and leaf
    //    rows are not particle rows. chunk::row_begin() is how a particle finds
    //    its own index in the sorted array, to skip itself in the direct sum.
    s.add_parallel(
        "evaluate",
        [](Query<Field, LeafId const, Pos const> q,
           ColumnView<Pos, Charge> parts,
           ColumnView<Lc, NodeRef> locals,
           Res<Tree> t) {
            auto const pc = particle_cols(parts);
            LevelSpans<Lc> lc;
            lc.gather(locals);
            q.for_each_chunk([&](std::span<Entity const>,
                                 chunk<Field> f,
                                 chunk<LeafId const> l,
                                 chunk<Pos const> p) {
                auto const base = f.row_begin();
                auto fx         = f.column<0>();
                auto fy         = f.column<1>();
                auto fp         = f.column<2>();
                auto const lf   = l.column<0>();
                auto const x    = p.column<0>();
                auto const y    = p.column<1>();
                for (std::size_t i = 0; i < x.size(); ++i) {
                    auto const leaf = t->leaves[lf[i]];
                    auto const lv   = t->level[leaf];
                    cplx const z {x[i], y[i]};
                    double ax = 0, ay = 0, phi = 0;
                    Lc loc {};
                    loc.b = lc.level[lv][leaf - t->level_begin[lv]];
                    l2p(loc, z - cplx {t->cx[leaf], t->cy[leaf]}, ax, ay, phi);
                    std::size_t const self = base + i;
                    for (std::uint32_t k = t->p2p_off[leaf]; k < t->p2p_off[leaf + 1];
                         ++k) {
                        auto const src = t->p2p_src[k];
                        for (std::uint32_t j = t->pbegin[src]; j < t->pend[src]; ++j) {
                            if (j == self)
                                continue;
                            cplx const dz  = z - cplx {pc.x[j], pc.y[j]};
                            double const q = pc.q[j];
                            cplx const w   = q / dz;
                            ax += w.real();
                            ay -= w.imag();
                            phi += q * std::log(std::abs(dz));
                        }
                    }
                    fx[i] = ax;
                    fy[i] = ay;
                    fp[i] = phi;
                }
            });
        },
        grain {kParticleGrain});
}

// --- main ------------------------------------------------------------------
int main(int argc, char** argv) {
    auto const n             = argc > 1 ? std::strtoul(argv[1], nullptr, 10) : 50'000ul;
    auto const leaf_cap      = argc > 2 ? std::strtoul(argv[2], nullptr, 10) : 32ul;
    int const ticks          = argc > 3 ? std::atoi(argv[3]) : 3;
    unsigned const max_lanes = argc > 4
                                   ? unsigned(std::atoi(argv[4]))
                                   : std::max(1u, std::thread::hardware_concurrency());

    World world;
    world.emplace_resource<Tree>();
    world.emplace_resource<Pools>();
    world.emplace_resource<BuildStats>();

    {
        // A clustered (non-uniform) distribution, so the tree is genuinely
        // adaptive: a few dense blobs plus a uniform background.
        std::mt19937 rng {12345};
        std::uniform_real_distribution<double> u {0.0, 1.0};
        std::normal_distribution<double> g {0.0, 0.03};
        Schedule init;
        init.add_serial(
            "spawn",
            [&](Commands& cmd) {
                std::array<std::pair<double, double>, 4> hub {
                    {{0.22, 0.31}, {0.71, 0.19}, {0.55, 0.77}, {0.85, 0.62}}};
                for (std::uint32_t i = 0; i < n; ++i) {
                    double x, y;
                    if (i % 4 == 0) {
                        x = u(rng);
                        y = u(rng);
                    } else {
                        auto const& h = hub[(i / 4) % hub.size()];
                        x             = std::clamp(h.first + g(rng), 1e-6, 1.0 - 1e-6);
                        y             = std::clamp(h.second + g(rng), 1e-6, 1.0 - 1e-6);
                    }
                    cmd.spawn(Pos {x, y},
                              Charge {u(rng) * 2.0 - 1.0},
                              Field {},
                              Key {},
                              LeafId {});
                }
            },
            times {1});
        init.run(world);
    }
    std::printf("fmm2d: n=%zu leaf_cap=%zu p=%d buffer=%.2f max_level=%d\n",
                std::size_t(n),
                std::size_t(leaf_cap),
                kP,
                kBuffer,
                kMaxLevel);

    Schedule sched;
    build_schedule(sched, static_cast<std::uint32_t>(leaf_cap));
    std::printf("schedule: %zu systems, %zu waves\n", sched.size(), sched.level_count());

    // Optional per-system attribution (FMM_STATS=1): the library's own
    // observer, so the numbers in the write-up are the scheduler's, not ours.
    std::optional<diag::ScheduleReport> report;
    std::optional<event::Emitter<ScheduleEvent>::Subscription> report_sub;
    if (char const* v = std::getenv("FMM_STATS"); v && *v == '1') {
        report.emplace(diag::to_stream(std::cout), /*report_every_s=*/1.0);
        report_sub.emplace(sched.events().subscribe(std::ref(*report)));
    }

    for (unsigned lanes = 1; lanes <= max_lanes; lanes *= 2) {
        WorkerPool pool {lanes};
        sched.run(world, pool); // warm: pools spawn, caches fill
        sched.prewarm(world);
        auto const t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < ticks; ++i)
            sched.run(world, pool);
        auto const t1 = std::chrono::steady_clock::now();
        auto const ms =
            std::chrono::duration<double, std::milli>(t1 - t0).count() / ticks;
        auto const& st = world.resource<BuildStats>();
        if (report)
            report->flush();
        std::printf("lanes=%u  %.2f ms/tick   nodes=%u leaves=%u depth=%d "
                    "m2l=%u p2p=%u\n",
                    lanes,
                    ms,
                    st.nodes,
                    st.leaves,
                    st.depth,
                    st.m2l_pairs,
                    st.p2p_pairs);
    }

    // --- accuracy: compare against a direct O(N^2) sum on a random sample ---
    std::vector<double> px, py, pq, ffx, ffy, fphi;
    world.for_each_chunk<Pos, Charge, Field>([&](std::span<Entity const>,
                                                 chunk<Pos const> p,
                                                 chunk<Charge const> c,
                                                 chunk<Field const> f) {
        auto const x = p.column<0>();
        px.insert(px.end(), x.begin(), x.end());
        auto const y = p.column<1>();
        py.insert(py.end(), y.begin(), y.end());
        auto const q = c.column<0>();
        pq.insert(pq.end(), q.begin(), q.end());
        auto const a = f.column<0>();
        ffx.insert(ffx.end(), a.begin(), a.end());
        auto const b = f.column<1>();
        ffy.insert(ffy.end(), b.begin(), b.end());
        auto const h = f.column<2>();
        fphi.insert(fphi.end(), h.begin(), h.end());
    });
    std::mt19937 pick {7};
    std::uniform_int_distribution<std::size_t> which {0, px.size() - 1};
    double num = 0, den = 0, pnum = 0, pden = 0;
    int const samples = 200;
    for (int s = 0; s < samples; ++s) {
        auto const i = which(pick);
        cplx const z {px[i], py[i]};
        cplx w {};
        double phi = 0;
        for (std::size_t j = 0; j < px.size(); ++j) {
            if (j == i)
                continue;
            cplx const dz = z - cplx {px[j], py[j]};
            w += pq[j] / dz;
            phi += pq[j] * std::log(std::abs(dz));
        }
        double const ex = w.real(), ey = -w.imag();
        num += (ffx[i] - ex) * (ffx[i] - ex) + (ffy[i] - ey) * (ffy[i] - ey);
        den += ex * ex + ey * ey;
        pnum += (fphi[i] - phi) * (fphi[i] - phi);
        pden += phi * phi;
    }
    std::printf("accuracy vs direct (%d samples): force L2 rel err = %.3e, "
                "potential L2 rel err = %.3e\n",
                samples,
                std::sqrt(num / den),
                std::sqrt(pnum / pden));
    return 0;
}
