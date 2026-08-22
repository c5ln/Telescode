#include "algo/Graph.h"
#include "algo/Scoring.h"
#include "db/db.h"

#include <gtest/gtest.h>
#include <sqlite3.h>

// ── Graph::add_edge ───────────────────────────────────────────────────────────

TEST(GraphWeights, add_edge_keeps_adjacency_simple)
{
    Graph g;
    NodeId a = g.get_or_add("A");
    NodeId b = g.get_or_add("B");

    g.add_edge(a, b, 1.0);
    g.add_edge(a, b, 2.0);
    g.add_edge(a, b, 4.0);

    // Repeats accumulate into the weight; SCC/Brandes must not see parallel edges.
    ASSERT_EQ(g.adj[a].size(), 1u);
    ASSERT_EQ(g.radj[b].size(), 1u);
    EXPECT_NEAR(g.edge_weight(a, 0), 7.0, 1e-12);
    EXPECT_NEAR(g.out_weight(a), 7.0, 1e-12);
}

TEST(GraphWeights, add_edge_ignores_self_loops)
{
    Graph g;
    NodeId a = g.get_or_add("A");
    g.add_edge(a, a, 5.0);
    EXPECT_TRUE(g.adj[a].empty());
    EXPECT_NEAR(g.out_weight(a), 0.0, 1e-12);
}

TEST(GraphWeights, out_weight_sums_distinct_targets)
{
    Graph g;
    NodeId a = g.get_or_add("A");
    NodeId b = g.get_or_add("B");
    NodeId c = g.get_or_add("C");
    g.add_edge(a, b, 3.0);
    g.add_edge(a, c, 0.5);
    EXPECT_EQ(g.adj[a].size(), 2u);
    EXPECT_NEAR(g.out_weight(a), 3.5, 1e-12);
}

// A graph assembled by pushing into adj/radj directly leaves adj_w empty and
// must still behave as a plain unweighted graph.
TEST(GraphWeights, unpopulated_weights_fall_back_to_one)
{
    Graph g;
    NodeId a = g.get_or_add("A");
    NodeId b = g.get_or_add("B");
    NodeId c = g.get_or_add("C");
    g.adj[a].push_back(b);  g.radj[b].push_back(a);
    g.adj[a].push_back(c);  g.radj[c].push_back(a);

    EXPECT_NEAR(g.edge_weight(a, 0), 1.0, 1e-12);
    EXPECT_NEAR(g.edge_weight(a, 1), 1.0, 1e-12);
    EXPECT_NEAR(g.out_weight(a), 2.0, 1e-12);
    EXPECT_NEAR(g.out_weight(b), 0.0, 1e-12);
}

// ── Weighted PageRank ─────────────────────────────────────────────────────────

// All-equal weights must reproduce the unweighted result exactly.
TEST(GraphWeights, uniform_weights_match_unweighted_pagerank)
{
    AlgoConfig cfg;

    Graph plain;
    NodeId pa = plain.get_or_add("A");
    NodeId pb = plain.get_or_add("B");
    NodeId pc = plain.get_or_add("C");
    plain.adj[pa].push_back(pb); plain.radj[pb].push_back(pa);
    plain.adj[pa].push_back(pc); plain.radj[pc].push_back(pa);
    plain.adj[pb].push_back(pc); plain.radj[pc].push_back(pb);

    Graph weighted;
    NodeId wa = weighted.get_or_add("A");
    NodeId wb = weighted.get_or_add("B");
    NodeId wc = weighted.get_or_add("C");
    weighted.add_edge(wa, wb, 2.0);
    weighted.add_edge(wa, wc, 2.0);
    weighted.add_edge(wb, wc, 2.0);

    auto p = PageRank::compute(plain, cfg);
    auto w = PageRank::compute(weighted, cfg);
    ASSERT_EQ(p.size(), w.size());
    for (std::size_t i = 0; i < p.size(); ++i)
        EXPECT_NEAR(p[i], w[i], 1e-12);
}

// A→B weight 9 vs A→C weight 1: B must end up with the larger share.
// Without weights the two would be identical by symmetry.
TEST(GraphWeights, heavier_edge_receives_more_pagerank)
{
    AlgoConfig cfg;
    Graph g;
    NodeId a = g.get_or_add("A");
    NodeId b = g.get_or_add("B");
    NodeId c = g.get_or_add("C");
    g.add_edge(a, b, 9.0);
    g.add_edge(a, c, 1.0);

    auto pr = PageRank::compute(g, cfg);
    ASSERT_EQ(pr.size(), 3u);
    EXPECT_GT(pr[b], pr[c]);
}

// ── GraphBuilder edge weighting from the DB ───────────────────────────────────

namespace {

void exec(sqlite3* db, const char* sql)
{
    char* err = nullptr;
    ASSERT_EQ(sqlite3_exec(db, sql, nullptr, nullptr, &err), SQLITE_OK)
        << (err ? err : "") << " :: " << sql;
    sqlite3_free(err);
}

// Finds the weight of src→tgt in a built file graph, or -1 if absent.
double file_edge_weight(const Graph& g, const std::string& src, const std::string& tgt)
{
    auto su = g.id_to_node.find(src);
    auto tv = g.id_to_node.find(tgt);
    if (su == g.id_to_node.end() || tv == g.id_to_node.end()) return -1.0;
    for (std::size_t i = 0; i < g.adj[su->second].size(); ++i)
        if (g.adj[su->second][i] == tv->second)
            return g.edge_weight(su->second, i);
    return -1.0;
}

}  // namespace

// INHERITS > CALLS > IMPORTS, and repeated entity pairs raise the file edge weight.
TEST(GraphWeights, builder_weights_by_link_type_and_count)
{
    sqlite3* db = nullptr;
    ASSERT_EQ(initDb(":memory:", &db), SQLITE_OK);

    exec(db,
        "INSERT INTO file(file_id, file_name, language, raw_loc) VALUES"
        " ('a.py','a.py','python',10),"
        " ('b.py','b.py','python',10),"
        " ('c.py','c.py','python',10),"
        " ('d.py','d.py','python',10);");

    exec(db,
        "INSERT INTO link(source_id, target_id, link_type) VALUES"
        // a.py -> b.py: one INHERITS
        " ('a.py::A','b.py::B','INHERITS'),"
        // a.py -> c.py: one CALLS
        " ('a.py::f','c.py::g','CALLS'),"
        // a.py -> d.py: one IMPORTS
        " ('a.py','d','IMPORTS');");

    AlgoConfig cfg;
    cfg.edge_count_mode = 0;  // isolate the link-type weighting
    Graph g;
    {
        auto gbr = GraphBuilder::build(db, cfg);
        g = std::move(gbr.file_graph);
    }

    EXPECT_NEAR(file_edge_weight(g, "a.py", "b.py"), cfg.edge_w_inherits, 1e-12);
    EXPECT_NEAR(file_edge_weight(g, "a.py", "c.py"), cfg.edge_w_calls,    1e-12);
    EXPECT_NEAR(file_edge_weight(g, "a.py", "d.py"), cfg.edge_w_imports,  1e-12);

    sqlite3_close(db);
}

// Four distinct call pairs between the same two files must weigh more than one.
// (The old `SELECT DISTINCT` collapsed them into a single unit-weight edge.)
TEST(GraphWeights, builder_counts_call_multiplicity)
{
    sqlite3* db = nullptr;
    ASSERT_EQ(initDb(":memory:", &db), SQLITE_OK);

    exec(db,
        "INSERT INTO file(file_id, file_name, language, raw_loc) VALUES"
        " ('a.py','a.py','python',10),"
        " ('b.py','b.py','python',10),"
        " ('c.py','c.py','python',10);");

    exec(db,
        "INSERT INTO link(source_id, target_id, link_type) VALUES"
        " ('a.py::f1','b.py::g1','CALLS'),"
        " ('a.py::f2','b.py::g2','CALLS'),"
        " ('a.py::f3','b.py::g3','CALLS'),"
        " ('a.py::f4','b.py::g4','CALLS'),"
        " ('a.py::f1','c.py::h1','CALLS');");

    AlgoConfig cfg;
    cfg.edge_count_mode = 1;  // linear, so the expected value is exact

    Graph g;
    {
        auto gbr = GraphBuilder::build(db, cfg);
        g = std::move(gbr.file_graph);
    }

    EXPECT_NEAR(file_edge_weight(g, "a.py", "b.py"), 4.0 * cfg.edge_w_calls, 1e-12);
    EXPECT_NEAR(file_edge_weight(g, "a.py", "c.py"), 1.0 * cfg.edge_w_calls, 1e-12);
    // Still one edge, not four -- multiplicity lives in the weight.
    EXPECT_EQ(g.adj[g.id_to_node.at("a.py")].size(), 2u);

    sqlite3_close(db);
}

// edge_count_mode 0 ignores multiplicity entirely.
TEST(GraphWeights, builder_count_mode_zero_ignores_multiplicity)
{
    sqlite3* db = nullptr;
    ASSERT_EQ(initDb(":memory:", &db), SQLITE_OK);

    exec(db,
        "INSERT INTO file(file_id, file_name, language, raw_loc) VALUES"
        " ('a.py','a.py','python',10), ('b.py','b.py','python',10);");
    exec(db,
        "INSERT INTO link(source_id, target_id, link_type) VALUES"
        " ('a.py::f1','b.py::g1','CALLS'),"
        " ('a.py::f2','b.py::g2','CALLS'),"
        " ('a.py::f3','b.py::g3','CALLS');");

    AlgoConfig cfg;
    cfg.edge_count_mode = 0;
    Graph g;
    {
        auto gbr = GraphBuilder::build(db, cfg);
        g = std::move(gbr.file_graph);
    }
    EXPECT_NEAR(file_edge_weight(g, "a.py", "b.py"), cfg.edge_w_calls, 1e-12);

    sqlite3_close(db);
}

// Weights of different link types between the same file pair add up.
TEST(GraphWeights, builder_sums_weights_across_link_types)
{
    sqlite3* db = nullptr;
    ASSERT_EQ(initDb(":memory:", &db), SQLITE_OK);

    exec(db,
        "INSERT INTO file(file_id, file_name, language, raw_loc) VALUES"
        " ('a.py','a.py','python',10), ('b.py','b.py','python',10);");
    exec(db,
        "INSERT INTO link(source_id, target_id, link_type) VALUES"
        " ('a.py::A','b.py::B','INHERITS'),"
        " ('a.py::f','b.py::g','CALLS'),"
        " ('a.py','b','IMPORTS');");

    AlgoConfig cfg;
    cfg.edge_count_mode = 0;
    Graph g;
    {
        auto gbr = GraphBuilder::build(db, cfg);
        g = std::move(gbr.file_graph);
    }

    EXPECT_NEAR(file_edge_weight(g, "a.py", "b.py"),
                cfg.edge_w_inherits + cfg.edge_w_calls + cfg.edge_w_imports, 1e-12);
    EXPECT_EQ(g.adj[g.id_to_node.at("a.py")].size(), 1u);

    sqlite3_close(db);
}
