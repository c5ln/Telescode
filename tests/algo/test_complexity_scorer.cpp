#include <gtest/gtest.h>

#include "algo/Scoring.h"
#include "db/db.h"
#include <sqlite3.h>

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

namespace {

double readComplexityScore(sqlite3* db, const std::string& fileId) {
    sqlite3_stmt* stmt = nullptr;
    std::string sql = "SELECT complexity_score FROM file WHERE file_id = ?;";
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, fileId.c_str(), -1, SQLITE_STATIC);
    double value = -1.0;
    if (sqlite3_step(stmt) == SQLITE_ROW) value = sqlite3_column_double(stmt, 0);
    sqlite3_finalize(stmt);
    return value;
}

} // namespace

TEST(ComplexityScorerComputeAndWrite, WritesScoresMatchingDirectComputeAndExcludesGenerated) {
    sqlite3* db = nullptr;
    ASSERT_EQ(initDb(":memory:", &db), SQLITE_OK);

    char* err = nullptr;
    int rc = sqlite3_exec(db,
        "INSERT INTO file(file_id, file_name, language, raw_loc, logical_loc, is_generated,"
        " max_cyclomatic_complexity, avg_cyclomatic_complexity, max_block_depth, avg_block_depth) VALUES"
        " ('a.py','a.py','python',100,100,0,10,5,4,2),"
        " ('b.py','b.py','python',200,200,0,20,15,8,6),"
        " ('gen.py','gen.py','python',9999,9999,1,999,999,999,999);",
        nullptr, nullptr, &err);
    ASSERT_EQ(rc, SQLITE_OK) << (err ? err : "");

    Graph g;
    NodeId a = g.get_or_add("a.py");
    NodeId b = g.get_or_add("b.py");
    g.get_or_add("gen.py");
    g.adj[a].push_back(b);   // a -> b: a.outbound=1, b.inbound=1
    g.radj[b].push_back(a);

    AlgoConfig cfg; // complexity_include_generated = false by default
    ASSERT_EQ(ComplexityScorer::computeAndWrite(db, g, cfg), SQLITE_OK);

    // gen.py wasn't in the scored population -- left at the column default.
    EXPECT_DOUBLE_EQ(readComplexityScore(db, "gen.py"), 0.0);

    // a.py/b.py scores should equal calling compute() directly on the same
    // (unblended) inputs -- i.e. the DB read + graph join didn't corrupt anything.
    std::vector<ComplexityFileMetrics> inputs(2);
    inputs[0].file_id = "a.py";
    inputs[0].max_cyclomatic_complexity = 10; inputs[0].avg_cyclomatic_complexity = 5;
    inputs[0].max_block_depth = 4;            inputs[0].avg_block_depth = 2;
    inputs[0].logical_loc = 100; inputs[0].inbound = 0; inputs[0].outbound = 1;
    inputs[1].file_id = "b.py";
    inputs[1].max_cyclomatic_complexity = 20; inputs[1].avg_cyclomatic_complexity = 15;
    inputs[1].max_block_depth = 8;            inputs[1].avg_block_depth = 6;
    inputs[1].logical_loc = 200; inputs[1].inbound = 1; inputs[1].outbound = 0;
    auto expected = ComplexityScorer::compute(inputs, cfg);

    EXPECT_NEAR(readComplexityScore(db, "a.py"), expected[0], 1e-9);
    EXPECT_NEAR(readComplexityScore(db, "b.py"), expected[1], 1e-9);

    sqlite3_close(db);
}

TEST(ComplexityScorerComputeAndWrite, IncludeGeneratedFlagScoresGeneratedFilesToo) {
    sqlite3* db = nullptr;
    ASSERT_EQ(initDb(":memory:", &db), SQLITE_OK);

    char* err = nullptr;
    int rc = sqlite3_exec(db,
        "INSERT INTO file(file_id, file_name, language, raw_loc, is_generated) VALUES"
        " ('a.py','a.py','python',100,0),"
        " ('gen.py','gen.py','python',50,1);",
        nullptr, nullptr, &err);
    ASSERT_EQ(rc, SQLITE_OK) << (err ? err : "");

    Graph g;
    g.get_or_add("a.py");
    g.get_or_add("gen.py");

    AlgoConfig cfg;
    cfg.complexity_include_generated = true;
    ASSERT_EQ(ComplexityScorer::computeAndWrite(db, g, cfg), SQLITE_OK);

    // Every unset metric column defaults to 0 and both files are isolated
    // graph nodes, so a.py and gen.py have identical (all-zero) inputs --
    // the all-tied case percentile-ranks both to 0.5, and default weights
    // sum to 1.0, so a real (non-default) score of exactly 0.5 proves
    // gen.py was actually included in the scored population, not skipped.
    EXPECT_DOUBLE_EQ(readComplexityScore(db, "gen.py"), 0.5);
    EXPECT_DOUBLE_EQ(readComplexityScore(db, "a.py"), 0.5);

    sqlite3_close(db);
}

TEST(ComplexityScorerComputeAndWrite, EmptyFileTableSucceedsWithoutError) {
    sqlite3* db = nullptr;
    ASSERT_EQ(initDb(":memory:", &db), SQLITE_OK);

    Graph g;
    AlgoConfig cfg;
    EXPECT_EQ(ComplexityScorer::computeAndWrite(db, g, cfg), SQLITE_OK);

    sqlite3_close(db);
}
