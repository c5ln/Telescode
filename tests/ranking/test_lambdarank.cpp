// Contract tests for ranking/LambdaRankModel.h.
// Python-generated golden data is shared with the CLI parity tests.
#include "ranking/LambdaRankModel.h"
#include "algo/SeedIo.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using Model = TS::LambdaRankModel;
using Table = TS::LtrFeatureTable;

namespace {

std::string readText(const fs::path& path)
{
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot read test fixture: " + path.string());
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

Table readTable(const fs::path& path)
{
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot read test fixture: " + path.string());
    std::string line;
    std::getline(in, line);
    const auto header = splitCsvLine(line);
    if (header.size() < 3) throw std::runtime_error("invalid test fixture header");
    Table table;
    table.columns.assign(header.begin() + 2, header.end());
    while (std::getline(in, line)) {
        const auto cells = splitCsvLine(line);
        if (cells.size() != header.size()) throw std::runtime_error("invalid test fixture row");
        TS::LtrFeatureRow row;
        row.query_id = cells[0];
        row.file_id = cells[1];
        for (std::size_t i = 2; i < cells.size(); ++i)
            row.values.push_back(cells[i].empty() ? std::numeric_limits<double>::quiet_NaN()
                                                 : std::stod(cells[i]));
        table.rows.push_back(std::move(row));
    }
    return table;
}

std::vector<TS::LtrRankedFile> readRanking(const char* name = "ranked.csv")
{
    std::ifstream in(fs::path(TS_LTR_FIXTURE) / name);
    if (!in) throw std::runtime_error("cannot read ranked fixture");
    std::string line;
    std::getline(in, line);
    std::vector<TS::LtrRankedFile> rows;
    while (std::getline(in, line)) {
        const auto cells = splitCsvLine(line);
        if (cells.size() != 4) throw std::runtime_error("invalid ranked fixture row");
        rows.push_back({cells[0], cells[1], std::stod(cells[2]), std::stoi(cells[3])});
    }
    return rows;
}

void expectTable(const Table& actual, const Table& expected)
{
    ASSERT_FALSE(expected.columns.empty());
    ASSERT_FALSE(expected.rows.empty());
    ASSERT_EQ(actual.columns, expected.columns);
    ASSERT_EQ(actual.rows.size(), expected.rows.size());
    for (std::size_t i = 0; i < expected.rows.size(); ++i) {
        SCOPED_TRACE(i);
        EXPECT_EQ(actual.rows[i].query_id, expected.rows[i].query_id);
        EXPECT_EQ(actual.rows[i].file_id, expected.rows[i].file_id);
        ASSERT_EQ(actual.rows[i].values.size(), expected.columns.size());
        ASSERT_EQ(expected.rows[i].values.size(), expected.columns.size());
        for (std::size_t j = 0; j < expected.columns.size(); ++j) {
            SCOPED_TRACE(expected.columns[j]);
            if (std::isnan(expected.rows[i].values[j])) EXPECT_TRUE(std::isnan(actual.rows[i].values[j]));
            else EXPECT_DOUBLE_EQ(actual.rows[i].values[j], expected.rows[i].values[j]);
        }
    }
}

void expectRanking(const std::vector<TS::LtrRankedFile>& actual,
                   const std::vector<TS::LtrRankedFile>& expected)
{
    ASSERT_EQ(actual.size(), expected.size());
    for (std::size_t i = 0; i < actual.size(); ++i) {
        SCOPED_TRACE(i);
        EXPECT_EQ(actual[i].query_id, expected[i].query_id);
        EXPECT_EQ(actual[i].file_id, expected[i].file_id);
        EXPECT_EQ(actual[i].rank, expected[i].rank);
        EXPECT_NEAR(actual[i].score, expected[i].score,
                    1e-12 * std::max(1.0, std::abs(expected[i].score)));
    }
}

struct ArtifactCopy {
    fs::path path;
    ArtifactCopy()
    {
        static std::atomic<unsigned> count{0};
        path = fs::temp_directory_path() /
            ("ts_ltr_artifact_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
             + "_" + std::to_string(count++));
        fs::create_directories(path);
        for (const char* file : {"model.txt", "artifact.json"})
            fs::copy_file(fs::path(TS_LTR_ARTIFACT) / file, path / file);
    }
    ~ArtifactCopy() { std::error_code ec; fs::remove_all(path, ec); }
    std::string metadata() const { return readText(path / "artifact.json"); }
    void write(const std::string& text) const { std::ofstream(path / "artifact.json") << text; }
};

std::string replace(std::string text, const std::string& from, const std::string& to)
{
    const auto at = text.find(from);
    if (at == std::string::npos) throw std::runtime_error("test metadata token not found: " + from);
    return text.replace(at, from.size(), to);
}

} // namespace

TEST(LambdaRank, LoadsReleasedFeatureOrder)
{
    const auto model = Model::Load(TS_LTR_ARTIFACT);
    EXPECT_EQ(model.featureColumns(), readTable(fs::path(TS_LTR_FIXTURE) / "prepared.csv").columns);
    EXPECT_EQ(model.featureColumns().size(), 19u);
}

TEST(LambdaRank, PreparedMatrixMatchesPythonIncludingNaNAndCandidateSelection)
{
    const auto model = Model::Load(TS_LTR_ARTIFACT);
    const auto actual = model.prepare(readTable(fs::path(TS_LTR_FIXTURE) / "input.csv"));
    const auto expected = readTable(fs::path(TS_LTR_FIXTURE) / "prepared.csv");
    expectTable(actual, expected);
}

TEST(LambdaRank, ScoresAndRanksMatchReleasedPythonModel)
{
    const auto model = Model::Load(TS_LTR_ARTIFACT);
    expectRanking(model.rank(readTable(fs::path(TS_LTR_FIXTURE) / "input.csv")), readRanking());
}

TEST(LambdaRank, SplitBoundaryScoresMatchPythonWithoutFloat32Conversion)
{
    const auto model = Model::Load(TS_LTR_ARTIFACT);
    const auto input = readTable(fs::path(TS_LTR_FIXTURE) / "boundary_input.csv");
    expectTable(model.prepare(input), readTable(fs::path(TS_LTR_FIXTURE) / "boundary_prepared.csv"));
    expectRanking(model.rank(input), readRanking("boundary_ranked.csv"));
}

TEST(LambdaRank, ConstantsBecomeZeroAndEntirelyMissingColumnsStayNaN)
{
    const auto model = Model::Load(TS_LTR_ARTIFACT);
    Table input;
    input.columns = model.featureColumns();
    std::vector<double> values(input.columns.size(), 5.0);
    values[0] = std::numeric_limits<double>::quiet_NaN();
    input.rows = {{"q", "z.py", values}, {"q", "a.py", values}};
    const auto prepared = model.prepare(input);
    ASSERT_EQ(prepared.columns, input.columns);
    ASSERT_EQ(prepared.rows.size(), 2u);
    for (const auto& row : prepared.rows) {
        ASSERT_EQ(row.values.size(), input.columns.size());
        EXPECT_TRUE(std::isnan(row.values[0]));
        for (std::size_t i = 1; i < row.values.size(); ++i) EXPECT_DOUBLE_EQ(row.values[i], 0.0);
    }
    const auto ranked = model.rank(input);
    ASSERT_EQ(ranked.size(), 2u);
    EXPECT_EQ(ranked[0].file_id, "a.py");
    EXPECT_EQ(ranked[1].file_id, "z.py");
    EXPECT_DOUBLE_EQ(ranked[0].score, ranked[1].score);
    EXPECT_EQ(ranked[0].rank, 1);
    EXPECT_EQ(ranked[1].rank, 2);
}

TEST(LambdaRank, NoneNormalizationPreservesRawValuesInMetadataOrder)
{
    ArtifactCopy artifact;
    artifact.write(replace(artifact.metadata(), "\"per_query_minmax\"", "\"none\""));
    const auto model = Model::Load(artifact.path.string());
    const auto input = readTable(fs::path(TS_LTR_FIXTURE) / "input.csv");
    expectTable(model.prepare(input), readTable(fs::path(TS_LTR_FIXTURE) / "none_prepared.csv"));
    expectRanking(model.rank(input), readRanking("none_ranked.csv"));
}

TEST(LambdaRank, RepeatedCallsDoNotMutateInputOrRetainQueryNormalization)
{
    const auto model = Model::Load(TS_LTR_ARTIFACT);
    const auto input = readTable(fs::path(TS_LTR_FIXTURE) / "input.csv");
    expectRanking(model.rank(input), readRanking());
    auto other = input;
    other.rows = {{"different", "x.py", std::vector<double>(input.columns.size(), 400.)}};
    ASSERT_EQ(model.rank(other).size(), 1u);
    expectRanking(model.rank(input), readRanking());
}

class InvalidRankingInput : public testing::TestWithParam<std::string> {};
TEST_P(InvalidRankingInput, RejectsBeforeInference)
{
    const auto model = Model::Load(TS_LTR_ARTIFACT);
    auto input = readTable(fs::path(TS_LTR_FIXTURE) / "input.csv");
    input.rows.resize(1);
    const auto kind = GetParam();
    if (kind == "missing_feature") {
        input.columns.erase(input.columns.begin() + 1); // first column is an unused extra
        input.rows[0].values.erase(input.rows[0].values.begin() + 1);
    } else if (kind == "duplicate_column") input.columns[1] = input.columns[2];
    else if (kind == "row_width") input.rows[0].values.pop_back();
    else if (kind == "duplicate_candidate") input.rows.push_back(input.rows.front());
    else if (kind == "empty_query") input.rows[0].query_id = "";
    else if (kind == "empty_file") input.rows[0].file_id = "";
    else if (kind == "infinity") input.rows[0].values[1] = std::numeric_limits<double>::infinity();
    else if (kind == "no_candidates") input.rows[0].file_id = "pkg/__init__.py";
    else if (kind == "empty_input") input.rows.clear();
    EXPECT_THROW(model.prepare(input), std::invalid_argument);
    EXPECT_THROW(model.rank(input), std::invalid_argument);
}
INSTANTIATE_TEST_SUITE_P(Contract, InvalidRankingInput, testing::Values(
    "missing_feature", "duplicate_column", "row_width", "duplicate_candidate",
    "empty_query", "empty_file", "infinity", "no_candidates", "empty_input"));

class InvalidRankingMetadata : public testing::TestWithParam<std::string> {};
TEST_P(InvalidRankingMetadata, RejectsUnsupportedContract)
{
    ArtifactCopy artifact;
    auto text = artifact.metadata();
    const auto kind = GetParam();
    if (kind == "version") text = replace(text, "\"artifact_version\": 1", "\"artifact_version\": 999");
    else if (kind == "type") text = replace(text, "lightgbm_lambdarank", "other_model");
    else if (kind == "normalization") text = replace(text, "per_query_minmax", "unknown");
    else if (kind == "policy") text = replace(text, "all_no_init", "top20");
    else if (kind == "model_file") text = replace(text, "\"model.txt\"", "\"../model.txt\"");
    else if (kind == "empty_feature") text = replace(text, "\"pagerank\"", "\"\"");
    else if (kind == "duplicate_feature") text = replace(text, "\"pagerank\"", "\"bc\"");
    else if (kind == "empty_group") text = replace(text, "\"instance_id\"", "\"\"");
    artifact.write(text);
    EXPECT_THROW(Model::Load(artifact.path.string()), std::runtime_error);
}
INSTANTIATE_TEST_SUITE_P(Contract, InvalidRankingMetadata, testing::Values(
    "version", "type", "normalization", "policy", "model_file", "empty_feature",
    "duplicate_feature", "empty_group"));

TEST(LambdaRank, RejectsSameFeatureCountWithDifferentNamesOrOrder)
{
    ArtifactCopy artifact;
    auto text = replace(artifact.metadata(), "\"pagerank\"", "\"temporary\"");
    text = replace(text, "\"bc\"", "\"pagerank\"");
    text = replace(text, "\"temporary\"", "\"bc\"");
    artifact.write(text);
    EXPECT_THROW(Model::Load(artifact.path.string()), std::runtime_error);
}

TEST(LambdaRank, RejectsDifferentFeatureCount)
{
    ArtifactCopy artifact;
    artifact.write(replace(artifact.metadata(),
        "\"nbr_bm25_max\",\n    \"nbr_bm25_mean\"", "\"nbr_bm25_max\""));
    EXPECT_THROW(Model::Load(artifact.path.string()), std::runtime_error);
}

TEST(LambdaRank, RejectsMissingModelAndMalformedMetadata)
{
    ArtifactCopy artifact;
    fs::remove(artifact.path / "model.txt");
    EXPECT_THROW(Model::Load(artifact.path.string()), std::runtime_error);
    fs::copy_file(fs::path(TS_LTR_ARTIFACT) / "model.txt", artifact.path / "model.txt");
    artifact.write("{invalid JSON");
    EXPECT_THROW(Model::Load(artifact.path.string()), std::runtime_error);
}
