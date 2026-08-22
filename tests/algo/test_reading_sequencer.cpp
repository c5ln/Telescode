#include "algo/ReadingSequencer.h"
#include "algo/Graph.h"
#include <gtest/gtest.h>

// Adds a directed edge u->v (u depends on v) and keeps adj/radj consistent.
static void add_edge(Graph& g, NodeId u, NodeId v)
{
    g.adj[u].push_back(v);
    g.radj[v].push_back(u);
}

TEST(ReadingSequencer, empty_graph)
{
    Graph g;
    auto result = ReadingSequencer::sequence(g, {}, {});
    EXPECT_TRUE(result.empty());
}

TEST(ReadingSequencer, single_node)
{
    Graph g;
    NodeId a = g.get_or_add("A");
    auto result = ReadingSequencer::sequence(g, {1.0}, {});
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0], a);
}

// A->B->C: A depends on B, B depends on C.
// C has no outgoing deps. 그럼 순서는 C,B,A 순서
TEST(ReadingSequencer, linear_chain)
{
    Graph g;
    NodeId a = g.get_or_add("A");
    NodeId b = g.get_or_add("B");
    NodeId c = g.get_or_add("C");
    add_edge(g, a, b);
    add_edge(g, b, c);

    auto result = ReadingSequencer::sequence(g, {1.0, 1.0, 1.0}, {});
    ASSERT_EQ(result.size(), 3u);
    EXPECT_EQ(result[0], c);
    EXPECT_EQ(result[1], b);
    EXPECT_EQ(result[2], a);
}

// 점수 높은 거 먼저 읽기
TEST(ReadingSequencer, score_ordering_no_edges)
{
    Graph g;
    NodeId lo = g.get_or_add("lo");  // score 1.0
    NodeId hi = g.get_or_add("hi");  // score 2.0

    auto result = ReadingSequencer::sequence(g, {1.0, 2.0}, {});
    ASSERT_EQ(result.size(), 2u);
    EXPECT_EQ(result[0], hi);
    EXPECT_EQ(result[1], lo);
}

// Diamond: A depends on B and C; B and C both depend on D.
// Read order: D -> (B or C by score) -> A.
// score: B=2.0, C=1.0 -> 순서는 D, B, C, A.
TEST(ReadingSequencer, diamond_score_ordering)
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

    // scores indexed by NodeId: A=0, B=2, C=1, D=0.5
    auto result = ReadingSequencer::sequence(g, {0.0, 2.0, 1.0, 0.5}, {});
    ASSERT_EQ(result.size(), 4u);
    EXPECT_EQ(result[0], d);
    EXPECT_EQ(result[1], b);
    EXPECT_EQ(result[2], c);
    EXPECT_EQ(result[3], a);
}

// ── SCC (cycle) handling ──────────────────────────────────────────────────────

// A<->B cycle; A also depends on C (A->C).
// SCC {A,B} depends on C. C read first, then {A,B}.
// Within SCC: B has higher score → B before A.
TEST(ReadingSequencer, cycle_scc_ordering)
{
    Graph g;
    NodeId a = g.get_or_add("A");
    NodeId b = g.get_or_add("B");
    NodeId c = g.get_or_add("C");
    add_edge(g, a, b);
    add_edge(g, b, a);  // cycle
    add_edge(g, a, c);  // A also depends on C

    auto result = ReadingSequencer::sequence(g, {1.0, 2.0, 0.5}, {});
    ASSERT_EQ(result.size(), 3u);
    EXPECT_EQ(result[0], c);
    // B (score 2.0) before A (score 1.0) within the SCC
    EXPECT_EQ(result[1], b);
    EXPECT_EQ(result[2], a);
}

// ── Tie-breaking ──────────────────────────────────────────────────────────────

// Within a cycle SCC, same score → cost_hint asc decides order.
// A and B in a cycle, same score 1.0. complexity: A=0.9, B=0.2 → B (easier) first.
TEST(ReadingSequencer, tie_break_cost_hint_prefers_cheaper)
{
    Graph g;
    NodeId a = g.get_or_add("A");
    NodeId b = g.get_or_add("B");
    add_edge(g, a, b);
    add_edge(g, b, a);

    std::vector<double> scores = {1.0, 1.0};
    std::vector<double> cost   = {0.9, 0.2};  // A=0.9, B=0.2
    auto result = ReadingSequencer::sequence(g, scores, cost);
    ASSERT_EQ(result.size(), 2u);
    EXPECT_EQ(result[0], b);  // cheaper to understand first
    EXPECT_EQ(result[1], a);
}

// cost_hint never overrides combined_score: A is harder but strictly more
// important, so it still comes first.
TEST(ReadingSequencer, cost_hint_does_not_override_score)
{
    Graph g;
    NodeId a = g.get_or_add("A");
    NodeId b = g.get_or_add("B");
    add_edge(g, a, b);
    add_edge(g, b, a);

    std::vector<double> scores = {2.0, 1.0};
    std::vector<double> cost   = {0.9, 0.1};
    auto result = ReadingSequencer::sequence(g, scores, cost);
    ASSERT_EQ(result.size(), 2u);
    EXPECT_EQ(result[0], a);
    EXPECT_EQ(result[1], b);
}

// A shorter cost_hint than the graph (or an empty one) must not read past the
// end; missing entries fall back to 0 and the name tie-break decides.
TEST(ReadingSequencer, tie_break_cost_hint_shorter_than_graph)
{
    Graph g;
    NodeId a = g.get_or_add("A");
    NodeId b = g.get_or_add("B");
    add_edge(g, a, b);
    add_edge(g, b, a);

    std::vector<double> cost = {0.5};  // only A has a cost
    auto result = ReadingSequencer::sequence(g, {1.0, 1.0}, cost);
    ASSERT_EQ(result.size(), 2u);
    EXPECT_EQ(result[0], b);  // B's missing cost reads as 0.0 < 0.5
    EXPECT_EQ(result[1], a);
}

// Within a cycle SCC, same score, same cost → alphabetical name asc.
// A and B in a cycle → "A" < "B", so A first.
TEST(ReadingSequencer, tie_break_node_name)
{
    Graph g;
    NodeId a = g.get_or_add("A");
    NodeId b = g.get_or_add("B");
    add_edge(g, a, b);
    add_edge(g, b, a);

    auto result = ReadingSequencer::sequence(g, {1.0, 1.0}, {});
    ASSERT_EQ(result.size(), 2u);
    EXPECT_EQ(result[0], a);  // "A" < "B"
    EXPECT_EQ(result[1], b);
}
