#include "algo/Scoring.h"
#include "algo/Graph.h"
#include "algo/AlgoConfig.h"
#include <gtest/gtest.h>

static void add_edge(Graph& g, NodeId u, NodeId v)
{
    g.adj[u].push_back(v);
    g.radj[v].push_back(u);
}

// 경계값 
TEST(ExactBrandes, empty_graph)
{
    Graph g;
    auto bc = ExactBrandesStrategy{}.compute(g);
    EXPECT_TRUE(bc.empty());
}

TEST(ExactBrandes, single_node)
{
    Graph g;
    g.get_or_add("A");
    auto bc = ExactBrandesStrategy{}.compute(g);
    ASSERT_EQ(bc.size(), 1u);
    EXPECT_DOUBLE_EQ(bc[0], 0.0);
}

TEST(ExactBrandes, two_nodes_no_edges)
{
    Graph g;
    g.get_or_add("A");
    g.get_or_add("B");
    auto bc = ExactBrandesStrategy{}.compute(g);
    ASSERT_EQ(bc.size(), 2u);
    EXPECT_DOUBLE_EQ(bc[0], 0.0);
    EXPECT_DOUBLE_EQ(bc[1], 0.0);
}

// A->B: 경로는 있지만 중간 노드가 없음 -> BC 모두 0
TEST(ExactBrandes, single_directed_edge_no_intermediary)
{
    Graph g;
    NodeId a = g.get_or_add("A");
    NodeId b = g.get_or_add("B");
    add_edge(g, a, b);
    auto bc = ExactBrandesStrategy{}.compute(g);
    ASSERT_EQ(bc.size(), 2u);
    EXPECT_DOUBLE_EQ(bc[a], 0.0);
    EXPECT_DOUBLE_EQ(bc[b], 0.0);
}

// A->B->C: B가 A->C 경로의 유일한 중간 노드 → bc[B]=1, 나머지 0
TEST(ExactBrandes, linear_chain_3_nodes)
{
    Graph g;
    NodeId a = g.get_or_add("A");
    NodeId b = g.get_or_add("B");
    NodeId c = g.get_or_add("C");
    add_edge(g, a, b);
    add_edge(g, b, c);

    auto bc = ExactBrandesStrategy{}.compute(g);
    ASSERT_EQ(bc.size(), 3u);
    EXPECT_DOUBLE_EQ(bc[a], 0.0);
    EXPECT_NEAR(bc[b], 1.0, 1e-9);
    EXPECT_DOUBLE_EQ(bc[c], 0.0);
}

// 다이아몬드: A->B, A->C, B->D, C->D
// A→D의 최단 경로가 2개(A-B-D, A-C-D)이므로 B와 C는 각 0.5씩 분담
TEST(ExactBrandes, diamond_graph_symmetric)
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

    auto bc = ExactBrandesStrategy{}.compute(g);
    ASSERT_EQ(bc.size(), 4u);
    EXPECT_DOUBLE_EQ(bc[a], 0.0);
    EXPECT_NEAR(bc[b], 0.5, 1e-9);
    EXPECT_NEAR(bc[c], 0.5, 1e-9);
    EXPECT_DOUBLE_EQ(bc[d], 0.0);
}

// 4노드 체인 A->B->C->D
// B: A->C, A->D 경로에 등장 -> bc[B]=2
// C: A->D, B->D 경로에 등장 -> bc[C]=2
TEST(ExactBrandes, linear_chain_4_nodes)
{
    Graph g;
    NodeId a = g.get_or_add("A");
    NodeId b = g.get_or_add("B");
    NodeId c = g.get_or_add("C");
    NodeId d = g.get_or_add("D");
    add_edge(g, a, b);
    add_edge(g, b, c);
    add_edge(g, c, d);

    auto bc = ExactBrandesStrategy{}.compute(g);
    ASSERT_EQ(bc.size(), 4u);
    EXPECT_DOUBLE_EQ(bc[a], 0.0);
    EXPECT_NEAR(bc[b], 2.0, 1e-9);
    EXPECT_NEAR(bc[c], 2.0, 1e-9);
    EXPECT_DOUBLE_EQ(bc[d], 0.0);
}

// 병렬 경로가 없는 방향 그래프: A->B, A->C (B, C는 독립)
// B와 C는 어떤 최단 경로의 중간에도 없음 → BC 모두 0
TEST(ExactBrandes, fork_no_intermediaries)
{
    Graph g;
    NodeId a = g.get_or_add("A");
    NodeId b = g.get_or_add("B");
    NodeId c = g.get_or_add("C");
    add_edge(g, a, b);
    add_edge(g, a, c);

    auto bc = ExactBrandesStrategy{}.compute(g);
    ASSERT_EQ(bc.size(), 3u);
    EXPECT_DOUBLE_EQ(bc[a], 0.0);
    EXPECT_DOUBLE_EQ(bc[b], 0.0);
    EXPECT_DOUBLE_EQ(bc[c], 0.0);
}

// ── SamplingBrandesStrategy ───────────────────────────────────────────────────

// k >= N이면 모든 소스를 사용하므로 Exact와 동일한 결과
TEST(SamplingBrandes, full_sample_matches_exact)
{
    Graph g;
    NodeId a = g.get_or_add("A");
    NodeId b = g.get_or_add("B");
    NodeId c = g.get_or_add("C");
    add_edge(g, a, b);
    add_edge(g, b, c);

    auto exact    = ExactBrandesStrategy{}.compute(g);
    auto sampling = SamplingBrandesStrategy{/*k=*/100, /*seed=*/42}.compute(g);

    ASSERT_EQ(exact.size(), sampling.size());
    for (std::size_t i = 0; i < exact.size(); ++i)
        EXPECT_NEAR(sampling[i], exact[i], 1e-9);
}

// k < N이면 스케일링된 근사값: 순서 관계(대소)는 Exact와 일치해야 함
TEST(SamplingBrandes, partial_sample_preserves_order)
{
    // 체인 A→B→C→D→E: 중간 노드일수록 BC가 더 높음
    Graph g;
    NodeId a = g.get_or_add("A");
    NodeId b = g.get_or_add("B");
    NodeId c = g.get_or_add("C");
    NodeId d = g.get_or_add("D");
    NodeId e = g.get_or_add("E");
    add_edge(g, a, b);
    add_edge(g, b, c);
    add_edge(g, c, d);
    add_edge(g, d, e);

    // k=3(N=5)이면 스케일링됨
    SamplingBrandesStrategy strat{3, 42};
    auto bc = strat.compute(g);
    ASSERT_EQ(bc.size(), 5u);

    // 끝 노드는 항상 BC=0, 내부 노드는 양수여야 함
    EXPECT_DOUBLE_EQ(bc[a], 0.0);
    EXPECT_GT(bc[b], 0.0);
    EXPECT_GT(bc[c], 0.0);
    EXPECT_GT(bc[d], 0.0);
    EXPECT_DOUBLE_EQ(bc[e], 0.0);
}

// 샘플링은 재현 가능해야 함 (같은 seed → 같은 결과)
TEST(SamplingBrandes, reproducible_with_same_seed)
{
    Graph g;
    for (int i = 0; i < 8; ++i) g.get_or_add(std::to_string(i));
    for (int i = 0; i < 7; ++i) add_edge(g, i, i + 1);

    auto r1 = SamplingBrandesStrategy{4, 123}.compute(g);
    auto r2 = SamplingBrandesStrategy{4, 123}.compute(g);
    ASSERT_EQ(r1.size(), r2.size());
    for (std::size_t i = 0; i < r1.size(); ++i)
        EXPECT_DOUBLE_EQ(r1[i], r2[i]);
}

// ── make_bc_strategy: 전략 선택 분기 ─────────────────────────────────────────

TEST(MakeBcStrategy, file_small_v_selects_exact)
{
    AlgoConfig cfg;  // bc_p1_exact_v=200
    auto strat = make_bc_strategy(10, cfg, PassLevel::File);
    EXPECT_NE(dynamic_cast<ExactBrandesStrategy*>(strat.get()), nullptr);
}

TEST(MakeBcStrategy, file_medium_v_selects_sampling)
{
    AlgoConfig cfg;  // 200 <= V < 2000
    auto strat = make_bc_strategy(400, cfg, PassLevel::File);
    auto* s = dynamic_cast<SamplingBrandesStrategy*>(strat.get());
    ASSERT_NE(s, nullptr);
    // k = max(bc_k_min=50, ceil(sqrt(400))=20) = 50
    EXPECT_EQ(s->k, 50);
    EXPECT_EQ(s->seed, cfg.bc_seed);
}

TEST(MakeBcStrategy, file_large_v_selects_sampling_fixed_k)
{
    AlgoConfig cfg;  // V >= bc_p1_large_v=2000
    auto strat = make_bc_strategy(3000, cfg, PassLevel::File);
    auto* s = dynamic_cast<SamplingBrandesStrategy*>(strat.get());
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->k, cfg.bc_p1_fixed_k);  // 64
}

TEST(MakeBcStrategy, function_small_v_selects_exact)
{
    AlgoConfig cfg;  // bc_p2_exact_v=500
    auto strat = make_bc_strategy(100, cfg, PassLevel::Function);
    EXPECT_NE(dynamic_cast<ExactBrandesStrategy*>(strat.get()), nullptr);
}

TEST(MakeBcStrategy, function_large_v_selects_sampling_fixed_k)
{
    AlgoConfig cfg;  // V >= bc_p2_large_v=5000
    auto strat = make_bc_strategy(6000, cfg, PassLevel::Function);
    auto* s = dynamic_cast<SamplingBrandesStrategy*>(strat.get());
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->k, cfg.bc_p2_fixed_k);  // 32
}

TEST(MakeBcStrategy, function_disable_p2_bc_selects_zero_strategy)
{
    AlgoConfig cfg;
    cfg.enable_p2_bc = false;
    auto strat = make_bc_strategy(1000, cfg, PassLevel::Function);

    // 타입: ZeroBCStrategy여야 함 (ExactBrandesStrategy가 아님)
    EXPECT_NE(dynamic_cast<ZeroBCStrategy*>(strat.get()), nullptr);
    EXPECT_EQ(dynamic_cast<ExactBrandesStrategy*>(strat.get()), nullptr);

    // 값: 비자명 그래프(체인)에서도 모두 0 반환
    Graph g;
    NodeId a = g.get_or_add("A");
    NodeId b = g.get_or_add("B");
    NodeId c = g.get_or_add("C");
    g.adj[a].push_back(b); g.radj[b].push_back(a);
    g.adj[b].push_back(c); g.radj[c].push_back(b);

    auto bc = strat->compute(g);
    ASSERT_EQ(bc.size(), 3u);
    for (double v : bc)
        EXPECT_DOUBLE_EQ(v, 0.0);
}
