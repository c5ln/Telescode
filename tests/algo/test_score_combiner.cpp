#include "algo/Scoring.h"
#include <gtest/gtest.h>

// 빈 인풋
TEST(ScoreCombiner, empty_input)
{
    auto result = ScoreCombiner::combine({}, {}, 0.6, 0.4);
    EXPECT_TRUE(result.empty());
}

// 단일 원소
TEST(ScoreCombiner, single_element_percentile_gives_zero)
{
    // Percentile rank of a lone value is 0 by definition (nothing ranks below it),
    // so both terms are 0 regardless of alpha/beta.
    auto result = ScoreCombiner::combine({3.0}, {7.0}, 0.6, 0.4);
    ASSERT_EQ(result.size(), 1u);
    EXPECT_NEAR(result[0], 0.0, 1e-9);
}

TEST(ScoreCombiner, single_element_minmax_gives_half)
{
    // norm_mode 0: minmax clamps a zero-variance input to 0.5 for both PR and BC.
    auto result = ScoreCombiner::combine({3.0}, {7.0}, 0.6, 0.4, /*norm_mode=*/0);
    ASSERT_EQ(result.size(), 1u);
    EXPECT_NEAR(result[0], 0.5, 1e-9);
}

// 균일 입력 -> 0.5로 정규화
TEST(ScoreCombiner, uniform_pr_normalized_to_half)
{
    std::vector<double> pr = {2.0, 2.0, 2.0};
    std::vector<double> bc = {0.0, 0.5, 1.0};  // varied so result differs per node
    auto result = ScoreCombiner::combine(pr, bc, 1.0, 0.0);  // alpha=1, beta=0 → pure PR
    // w_pr=1, w_bc=0: score = PR_norm. All equal → all 0.5.
    ASSERT_EQ(result.size(), 3u);
    for (auto s : result)
        EXPECT_NEAR(s, 0.5, 1e-9);
}

TEST(ScoreCombiner, uniform_bc_normalized_to_half)
{
    std::vector<double> pr = {0.0, 0.5, 1.0};
    std::vector<double> bc = {5.0, 5.0, 5.0};
    auto result = ScoreCombiner::combine(pr, bc, 0.0, 1.0);  // pure BC
    ASSERT_EQ(result.size(), 3u);
    for (auto s : result)
        EXPECT_NEAR(s, 0.5, 1e-9);
}


// alpha=1, beta=0 -> pure PageRank -> score = [0, 1].
// alpha=0, beta=1 -> pure BetweenessConnectivity -> score = [1, 0].
TEST(ScoreCombiner, pure_pr_weight)
{
    auto result = ScoreCombiner::combine({0.0, 1.0}, {1.0, 0.0}, 1.0, 0.0);
    ASSERT_EQ(result.size(), 2u);
    EXPECT_NEAR(result[0], 0.0, 1e-9);
    EXPECT_NEAR(result[1], 1.0, 1e-9);
}

TEST(ScoreCombiner, pure_bc_weight)
{
    auto result = ScoreCombiner::combine({0.0, 1.0}, {1.0, 0.0}, 0.0, 1.0);
    ASSERT_EQ(result.size(), 2u);
    EXPECT_NEAR(result[0], 1.0, 1e-9);
    EXPECT_NEAR(result[1], 0.0, 1e-9);
}

// 계산해보기

// PR=[0,1,2], BC=[0,0,10], alpha=beta=1 -> w_pr=w_bc=0.5
// PR_norm = [0.0, 0.5, 1.0]
// BC_norm = [0.0, 0.0, 1.0]
// score   = [0.0, 0.25, 1.0]
TEST(ScoreCombiner, known_values_equal_weights)
{
    auto result = ScoreCombiner::combine({0.0, 1.0, 2.0}, {0.0, 0.0, 10.0}, 1.0, 1.0);
    ASSERT_EQ(result.size(), 3u);
    EXPECT_NEAR(result[0], 0.0,  1e-9);
    EXPECT_NEAR(result[1], 0.25, 1e-9);
    EXPECT_NEAR(result[2], 1.0,  1e-9);
}

// alpha=3, beta=1 -> w_pr=0.75, w_bc=0.25
// PR_norm=[0,1], BC_norm=[0,1]
// score = [0.0*0.75+0.0*0.25, 1.0*0.75+1.0*0.25] = [0.0, 1.0]
TEST(ScoreCombiner, known_values_asymmetric_weights)
{
    auto result = ScoreCombiner::combine({0.0, 1.0}, {0.0, 1.0}, 3.0, 1.0);
    ASSERT_EQ(result.size(), 2u);
    EXPECT_NEAR(result[0], 0.0, 1e-9);
    EXPECT_NEAR(result[1], 1.0, 1e-9);
}

// PR=[0,1], BC=[1,0], alpha=3, beta=1 -> w_pr=0.75, w_bc=0.25
// PR_norm=[0,1], BC_norm=[1,0]
// score[0] = 0.75*0 + 0.25*1 = 0.25
// score[1] = 0.75*1 + 0.25*0 = 0.75
TEST(ScoreCombiner, known_values_opposing_signals)
{
    auto result = ScoreCombiner::combine({0.0, 1.0}, {1.0, 0.0}, 3.0, 1.0);
    ASSERT_EQ(result.size(), 2u);
    EXPECT_NEAR(result[0], 0.25, 1e-9);
    EXPECT_NEAR(result[1], 0.75, 1e-9);
}


// BC가 동일하면 PR이 높을 수록 score 높음
TEST(ScoreCombiner, higher_pr_gives_higher_score)
{
    // PR strictly increasing, BC uniform
    auto result = ScoreCombiner::combine({1.0, 2.0, 3.0}, {5.0, 5.0, 5.0}, 0.6, 0.4);
    ASSERT_EQ(result.size(), 3u);
    EXPECT_LT(result[0], result[1]);
    EXPECT_LT(result[1], result[2]);
}

// ── norm_mode: min-max vs percentile rank ─────────────────────────────────────

// The failure min-max caused in practice: one extreme outlier squashes every
// other value against 0, so distinct PageRank scores become an indistinguishable
// block. Percentile rank spaces them evenly instead.
TEST(ScoreCombiner, percentile_survives_an_outlier_where_minmax_collapses)
{
    std::vector<double> pr = {1.0, 2.0, 3.0, 4.0, 1000.0};
    std::vector<double> bc(5, 0.0);

    auto mm  = ScoreCombiner::combine(pr, bc, 1.0, 0.0, /*norm_mode=*/0);
    auto pct = ScoreCombiner::combine(pr, bc, 1.0, 0.0, /*norm_mode=*/1);

    // min-max: the four small values all land within 0.003 of each other.
    EXPECT_LT(mm[3] - mm[0], 0.005);
    // percentile: evenly spaced quarters, 0.25 apart.
    EXPECT_NEAR(pct[0], 0.00, 1e-9);
    EXPECT_NEAR(pct[1], 0.25, 1e-9);
    EXPECT_NEAR(pct[2], 0.50, 1e-9);
    EXPECT_NEAR(pct[3], 0.75, 1e-9);
    EXPECT_NEAR(pct[4], 1.00, 1e-9);
}

TEST(ScoreCombiner, percentile_is_the_default_norm_mode)
{
    std::vector<double> pr = {1.0, 2.0, 3.0, 4.0, 1000.0};
    std::vector<double> bc(5, 0.0);
    EXPECT_EQ(ScoreCombiner::combine(pr, bc, 1.0, 0.0),
              ScoreCombiner::combine(pr, bc, 1.0, 0.0, /*norm_mode=*/1));
}

// ── gamma / ease term ─────────────────────────────────────────────────────────

// An empty complexity vector must reduce exactly to the two-term form,
// whatever gamma says -- this is what keeps Pass 2 (functions/classes) unchanged.
TEST(ScoreCombiner, empty_complexity_reduces_to_two_term)
{
    std::vector<double> pr = {0.0, 1.0, 2.0};
    std::vector<double> bc = {0.0, 0.0, 10.0};
    auto two   = ScoreCombiner::combine(pr, bc, 0.6, 0.4);
    auto three = ScoreCombiner::combine(pr, bc, {}, 0.6, 0.4, /*gamma=*/0.5);
    ASSERT_EQ(two.size(), three.size());
    for (std::size_t i = 0; i < two.size(); ++i)
        EXPECT_NEAR(two[i], three[i], 1e-12);
}

TEST(ScoreCombiner, gamma_zero_reduces_to_two_term)
{
    std::vector<double> pr = {0.0, 1.0, 2.0};
    std::vector<double> bc = {0.0, 0.0, 10.0};
    std::vector<double> cx = {0.1, 0.9, 0.5};
    auto two   = ScoreCombiner::combine(pr, bc, 0.6, 0.4);
    auto three = ScoreCombiner::combine(pr, bc, cx, 0.6, 0.4, /*gamma=*/0.0);
    ASSERT_EQ(two.size(), three.size());
    for (std::size_t i = 0; i < two.size(); ++i)
        EXPECT_NEAR(two[i], three[i], 1e-12);
}

// Equal PR and BC → the easier file must score strictly higher.
TEST(ScoreCombiner, easier_file_scores_higher_at_equal_importance)
{
    std::vector<double> pr = {1.0, 1.0};
    std::vector<double> bc = {1.0, 1.0};
    std::vector<double> cx = {0.9, 0.1};  // node 1 is much easier
    auto s = ScoreCombiner::combine(pr, bc, cx, 0.6, 0.4, 0.5);
    ASSERT_EQ(s.size(), 2u);
    EXPECT_GT(s[1], s[0]);
}

// alpha=beta=0, gamma=1 → score is purely ease = 1 - complexity.
TEST(ScoreCombiner, pure_ease_weight)
{
    std::vector<double> pr = {5.0, 5.0, 5.0};
    std::vector<double> bc = {5.0, 5.0, 5.0};
    std::vector<double> cx = {0.0, 0.25, 1.0};
    auto s = ScoreCombiner::combine(pr, bc, cx, 0.0, 0.0, 1.0);
    ASSERT_EQ(s.size(), 3u);
    EXPECT_NEAR(s[0], 1.00, 1e-9);
    EXPECT_NEAR(s[1], 0.75, 1e-9);
    EXPECT_NEAR(s[2], 0.00, 1e-9);
}

// complexity outside [0,1] is clamped rather than allowed to blow past the term.
TEST(ScoreCombiner, complexity_out_of_range_is_clamped)
{
    std::vector<double> pr = {5.0, 5.0};
    std::vector<double> bc = {5.0, 5.0};
    auto s = ScoreCombiner::combine(pr, bc, {-3.0, 7.0}, 0.0, 0.0, 1.0);
    ASSERT_EQ(s.size(), 2u);
    EXPECT_NEAR(s[0], 1.0, 1e-9);
    EXPECT_NEAR(s[1], 0.0, 1e-9);
}

// A complexity vector shorter than the node count falls back to the neutral
// value for the missing entries instead of reading out of bounds.
TEST(ScoreCombiner, short_complexity_uses_neutral)
{
    std::vector<double> pr = {5.0, 5.0};
    std::vector<double> bc = {5.0, 5.0};
    auto s = ScoreCombiner::combine(pr, bc, {0.0}, 0.0, 0.0, 1.0,
                                    /*norm_mode=*/1, /*complexity_neutral=*/0.25);
    ASSERT_EQ(s.size(), 2u);
    EXPECT_NEAR(s[0], 1.00, 1e-9);
    EXPECT_NEAR(s[1], 0.75, 1e-9);
}

// Known values, three terms: alpha=beta=gamma=1 → each weight 1/3.
// PR=[0,1,2]  -> pct [0, 0.5, 1]
// BC=[0,0,10] -> pct [0, 0,   1]
// cx=[1,0.5,0] -> ease [0, 0.5, 1]
// score = [0, (0.5+0+0.5)/3, 1] = [0, 1/3, 1]
TEST(ScoreCombiner, known_values_three_terms)
{
    auto s = ScoreCombiner::combine({0.0, 1.0, 2.0}, {0.0, 0.0, 10.0},
                                    {1.0, 0.5, 0.0}, 1.0, 1.0, 1.0);
    ASSERT_EQ(s.size(), 3u);
    EXPECT_NEAR(s[0], 0.0,       1e-9);
    EXPECT_NEAR(s[1], 1.0 / 3.0, 1e-9);
    EXPECT_NEAR(s[2], 1.0,       1e-9);
}
