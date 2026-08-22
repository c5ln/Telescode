#include "algo/Scoring.h"
#include "algo/Graph.h"
#include "algo/AlgoConfig.h"
#include <gtest/gtest.h>
#include <numeric>
#include <cmath>

static void add_edge(Graph& g, NodeId u, NodeId v)
{
    g.adj[u].push_back(v);
    g.radj[v].push_back(u);
}

static AlgoConfig tight_cfg()
{
    AlgoConfig cfg;
    cfg.damping         = 0.85;
    cfg.max_iter        = 200;
    cfg.convergence_eps = 1e-9;
    return cfg;
}

static double sum(const std::vector<double>& v)
{
    return std::accumulate(v.begin(), v.end(), 0.0);
}

// 빈 그래프
TEST(PageRank, empty_graph)
{
    Graph g;
    auto pr = PageRank::compute(g, tight_cfg());
    EXPECT_TRUE(pr.empty());
}

// 단일 노드
TEST(PageRank, single_node_sums_to_one)
{
    Graph g;
    g.get_or_add("A");
    auto pr = PageRank::compute(g, tight_cfg());
    ASSERT_EQ(pr.size(), 1u);
    EXPECT_NEAR(pr[0], 1.0, 1e-6);
}

// 합이 항상 1
TEST(PageRank, two_isolated_nodes_sum_to_one)
{
    Graph g;
    g.get_or_add("A");
    g.get_or_add("B");
    auto pr = PageRank::compute(g, tight_cfg());
    ASSERT_EQ(pr.size(), 2u);
    EXPECT_NEAR(sum(pr), 1.0, 1e-6);
    // Symmetry: both dangling, equal mass
    EXPECT_NEAR(pr[0], pr[1], 1e-6);
}

// 합이 항상 1, 임의 DAG 포함
TEST(PageRank, sum_is_one_on_arbitrary_dag)
{
    Graph g;
    NodeId a = g.get_or_add("A");
    NodeId b = g.get_or_add("B");
    NodeId c = g.get_or_add("C");
    NodeId d = g.get_or_add("D");
    add_edge(g, a, b);
    add_edge(g, a, c);
    add_edge(g, b, d);
    add_edge(g, c, d);
    auto pr = PageRank::compute(g, tight_cfg());
    EXPECT_NEAR(sum(pr), 1.0, 1e-6);
}

// A imports B 이면 B가 더 높은 순위
TEST(PageRank, importee_ranks_higher)
{
    Graph g;
    NodeId a = g.get_or_add("A");
    NodeId b = g.get_or_add("B");
    add_edge(g, a, b);  // A depends on B

    auto pr = PageRank::compute(g, tight_cfg());
    ASSERT_EQ(pr.size(), 2u);
    EXPECT_NEAR(sum(pr), 1.0, 1e-6);
    EXPECT_GT(pr[b], pr[a]);
}

// 대칭성, 양방향 엣지는 동일 순위
TEST(PageRank, bidirectional_edge_gives_equal_rank)
{
    Graph g;
    NodeId a = g.get_or_add("A");
    NodeId b = g.get_or_add("B");
    add_edge(g, a, b);
    add_edge(g, b, a);

    auto pr = PageRank::compute(g, tight_cfg());
    ASSERT_EQ(pr.size(), 2u);
    EXPECT_NEAR(pr[a], pr[b], 1e-6);
}

// 리프가 허브보다 높음
TEST(PageRank, hub_importees_rank_higher_than_hub)
{
    Graph g;
    NodeId hub = g.get_or_add("hub");
    NodeId a   = g.get_or_add("A");
    NodeId b   = g.get_or_add("B");
    NodeId c   = g.get_or_add("C");
    add_edge(g, hub, a);
    add_edge(g, hub, b);
    add_edge(g, hub, c);

    auto pr = PageRank::compute(g, tight_cfg());
    ASSERT_EQ(pr.size(), 4u);
    EXPECT_NEAR(sum(pr), 1.0, 1e-6);
    // Symmetry among importees
    EXPECT_NEAR(pr[a], pr[b], 1e-6);
    EXPECT_NEAR(pr[b], pr[c], 1e-6);
    // Importees rank higher than the hub
    EXPECT_GT(pr[a], pr[hub]);
}

// A->B->C이면 C > B > A 순서로 우선
TEST(PageRank, chain_rank_order)
{
    Graph g;
    NodeId a = g.get_or_add("A");
    NodeId b = g.get_or_add("B");
    NodeId c = g.get_or_add("C");
    add_edge(g, a, b);
    add_edge(g, b, c);

    auto pr = PageRank::compute(g, tight_cfg());
    ASSERT_EQ(pr.size(), 3u);
    EXPECT_NEAR(sum(pr), 1.0, 1e-6);
    EXPECT_GT(pr[c], pr[b]);
    EXPECT_GT(pr[b], pr[a]);
}
