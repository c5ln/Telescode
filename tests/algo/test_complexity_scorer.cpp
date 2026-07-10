#include <gtest/gtest.h>

#include "algo/Scoring.h"

namespace {

std::vector<ComplexityFileMetrics> threeFileFixture() {
    // A: mid on everything; B: high; C: low -- chosen so default weights
    // (0.30/0.20/0.15/0.20/0.15, sum exactly 1.0) and default blends
    // (0.8 max / 0.2 avg) produce clean hand-computable percentile ranks.
    ComplexityFileMetrics a;
    a.file_id = "a.py";
    a.max_cyclomatic_complexity = 10; a.avg_cyclomatic_complexity = 5;
    a.max_block_depth = 4;            a.avg_block_depth = 2;
    a.logical_loc = 100; a.inbound = 1; a.outbound = 0;

    ComplexityFileMetrics b;
    b.file_id = "b.py";
    b.max_cyclomatic_complexity = 20; b.avg_cyclomatic_complexity = 15;
    b.max_block_depth = 8;            b.avg_block_depth = 6;
    b.logical_loc = 200; b.inbound = 3; b.outbound = 2;

    ComplexityFileMetrics c;
    c.file_id = "c.py";
    c.max_cyclomatic_complexity = 5; c.avg_cyclomatic_complexity = 3;
    c.max_block_depth = 2;           c.avg_block_depth = 1;
    c.logical_loc = 50; c.inbound = 0; c.outbound = 5;

    return {a, b, c};
}

} // namespace

TEST(ComplexityScorer, EmptyInputReturnsEmpty) {
    AlgoConfig cfg;
    EXPECT_TRUE(ComplexityScorer::compute({}, cfg).empty());
}

TEST(ComplexityScorer, WeightedPercentileRankCombinationMatchesHandComputed) {
    AlgoConfig cfg; // defaults: w = 0.30/0.20/0.15/0.20/0.15, blends = 0.8/0.2
    auto scores = ComplexityScorer::compute(threeFileFixture(), cfg);
    ASSERT_EQ(scores.size(), 3u);

    // cc_raw:      a=0.8*10+0.2*5=9.0   b=0.8*20+0.2*15=19.0  c=0.8*5+0.2*3=4.6
    // nesting_raw: a=0.8*4+0.2*2=3.6    b=0.8*8+0.2*6=7.6     c=0.8*2+0.2*1=1.8
    // loc:         a=100 b=200 c=50 | inbound: a=1 b=3 c=0 | outbound: a=0 b=2 c=5
    // Sorted order for cc/nesting/loc/inbound is always c < a < b -> pct: c=0, a=0.5, b=1.0
    // Sorted order for outbound is a < b < c -> pct: a=0, b=0.5, c=1.0
    // score_a = 0.30*.5 + 0.20*.5 + 0.15*.5 + 0.20*.5 + 0.15*0   = 0.425
    // score_b = 0.30*1  + 0.20*1  + 0.15*1  + 0.20*1  + 0.15*.5  = 0.925
    // score_c = 0.30*0  + 0.20*0  + 0.15*0  + 0.20*0  + 0.15*1   = 0.15
    EXPECT_NEAR(scores[0], 0.425, 1e-9);
    EXPECT_NEAR(scores[1], 0.925, 1e-9);
    EXPECT_NEAR(scores[2], 0.15,  1e-9);
}

TEST(ComplexityScorer, TopLevelWeightsAutoNormalizeByRatio) {
    // Uniform weights of 1.0 each (sum=5.0) should normalize to the same
    // effective 0.2-each weighting as explicitly-normalized uniform weights.
    AlgoConfig unnormalized;
    unnormalized.complexity_w_cc = unnormalized.complexity_w_nesting =
        unnormalized.complexity_w_loc = unnormalized.complexity_w_inbound =
        unnormalized.complexity_w_outbound = 1.0;

    AlgoConfig normalized;
    normalized.complexity_w_cc = normalized.complexity_w_nesting =
        normalized.complexity_w_loc = normalized.complexity_w_inbound =
        normalized.complexity_w_outbound = 0.2;

    auto inputs = threeFileFixture();
    auto s1 = ComplexityScorer::compute(inputs, unnormalized);
    auto s2 = ComplexityScorer::compute(inputs, normalized);

    ASSERT_EQ(s1.size(), s2.size());
    for (std::size_t i = 0; i < s1.size(); ++i)
        EXPECT_NEAR(s1[i], s2[i], 1e-9);
}

TEST(ComplexityScorer, BlendWeightsAutoNormalizeByRatio) {
    // cc blend of 2.0/2.0 (sum=4.0, ratio 1:1) should normalize the same as
    // an explicit 0.5/0.5 blend.
    AlgoConfig unnormalized;
    unnormalized.complexity_cc_max_blend = 2.0;
    unnormalized.complexity_cc_avg_blend = 2.0;

    AlgoConfig normalized;
    normalized.complexity_cc_max_blend = 0.5;
    normalized.complexity_cc_avg_blend = 0.5;

    auto inputs = threeFileFixture();
    auto s1 = ComplexityScorer::compute(inputs, unnormalized);
    auto s2 = ComplexityScorer::compute(inputs, normalized);

    ASSERT_EQ(s1.size(), s2.size());
    for (std::size_t i = 0; i < s1.size(); ++i)
        EXPECT_NEAR(s1[i], s2[i], 1e-9);
}

TEST(ComplexityScorer, LogicalLocInboundOutboundFeedDirectlyWithoutBlending) {
    // Isolate loc/inbound/outbound by zeroing every other weight -- the
    // resulting score should equal a plain weighted percentile-rank of the
    // raw (unblended) values.
    AlgoConfig cfg;
    cfg.complexity_w_cc = 0.0;
    cfg.complexity_w_nesting = 0.0;
    cfg.complexity_w_loc = 1.0;
    cfg.complexity_w_inbound = 0.0;
    cfg.complexity_w_outbound = 0.0;

    auto inputs = threeFileFixture();
    auto scores = ComplexityScorer::compute(inputs, cfg);

    std::vector<double> loc_raw = {100.0, 200.0, 50.0};
    auto expected = percentile_rank_normalize(loc_raw);

    ASSERT_EQ(scores.size(), expected.size());
    for (std::size_t i = 0; i < scores.size(); ++i)
        EXPECT_NEAR(scores[i], expected[i], 1e-9);
}
