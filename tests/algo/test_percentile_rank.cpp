#include <gtest/gtest.h>

#include "algo/Scoring.h"

TEST(PercentileRankNormalize, EmptyInputReturnsEmpty) {
    EXPECT_TRUE(percentile_rank_normalize({}).empty());
}

TEST(PercentileRankNormalize, SingleFileReturnsZero) {
    auto out = percentile_rank_normalize({42.0});
    ASSERT_EQ(out.size(), 1u);
    EXPECT_DOUBLE_EQ(out[0], 0.0);
}

TEST(PercentileRankNormalize, AllSameValueReturnsHalf) {
    auto out = percentile_rank_normalize({5.0, 5.0, 5.0, 5.0});
    ASSERT_EQ(out.size(), 4u);
    for (double v : out) EXPECT_DOUBLE_EQ(v, 0.5);
}

TEST(PercentileRankNormalize, ValuesInRangeZeroToOne) {
    auto out = percentile_rank_normalize({10.0, 2.0, 7.0, 100.0, 3.0});
    for (double v : out) {
        EXPECT_GE(v, 0.0);
        EXPECT_LE(v, 1.0);
    }
}

TEST(PercentileRankNormalize, MinAndMaxHitZeroAndOne) {
    auto out = percentile_rank_normalize({10.0, 2.0, 7.0, 100.0, 3.0});
    // index 1 (value 2.0) is the minimum -> 0.0; index 3 (value 100.0) is the max -> 1.0
    EXPECT_DOUBLE_EQ(out[1], 0.0);
    EXPECT_DOUBLE_EQ(out[3], 1.0);
}

TEST(PercentileRankNormalize, TiesShareTheSameRank) {
    // sorted: 1, 5, 5, 5, 9 (N=5, N-1=4)
    // strictly-lower counts: 1->0, 5->1 (all three), 9->4
    auto out = percentile_rank_normalize({5.0, 1.0, 5.0, 9.0, 5.0});
    EXPECT_DOUBLE_EQ(out[0], 0.25);
    EXPECT_DOUBLE_EQ(out[2], 0.25);
    EXPECT_DOUBLE_EQ(out[4], 0.25);
    EXPECT_DOUBLE_EQ(out[1], 0.0);
    EXPECT_DOUBLE_EQ(out[3], 1.0);
}

TEST(PercentileRankNormalize, OneOutlierDoesNotCompressOthers) {
    // Unlike min-max, a single huge outlier shouldn't push every other
    // value's rank toward 0 -- ranks should stay spread out by position.
    auto out = percentile_rank_normalize({10.0, 20.0, 30.0, 40.0, 5000.0});
    // sorted: 10,20,30,40,5000 -> N-1=4
    EXPECT_DOUBLE_EQ(out[0], 0.0);   // 10
    EXPECT_DOUBLE_EQ(out[1], 0.25);  // 20
    EXPECT_DOUBLE_EQ(out[2], 0.5);   // 30
    EXPECT_DOUBLE_EQ(out[3], 0.75);  // 40
    EXPECT_DOUBLE_EQ(out[4], 1.0);   // 5000
}
