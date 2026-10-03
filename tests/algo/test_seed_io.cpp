#include "algo/SeedIo.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <string>

namespace {

std::string tmp(const char* name) { return ::testing::TempDir() + name; }

void write(const std::string& path, const std::string& body)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << body;
}

}  // namespace

// ── splitCsvLine ──────────────────────────────────────────────────────────────

TEST(SplitCsvLine, PlainFields)
{
    auto f = splitCsvLine("a.py,1.5");
    ASSERT_EQ(f.size(), 2u);
    EXPECT_EQ(f[0], "a.py");
    EXPECT_EQ(f[1], "1.5");
}

TEST(SplitCsvLine, EmptyFieldsArePreserved)
{
    auto f = splitCsvLine("a.py,,3");
    ASSERT_EQ(f.size(), 3u);
    EXPECT_EQ(f[1], "");
}

// A path containing a comma must survive; splitting it silently would surface
// only as a mysterious join failure downstream.
TEST(SplitCsvLine, QuotedFieldWithComma)
{
    auto f = splitCsvLine("\"pkg/a,b.py\",2.0");
    ASSERT_EQ(f.size(), 2u);
    EXPECT_EQ(f[0], "pkg/a,b.py");
    EXPECT_EQ(f[1], "2.0");
}

TEST(SplitCsvLine, EscapedQuoteInsideQuotedField)
{
    auto f = splitCsvLine("\"say \"\"hi\"\".py\",1");
    ASSERT_EQ(f.size(), 2u);
    EXPECT_EQ(f[0], "say \"hi\".py");
}

TEST(SplitCsvLine, TrailingCarriageReturnIsStripped)
{
    auto f = splitCsvLine("a.py,1.0\r");
    ASSERT_EQ(f.size(), 2u);
    EXPECT_EQ(f[1], "1.0");
}

// ── loadSeedCsv ───────────────────────────────────────────────────────────────

TEST(LoadSeedCsv, ReadsFileIdAndWeight)
{
    const std::string p = tmp("seed_ok.csv");
    write(p, "file_id,weight\nsrc/a.py,3.5\nsrc/b.py,0.25\n");

    auto r = loadSeedCsv(p);
    ASSERT_EQ(r.error, "");
    ASSERT_EQ(r.seeds.size(), 2u);
    EXPECT_EQ(r.seeds[0].file_id, "src/a.py");
    EXPECT_DOUBLE_EQ(r.seeds[0].weight, 3.5);
    EXPECT_EQ(r.seeds[1].file_id, "src/b.py");
    EXPECT_DOUBLE_EQ(r.seeds[1].weight, 0.25);
    std::remove(p.c_str());
}

// Column order comes from the header, not from position.
TEST(LoadSeedCsv, ColumnOrderIndependent)
{
    const std::string p = tmp("seed_order.csv");
    write(p, "weight,file_id\n7,a.py\n");
    auto r = loadSeedCsv(p);
    ASSERT_EQ(r.error, "");
    ASSERT_EQ(r.seeds.size(), 1u);
    EXPECT_EQ(r.seeds[0].file_id, "a.py");
    EXPECT_DOUBLE_EQ(r.seeds[0].weight, 7.0);
    std::remove(p.c_str());
}

TEST(LoadSeedCsv, ExtraColumnsAreIgnored)
{
    const std::string p = tmp("seed_extra.csv");
    write(p, "instance_id,file_id,weight,note\nx-1,a.py,2,hello\n");
    auto r = loadSeedCsv(p);
    ASSERT_EQ(r.error, "");
    ASSERT_EQ(r.seeds.size(), 1u);
    EXPECT_EQ(r.seeds[0].file_id, "a.py");
    EXPECT_DOUBLE_EQ(r.seeds[0].weight, 2.0);
    std::remove(p.c_str());
}

TEST(LoadSeedCsv, ScientificNotationAndNegativeWeights)
{
    const std::string p = tmp("seed_sci.csv");
    write(p, "file_id,weight\na.py,1.5e-3\nb.py,-2\n");
    auto r = loadSeedCsv(p);
    ASSERT_EQ(r.error, "");
    ASSERT_EQ(r.seeds.size(), 2u);
    EXPECT_DOUBLE_EQ(r.seeds[0].weight, 1.5e-3);
    EXPECT_DOUBLE_EQ(r.seeds[1].weight, -2.0);   // makeTeleport drops it later
    std::remove(p.c_str());
}

TEST(LoadSeedCsv, BlankLinesAreSkipped)
{
    const std::string p = tmp("seed_blank.csv");
    write(p, "file_id,weight\na.py,1\n\n   \nb.py,2\n");
    auto r = loadSeedCsv(p);
    ASSERT_EQ(r.error, "");
    EXPECT_EQ(r.seeds.size(), 2u);
    std::remove(p.c_str());
}

TEST(LoadSeedCsv, HeaderOnlyGivesNoSeedsAndNoError)
{
    const std::string p = tmp("seed_hdr.csv");
    write(p, "file_id,weight\n");
    auto r = loadSeedCsv(p);
    EXPECT_EQ(r.error, "");
    EXPECT_TRUE(r.seeds.empty());   // the CLI turns this into a failure
    std::remove(p.c_str());
}

// ── loadSeedCsv: failures must be loud, never silently skipped (CONTRACT §7) ──

TEST(LoadSeedCsv, MissingFileErrors)
{
    auto r = loadSeedCsv(tmp("does_not_exist_seed.csv"));
    EXPECT_NE(r.error, "");
    EXPECT_TRUE(r.seeds.empty());
}

TEST(LoadSeedCsv, EmptyFileErrors)
{
    const std::string p = tmp("seed_empty.csv");
    write(p, "");
    EXPECT_NE(loadSeedCsv(p).error, "");
    std::remove(p.c_str());
}

TEST(LoadSeedCsv, MissingRequiredColumnErrors)
{
    const std::string p = tmp("seed_badhdr.csv");
    write(p, "file_id,bm25\na.py,1\n");
    auto r = loadSeedCsv(p);
    EXPECT_NE(r.error, "");
    EXPECT_NE(r.error.find("weight"), std::string::npos);
    std::remove(p.c_str());
}

TEST(LoadSeedCsv, NonNumericWeightErrorsWithLineNumber)
{
    const std::string p = tmp("seed_nan.csv");
    write(p, "file_id,weight\na.py,1\nb.py,abc\n");
    auto r = loadSeedCsv(p);
    ASSERT_NE(r.error, "");
    EXPECT_NE(r.error.find("line 3"), std::string::npos) << r.error;
    std::remove(p.c_str());
}

TEST(LoadSeedCsv, TooFewColumnsErrors)
{
    const std::string p = tmp("seed_short.csv");
    write(p, "file_id,weight\na.py\n");
    auto r = loadSeedCsv(p);
    EXPECT_NE(r.error, "");
    std::remove(p.c_str());
}

TEST(LoadSeedCsv, EmptyWeightErrors)
{
    const std::string p = tmp("seed_emptyw.csv");
    write(p, "file_id,weight\na.py,\n");
    EXPECT_NE(loadSeedCsv(p).error, "");
    std::remove(p.c_str());
}

TEST(LoadSeedCsv, EmptyFileIdErrors)
{
    const std::string p = tmp("seed_emptyf.csv");
    write(p, "file_id,weight\n,1.0\n");
    EXPECT_NE(loadSeedCsv(p).error, "");
    std::remove(p.c_str());
}

// ── writePprCsv ───────────────────────────────────────────────────────────────

TEST(WritePprCsv, WritesHeaderAndRows)
{
    const std::string p = tmp("ppr_out.csv");
    ASSERT_EQ(writePprCsv(p, {{"a.py", 0.5}, {"b.py", 0.25}}), "");

    std::ifstream in(p);
    std::string l1, l2, l3;
    std::getline(in, l1); std::getline(in, l2); std::getline(in, l3);
    EXPECT_EQ(l1, "file_id,ppr");
    EXPECT_EQ(l2, "a.py,0.5");
    EXPECT_EQ(l3, "b.py,0.25");
    std::remove(p.c_str());
}

// Round-trip at full precision: the harness must read back exactly what we ranked.
TEST(WritePprCsv, PreservesFullPrecision)
{
    const std::string p = tmp("ppr_prec.csv");
    const double v = 0.12345678901234567;
    ASSERT_EQ(writePprCsv(p, {{"a.py", v}}), "");

    std::ifstream in(p);
    std::string line;
    std::getline(in, line);            // header
    std::getline(in, line);
    auto f = splitCsvLine(line);
    ASSERT_EQ(f.size(), 2u);
    EXPECT_DOUBLE_EQ(std::stod(f[1]), v);
    std::remove(p.c_str());
}

TEST(WritePprCsv, QuotesFileIdContainingComma)
{
    const std::string p = tmp("ppr_comma.csv");
    ASSERT_EQ(writePprCsv(p, {{"pkg/a,b.py", 1.0}}), "");

    std::ifstream in(p);
    std::string line;
    std::getline(in, line);
    std::getline(in, line);
    auto f = splitCsvLine(line);
    ASSERT_EQ(f.size(), 2u);
    EXPECT_EQ(f[0], "pkg/a,b.py");
    std::remove(p.c_str());
}

TEST(WritePprCsv, EmptyRowsStillWritesHeader)
{
    const std::string p = tmp("ppr_none.csv");
    ASSERT_EQ(writePprCsv(p, {}), "");
    std::ifstream in(p);
    std::string line;
    ASSERT_TRUE(static_cast<bool>(std::getline(in, line)));
    EXPECT_EQ(line, "file_id,ppr");
    std::remove(p.c_str());
}

TEST(WritePprCsv, UnwritablePathErrors)
{
    EXPECT_NE(writePprCsv("/nonexistent_dir_xyz/out.csv", {{"a.py", 1.0}}), "");
}
