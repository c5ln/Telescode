#include "algo/AlgoRunner.h"
#include "algo/Graph.h"
#include "algo/Scoring.h"
#include "db/db.h"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <stdexcept>

namespace {

// A graph with a bit of everything: a hub, a cycle, and a dangling node.
Graph makeGraph()
{
    Graph g;
    NodeId a = g.get_or_add("a.py");
    NodeId b = g.get_or_add("b.py");
    NodeId c = g.get_or_add("c.py");
    NodeId d = g.get_or_add("d.py");
    NodeId e = g.get_or_add("e.py");   // dangling: no outgoing edges
    g.add_edge(a, b);
    g.add_edge(a, c);
    g.add_edge(b, c);
    g.add_edge(c, b);                  // b <-> c cycle
    g.add_edge(c, e);
    g.add_edge(d, a);
    return g;
}

double sum(const std::vector<double>& v)
{
    return std::accumulate(v.begin(), v.end(), 0.0);
}

}  // namespace

// ── 1. Reduction: a uniform seed must reproduce plain PageRank ────────────────
// If this ever fails, every Phase 0 baseline number measured with the pinned
// binary is invalid, so it guards everything else in this file.

TEST(PersonalizedPageRank, UniformTeleportReproducesPlainPageRank)
{
    Graph g = makeGraph();
    AlgoConfig cfg;
    const int N = g.size();

    std::vector<double> uniform(N, 1.0 / N);
    auto plain = PageRank::compute(g, cfg);
    auto ppr   = PageRank::compute(g, cfg, uniform);

    ASSERT_EQ(plain.size(), ppr.size());
    for (int i = 0; i < N; ++i)
        EXPECT_NEAR(plain[i], ppr[i], 1e-9) << "node " << i;
}

// The same must hold on a graph with weighted edges, since the weighted
// out-degree path is the one the shipped pipeline actually uses.
TEST(PersonalizedPageRank, UniformTeleportReproducesPlainPageRankOnWeightedGraph)
{
    Graph g;
    NodeId a = g.get_or_add("a.py");
    NodeId b = g.get_or_add("b.py");
    NodeId c = g.get_or_add("c.py");
    g.add_edge(a, b, 7.5);
    g.add_edge(a, c, 0.25);
    g.add_edge(b, c, 3.0);
    g.add_edge(c, a, 1.0);

    AlgoConfig cfg;
    std::vector<double> uniform(g.size(), 1.0 / g.size());
    auto plain = PageRank::compute(g, cfg);
    auto ppr   = PageRank::compute(g, cfg, uniform);

    ASSERT_EQ(plain.size(), ppr.size());
    for (std::size_t i = 0; i < plain.size(); ++i)
        EXPECT_NEAR(plain[i], ppr[i], 1e-9) << "node " << i;
}

// An empty teleport takes the untouched code path, so it must match bit for bit,
// not just to 1e-9 -- this is what keeps a rebuild reproducing the pinned
// bench/bin numbers exactly.
TEST(PersonalizedPageRank, EmptyTeleportIsBitIdenticalToPlainPageRank)
{
    Graph g = makeGraph();
    AlgoConfig cfg;
    auto plain = PageRank::compute(g, cfg);
    auto ppr   = PageRank::compute(g, cfg, {});
    ASSERT_EQ(plain.size(), ppr.size());
    for (std::size_t i = 0; i < plain.size(); ++i)
        EXPECT_DOUBLE_EQ(plain[i], ppr[i]) << "node " << i;
}

// A seed list that resolves to every node with equal weight is the same thing
// arriving through the public (file_id, weight) door.
TEST(PersonalizedPageRank, UniformSeedListReproducesPlainPageRank)
{
    Graph g = makeGraph();
    AlgoConfig cfg;

    std::vector<SeedEntry> seeds;
    for (const std::string& id : g.node_to_id) seeds.push_back({id, 1.0});

    auto plain = PageRank::compute(g, cfg);
    auto ppr   = PageRank::compute(g, cfg, PageRank::makeTeleport(g, seeds));

    ASSERT_EQ(plain.size(), ppr.size());
    for (std::size_t i = 0; i < plain.size(); ++i)
        EXPECT_NEAR(plain[i], ppr[i], 1e-9) << "node " << i;
}

// ── 2. Seed concentration ─────────────────────────────────────────────────────

// Seeding concentrates the teleport mass on the seed and what it reaches.
// Note what this does NOT mean: a reachable node is not guaranteed to score
// higher than it did under a uniform teleport. Under uniform teleport every
// node collects a free (1-d)/N share; a seeded walk takes that away, and for a
// node several damped hops from the seed the loss can exceed the inflow. The
// invariant is about *where the mass can go*, not about every node rising.

TEST(PersonalizedPageRank, SeedAbsorbsTeleportAndStarvesUnreachableNodes)
{
    Graph g = makeGraph();
    AlgoConfig cfg;
    const NodeId a = g.id_to_node.at("a.py");
    const NodeId d = g.id_to_node.at("d.py");

    auto plain = PageRank::compute(g, cfg);
    auto ppr   = PageRank::compute(g, cfg, PageRank::makeTeleport(g, {{"a.py", 1.0}}));

    EXPECT_GT(ppr[a], plain[a]);   // the seed absorbs the whole teleport
    // d.py depends on a.py, not the reverse, so nothing flows back up to it and
    // it no longer gets a teleport share either.
    EXPECT_LT(ppr[d], plain[d]);
    EXPECT_NEAR(ppr[d], 0.0, 1e-12);
}

// Everything the seed cannot reach must end up with no mass at all, so the whole
// distribution lives on the seed's forward closure.
TEST(PersonalizedPageRank, AllMassStaysInTheSeedsForwardClosure)
{
    Graph g = makeGraph();
    AlgoConfig cfg;
    cfg.convergence_eps = 1e-15;
    cfg.max_iter        = 500;

    auto ppr = PageRank::compute(g, cfg, PageRank::makeTeleport(g, {{"a.py", 1.0}}));

    // Reachable from a.py: a, b, c, e.  Not reachable: d.
    double reachable = ppr[g.id_to_node.at("a.py")] + ppr[g.id_to_node.at("b.py")]
                     + ppr[g.id_to_node.at("c.py")] + ppr[g.id_to_node.at("e.py")];
    EXPECT_NEAR(reachable, 1.0, 1e-9);
    EXPECT_NEAR(ppr[g.id_to_node.at("d.py")], 0.0, 1e-12);
}

// Along a plain chain the mass decays with distance from the seed.
TEST(PersonalizedPageRank, MassDecaysWithDistanceFromSeed)
{
    Graph g;
    NodeId a = g.get_or_add("a.py");
    NodeId b = g.get_or_add("b.py");
    NodeId c = g.get_or_add("c.py");
    g.add_edge(a, b);
    g.add_edge(b, c);

    AlgoConfig cfg;
    cfg.convergence_eps = 1e-15;
    cfg.max_iter        = 500;
    auto ppr = PageRank::compute(g, cfg, PageRank::makeTeleport(g, {{"a.py", 1.0}}));

    EXPECT_GT(ppr[a], ppr[b]);
    EXPECT_GT(ppr[b], ppr[c]);
    // Closed form for a→b→c with the sink's mass teleporting back to a:
    //   pr(a) = (1-d) + d*pr(c),  pr(b) = d*pr(a),  pr(c) = d*pr(b)
    //   => pr(a) = (1-d)/(1-d^3) with d=0.85
    const double d = cfg.damping;
    const double pa = (1.0 - d) / (1.0 - d * d * d);
    EXPECT_NEAR(ppr[a], pa,         1e-9);
    EXPECT_NEAR(ppr[b], d * pa,     1e-9);
    EXPECT_NEAR(ppr[c], d * d * pa, 1e-9);
}

// A node reachable from the seed must beat an identical node that is not.
// This is the property the whole phase exists for: structural relevance to the
// query, not global importance.
TEST(PersonalizedPageRank, ReachableBeatsSymmetricUnreachable)
{
    Graph g;
    NodeId s = g.get_or_add("seed.py");
    NodeId n = g.get_or_add("near.py");     // seed.py -> near.py
    NodeId o = g.get_or_add("other.py");    // other.py -> far.py, same shape
    NodeId f = g.get_or_add("far.py");
    g.add_edge(s, n);
    g.add_edge(o, f);

    AlgoConfig cfg;
    auto plain = PageRank::compute(g, cfg);
    // The two halves are structurally identical, so plain PageRank cannot tell
    // near.py from far.py at all.
    EXPECT_NEAR(plain[n], plain[f], 1e-12);

    auto ppr = PageRank::compute(g, cfg, PageRank::makeTeleport(g, {{"seed.py", 1.0}}));
    EXPECT_GT(ppr[n], ppr[f]);
    EXPECT_NEAR(ppr[f], 0.0, 1e-12);
    EXPECT_NEAR(ppr[o], 0.0, 1e-12);
}

// Different seeds must produce different rankings -- otherwise the walk is not
// personalized at all and the whole phase is a no-op.
TEST(PersonalizedPageRank, DifferentSeedsGiveDifferentRankings)
{
    Graph g = makeGraph();
    AlgoConfig cfg;
    auto from_a = PageRank::compute(g, cfg, PageRank::makeTeleport(g, {{"a.py", 1.0}}));
    auto from_d = PageRank::compute(g, cfg, PageRank::makeTeleport(g, {{"d.py", 1.0}}));

    bool differs = false;
    for (std::size_t i = 0; i < from_a.size(); ++i)
        if (std::fabs(from_a[i] - from_d[i]) > 1e-9) differs = true;
    EXPECT_TRUE(differs);

    // d.py is upstream of a.py: seeding d lifts a, seeding a leaves d at zero.
    EXPECT_GT(from_d[g.id_to_node.at("a.py")], 0.0);
    EXPECT_NEAR(from_a[g.id_to_node.at("d.py")], 0.0, 1e-12);
}

// Weight within a seed set matters, not just membership.
TEST(PersonalizedPageRank, SeedWeightsAreRespected)
{
    Graph g;
    NodeId a = g.get_or_add("a.py");
    NodeId b = g.get_or_add("b.py");
    g.get_or_add("c.py");
    g.add_edge(a, b);

    AlgoConfig cfg;
    auto heavy_a = PageRank::compute(g, cfg, PageRank::makeTeleport(g, {{"a.py", 9.0}, {"c.py", 1.0}}));
    auto heavy_c = PageRank::compute(g, cfg, PageRank::makeTeleport(g, {{"a.py", 1.0}, {"c.py", 9.0}}));

    EXPECT_GT(heavy_a[a], heavy_c[a]);
    EXPECT_GT(heavy_a[b], heavy_c[b]);
    EXPECT_GT(heavy_c[g.id_to_node.at("c.py")], heavy_a[g.id_to_node.at("c.py")]);
}

// ── 2b. Against an independent implementation ─────────────────────────────────

namespace {

// Dense power iteration written straight from the formula in Scoring.h, not
// derived from PageRank::compute -- a shared mistake cannot hide in both.
std::vector<double> referencePpr(const Graph& g, double d,
                                 const std::vector<double>& teleport, int iters)
{
    const int N = g.size();
    std::vector<std::vector<double>> W(N, std::vector<double>(N, 0.0));
    std::vector<double> outw(N, 0.0);
    for (int u = 0; u < N; ++u) {
        for (std::size_t i = 0; i < g.adj[u].size(); ++i) {
            const double w = g.edge_weight(static_cast<NodeId>(u), i);
            W[u][g.adj[u][i]] += w;
            outw[u] += w;
        }
    }

    std::vector<double> p = teleport;
    if (p.empty()) p.assign(N, 1.0 / N);
    double psum = std::accumulate(p.begin(), p.end(), 0.0);
    for (double& v : p) v /= psum;

    std::vector<double> pr(N, 1.0 / N);
    for (int it = 0; it < iters; ++it) {
        double dangling = 0.0;
        for (int u = 0; u < N; ++u) if (outw[u] <= 0.0) dangling += pr[u];

        std::vector<double> next(N, 0.0);
        for (int v = 0; v < N; ++v)
            next[v] = (1.0 - d) * p[v] + d * dangling * p[v];
        for (int u = 0; u < N; ++u) {
            if (outw[u] <= 0.0) continue;
            for (int v = 0; v < N; ++v)
                if (W[u][v] > 0.0) next[v] += d * pr[u] * W[u][v] / outw[u];
        }
        pr.swap(next);
    }
    return pr;
}

}  // namespace

TEST(PersonalizedPageRank, MatchesIndependentDenseReference)
{
    Graph g = makeGraph();
    AlgoConfig cfg;
    cfg.convergence_eps = 1e-15;
    cfg.max_iter        = 2000;

    struct Case { const char* name; std::vector<SeedEntry> seeds; };
    const std::vector<Case> cases = {
        {"uniform (no seed)", {}},
        {"single seed",       {{"a.py", 1.0}}},
        {"two seeds",         {{"a.py", 2.0}, {"e.py", 1.0}}},
        {"seed on the cycle", {{"b.py", 1.0}}},
        {"seed on the sink",  {{"e.py", 1.0}}},
    };

    for (const Case& c : cases) {
        auto p        = PageRank::makeTeleport(g, c.seeds);
        auto actual   = PageRank::compute(g, cfg, p);
        auto expected = referencePpr(g, cfg.damping, p, 2000);
        ASSERT_EQ(actual.size(), expected.size()) << c.name;
        for (std::size_t i = 0; i < actual.size(); ++i)
            EXPECT_NEAR(actual[i], expected[i], 1e-9) << c.name << " node " << i;
    }
}

TEST(PersonalizedPageRank, MatchesIndependentDenseReferenceOnWeightedGraph)
{
    Graph g;
    NodeId a = g.get_or_add("a.py");
    NodeId b = g.get_or_add("b.py");
    NodeId c = g.get_or_add("c.py");
    NodeId d = g.get_or_add("d.py");
    g.add_edge(a, b, 5.0);
    g.add_edge(a, c, 0.5);
    g.add_edge(b, c, 2.0);
    g.add_edge(c, a, 1.0);
    g.add_edge(c, d, 3.0);

    AlgoConfig cfg;
    cfg.convergence_eps = 1e-15;
    cfg.max_iter        = 2000;

    auto p        = PageRank::makeTeleport(g, {{"b.py", 3.0}, {"d.py", 1.0}});
    auto actual   = PageRank::compute(g, cfg, p);
    auto expected = referencePpr(g, cfg.damping, p, 2000);
    ASSERT_EQ(actual.size(), expected.size());
    for (std::size_t i = 0; i < actual.size(); ++i)
        EXPECT_NEAR(actual[i], expected[i], 1e-9) << "node " << i;
}

// ── 3. Normalization of the injected vector ───────────────────────────────────

TEST(PersonalizedPageRank, UnnormalizedTeleportIsNormalizedInternally)
{
    Graph g = makeGraph();
    AlgoConfig cfg;
    const int N = g.size();

    std::vector<double> p(N, 0.0);
    p[g.id_to_node.at("a.py")] = 3.0;
    p[g.id_to_node.at("d.py")] = 1.0;

    std::vector<double> scaled = p;
    for (double& v : scaled) v *= 1000.0;   // same distribution, sums to 4000

    auto a = PageRank::compute(g, cfg, p);
    auto b = PageRank::compute(g, cfg, scaled);
    ASSERT_EQ(a.size(), b.size());
    for (std::size_t i = 0; i < a.size(); ++i)
        EXPECT_NEAR(a[i], b[i], 1e-12) << "node " << i;
}

// The result is a distribution: it must still sum to 1.
TEST(PersonalizedPageRank, ResultSumsToOne)
{
    Graph g = makeGraph();
    AlgoConfig cfg;
    cfg.convergence_eps = 1e-15;   // run to a tight fixed point
    cfg.max_iter        = 500;

    EXPECT_NEAR(sum(PageRank::compute(g, cfg)), 1.0, 1e-9);
    EXPECT_NEAR(sum(PageRank::compute(g, cfg, PageRank::makeTeleport(g, {{"a.py", 1.0}}))),
                1.0, 1e-9);
    EXPECT_NEAR(sum(PageRank::compute(g, cfg, PageRank::makeTeleport(g, {{"c.py", 2.0}, {"e.py", 5.0}}))),
                1.0, 1e-9);
}

TEST(PersonalizedPageRank, MakeTeleportNormalizesToOne)
{
    Graph g = makeGraph();
    auto p = PageRank::makeTeleport(g, {{"a.py", 4.0}, {"b.py", 6.0}});
    ASSERT_EQ(p.size(), static_cast<std::size_t>(g.size()));
    EXPECT_NEAR(sum(p), 1.0, 1e-12);
    EXPECT_NEAR(p[g.id_to_node.at("a.py")], 0.4, 1e-12);
    EXPECT_NEAR(p[g.id_to_node.at("b.py")], 0.6, 1e-12);
    EXPECT_NEAR(p[g.id_to_node.at("c.py")], 0.0, 1e-12);
}

// ── 4. makeTeleport edge cases ────────────────────────────────────────────────

// Seed file_ids that are not in the graph are dropped, not an error: a BM25 seed
// list can name a path from a different snapshot, or an external module.
TEST(PersonalizedPageRank, UnknownFileIdsAreIgnored)
{
    Graph g = makeGraph();
    auto p = PageRank::makeTeleport(g, {{"a.py", 1.0}, {"nope/missing.py", 99.0}});
    ASSERT_EQ(p.size(), static_cast<std::size_t>(g.size()));
    EXPECT_NEAR(sum(p), 1.0, 1e-12);
    EXPECT_NEAR(p[g.id_to_node.at("a.py")], 1.0, 1e-12);
}

// Nothing matched -> empty -> compute() falls back to uniform, so a bad seed
// list degrades to plain PageRank instead of returning all zeros.
TEST(PersonalizedPageRank, NoMatchingSeedFallsBackToUniform)
{
    Graph g = makeGraph();
    AlgoConfig cfg;
    auto p = PageRank::makeTeleport(g, {{"nope.py", 1.0}, {"also/nope.py", 2.0}});
    EXPECT_TRUE(p.empty());

    auto plain = PageRank::compute(g, cfg);
    auto ppr   = PageRank::compute(g, cfg, p);
    for (std::size_t i = 0; i < plain.size(); ++i)
        EXPECT_DOUBLE_EQ(plain[i], ppr[i]);
}

TEST(PersonalizedPageRank, EmptySeedListGivesEmptyTeleport)
{
    Graph g = makeGraph();
    EXPECT_TRUE(PageRank::makeTeleport(g, {}).empty());
}

TEST(PersonalizedPageRank, NonPositiveSeedWeightsAreDropped)
{
    Graph g = makeGraph();
    auto p = PageRank::makeTeleport(g, {{"a.py", 1.0}, {"b.py", 0.0}, {"c.py", -5.0}});
    ASSERT_EQ(p.size(), static_cast<std::size_t>(g.size()));
    EXPECT_NEAR(p[g.id_to_node.at("a.py")], 1.0, 1e-12);
    EXPECT_NEAR(p[g.id_to_node.at("b.py")], 0.0, 1e-12);
    EXPECT_NEAR(p[g.id_to_node.at("c.py")], 0.0, 1e-12);
}

// The same file appearing twice accumulates rather than overwriting.
TEST(PersonalizedPageRank, RepeatedSeedFileAccumulates)
{
    Graph g = makeGraph();
    auto p = PageRank::makeTeleport(g, {{"a.py", 1.0}, {"a.py", 3.0}, {"b.py", 4.0}});
    EXPECT_NEAR(p[g.id_to_node.at("a.py")], 0.5, 1e-12);
    EXPECT_NEAR(p[g.id_to_node.at("b.py")], 0.5, 1e-12);
}

TEST(PersonalizedPageRank, EmptyGraphGivesEmptyResults)
{
    Graph g;
    AlgoConfig cfg;
    EXPECT_TRUE(PageRank::makeTeleport(g, {{"a.py", 1.0}}).empty());
    EXPECT_TRUE(PageRank::compute(g, cfg, {}).empty());
}

// ── 5. Wiring errors are loud ─────────────────────────────────────────────────

TEST(PersonalizedPageRank, WrongSizedTeleportThrows)
{
    Graph g = makeGraph();
    AlgoConfig cfg;
    EXPECT_THROW(PageRank::compute(g, cfg, std::vector<double>(g.size() - 1, 0.2)),
                 std::invalid_argument);
    EXPECT_THROW(PageRank::compute(g, cfg, std::vector<double>(g.size() + 1, 0.1)),
                 std::invalid_argument);
}

TEST(PersonalizedPageRank, NegativeTeleportEntryThrows)
{
    Graph g = makeGraph();
    AlgoConfig cfg;
    std::vector<double> p(g.size(), 0.5);
    p[0] = -1.0;
    EXPECT_THROW(PageRank::compute(g, cfg, p), std::invalid_argument);
}

// All-zero is degenerate rather than malformed: fall back to uniform.
TEST(PersonalizedPageRank, AllZeroTeleportFallsBackToUniform)
{
    Graph g = makeGraph();
    AlgoConfig cfg;
    auto plain = PageRank::compute(g, cfg);
    auto ppr   = PageRank::compute(g, cfg, std::vector<double>(g.size(), 0.0));
    ASSERT_EQ(plain.size(), ppr.size());
    for (std::size_t i = 0; i < plain.size(); ++i)
        EXPECT_DOUBLE_EQ(plain[i], ppr[i]);
}

// ── 6. DB-level injection path (AlgoRunner::personalizedPageRank) ─────────────

namespace {

void exec(sqlite3* db, const char* sql)
{
    char* err = nullptr;
    ASSERT_EQ(sqlite3_exec(db, sql, nullptr, nullptr, &err), SQLITE_OK) << (err ? err : "");
    sqlite3_free(err);
}

// a.py -> b.py -> c.py, plus an unrelated z.py and an unresolvable external import.
void seedDb(sqlite3* db)
{
    exec(db,
        "INSERT INTO file(file_id, file_name, language, raw_loc) VALUES"
        " ('a.py','a.py','python',10),"
        " ('b.py','b.py','python',10),"
        " ('c.py','c.py','python',10),"
        " ('z.py','z.py','python',10);");
    exec(db,
        "INSERT INTO link(source_id, target_id, link_type) VALUES"
        " ('a.py::f','b.py::g','CALLS'),"
        " ('b.py::g','c.py::h','CALLS'),"
        " ('a.py','requests','IMPORTS');");   // external, unresolvable
}

double scoreOf(const std::vector<PprEntry>& v, const std::string& fid)
{
    for (const PprEntry& e : v) if (e.file_id == fid) return e.ppr;
    return -1.0;
}

}  // namespace

TEST(PersonalizedPageRankDb, EndToEndOnTempDb)
{
    const std::string path = ::testing::TempDir() + "ppr_test.db";
    std::remove(path.c_str());

    sqlite3* db = nullptr;
    ASSERT_EQ(initDb(path.c_str(), &db), SQLITE_OK);
    seedDb(db);
    sqlite3_close(db);

    AlgoConfig cfg;
    auto seeded = AlgoRunner::personalizedPageRank(path.c_str(), {{"a.py", 1.0}}, cfg);
    auto plain  = AlgoRunner::personalizedPageRank(path.c_str(), {}, cfg);

    // Only project files come back; the unresolved "requests" import target does not.
    ASSERT_EQ(seeded.size(), 4u);
    for (const PprEntry& e : seeded)
        EXPECT_NE(e.file_id, "requests");

    // Seeding a.py absorbs the teleport and starves z.py, which nothing reaches.
    EXPECT_GT(scoreOf(seeded, "a.py"), scoreOf(plain, "a.py"));
    EXPECT_NEAR(scoreOf(seeded, "z.py"), 0.0, 1e-12);
    EXPECT_GT(scoreOf(plain, "z.py"), 0.0);

    // The chain a -> b -> c keeps essentially all of the mass, decaying by hop.
    EXPECT_NEAR(scoreOf(seeded, "a.py") + scoreOf(seeded, "b.py") + scoreOf(seeded, "c.py"),
                1.0, 1e-6);
    EXPECT_GT(scoreOf(seeded, "a.py"), scoreOf(seeded, "b.py"));
    EXPECT_GT(scoreOf(seeded, "b.py"), scoreOf(seeded, "c.py"));

    // Plain PageRank ranks the sink first; seeding at a.py puts a.py first.
    // Same graph, different question -- that is the point of the phase.
    EXPECT_EQ(plain.front().file_id,  "c.py");
    EXPECT_EQ(seeded.front().file_id, "a.py");

    // Sorted by ppr descending.
    for (std::size_t i = 1; i < seeded.size(); ++i)
        EXPECT_GE(seeded[i - 1].ppr, seeded[i].ppr);

    std::remove(path.c_str());
}

// An empty seed list through the DB path must equal plain PageRank on the same
// graph -- the reduction guarantee, checked at the outer seam too.
TEST(PersonalizedPageRankDb, EmptySeedMatchesPlainPageRank)
{
    const std::string path = ::testing::TempDir() + "ppr_reduce.db";
    std::remove(path.c_str());

    sqlite3* db = nullptr;
    ASSERT_EQ(initDb(path.c_str(), &db), SQLITE_OK);
    seedDb(db);
    auto gbr = GraphBuilder::build(db, AlgoConfig{});
    sqlite3_close(db);

    AlgoConfig cfg;
    auto expected = PageRank::compute(gbr.file_graph, cfg);
    auto actual   = AlgoRunner::personalizedPageRank(path.c_str(), {}, cfg);

    for (const PprEntry& e : actual) {
        NodeId nid = gbr.file_graph.id_to_node.at(e.file_id);
        EXPECT_NEAR(e.ppr, expected[nid], 1e-12) << e.file_id;
    }

    std::remove(path.c_str());
}

// Seeds naming files that are not in this repo snapshot must not break the run.
TEST(PersonalizedPageRankDb, AllUnknownSeedsDegradeToPlainPageRank)
{
    const std::string path = ::testing::TempDir() + "ppr_unknown.db";
    std::remove(path.c_str());

    sqlite3* db = nullptr;
    ASSERT_EQ(initDb(path.c_str(), &db), SQLITE_OK);
    seedDb(db);
    sqlite3_close(db);

    AlgoConfig cfg;
    auto plain   = AlgoRunner::personalizedPageRank(path.c_str(), {}, cfg);
    auto unknown = AlgoRunner::personalizedPageRank(
        path.c_str(), {{"other/repo/file.py", 5.0}}, cfg);

    ASSERT_EQ(plain.size(), unknown.size());
    for (std::size_t i = 0; i < plain.size(); ++i) {
        EXPECT_EQ(plain[i].file_id, unknown[i].file_id);
        EXPECT_DOUBLE_EQ(plain[i].ppr, unknown[i].ppr);
    }

    std::remove(path.c_str());
}
