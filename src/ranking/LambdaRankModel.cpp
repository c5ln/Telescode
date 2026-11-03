#include "LambdaRankModel.h"
#include "LightGbmApi.h"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <utility>

namespace TS {
namespace {

bool blank(const std::string& text)
{
    return std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c); });
}

bool initializer(const std::string& file)
{
    const auto at = file.find_last_of("/\\");
    return file.substr(at == std::string::npos ? 0 : at + 1) == "__init__.py";
}

std::string readFile(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read model metadata: " + path.string());
    std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    if (in.bad()) throw std::runtime_error("error reading model metadata: " + path.string());
    return text;
}

void checkLightGbm(int rc)
{
    if (rc != 0) throw std::runtime_error(std::string("LightGBM: ") + LGBM_GetLastError());
}

struct CloseDb { void operator()(sqlite3* db) const { sqlite3_close(db); } };
struct Finalize { void operator()(sqlite3_stmt* stmt) const { sqlite3_finalize(stmt); } };
using Statement = std::unique_ptr<sqlite3_stmt, Finalize>;

// Metadata is tiny. Reuse the bundled SQLite JSON parser instead of adding a
// second JSON dependency or maintaining a handwritten parser. No disk DB opens.
class Metadata {
public:
    explicit Metadata(std::string json) : json_(std::move(json))
    {
        sqlite3* raw = nullptr;
        const int rc = sqlite3_open(":memory:", &raw);
        db_.reset(raw);
        if (rc != SQLITE_OK) fail("cannot open JSON reader");
        auto valid = query("SELECT json_valid(?1)");
        if (sqlite3_column_int(valid.get(), 0) != 1) fail("malformed JSON");
        auto object = query("SELECT json_type(?1)");
        if (text(object.get(), 0) != "object") fail("expected an object");
        auto version = query("SELECT json_type(?1, '$.artifact_version'),"
                             " json_extract(?1, '$.artifact_version')");
        if (sqlite3_column_type(version.get(), 0) != SQLITE_NULL &&
            (text(version.get(), 0) != "integer" || sqlite3_column_int64(version.get(), 1) != 1))
            fail("unsupported artifact version");
    }

    std::string field(const char* path, const std::string& fallback)
    {
        auto stmt = query("SELECT json_type(?1, ?2), json_extract(?1, ?2)", path);
        if (sqlite3_column_type(stmt.get(), 0) == SQLITE_NULL) return fallback;
        if (text(stmt.get(), 0) != "text") fail(std::string("expected string at ") + path);
        return text(stmt.get(), 1);
    }

    std::vector<std::string> features()
    {
        auto array = query("SELECT json_type(?1, '$.feature_columns')");
        if (text(array.get(), 0) != "array") fail("feature_columns must be an array");
        auto stmt = prepare("SELECT type, value FROM json_each(?1, '$.feature_columns')"
                            " ORDER BY CAST(key AS INTEGER)");
        std::vector<std::string> out;
        std::set<std::string> names;
        int rc;
        while ((rc = sqlite3_step(stmt.get())) == SQLITE_ROW) {
            const auto name = text(stmt.get(), 1);
            if (text(stmt.get(), 0) != "text" || blank(name) ||
                std::any_of(name.begin(), name.end(), [](unsigned char c) { return std::isspace(c); }))
                fail("feature names must be non-empty strings without whitespace");
            if (!names.insert(name).second) fail("duplicate feature name");
            out.push_back(name);
        }
        if (rc != SQLITE_DONE) fail(sqlite3_errmsg(db_.get()));
        if (out.empty()) fail("feature_columns is empty");
        return out;
    }

private:
    [[noreturn]] static void fail(const std::string& message)
    { throw std::runtime_error("model metadata: " + message); }

    static std::string text(sqlite3_stmt* stmt, int column)
    {
        const auto value = sqlite3_column_text(stmt, column);
        return value ? std::string(reinterpret_cast<const char*>(value),
                                   sqlite3_column_bytes(stmt, column)) : std::string();
    }

    Statement prepare(const char* sql, const char* path = nullptr)
    {
        sqlite3_stmt* raw = nullptr;
        const int rc = sqlite3_prepare_v2(db_.get(), sql, -1, &raw, nullptr);
        Statement stmt(raw);
        if (rc != SQLITE_OK) fail(sqlite3_errmsg(db_.get()));
        if (sqlite3_bind_text(stmt.get(), 1, json_.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK)
            fail("cannot bind JSON");
        if (path && sqlite3_bind_text(stmt.get(), 2, path, -1, SQLITE_TRANSIENT) != SQLITE_OK)
            fail("cannot bind JSON field");
        return stmt;
    }

    Statement query(const char* sql, const char* path = nullptr)
    {
        auto stmt = prepare(sql, path);
        if (sqlite3_step(stmt.get()) != SQLITE_ROW) fail(sqlite3_errmsg(db_.get()));
        return stmt;
    }

    std::string json_;
    std::unique_ptr<sqlite3, CloseDb> db_;
};

} // namespace

struct LambdaRankModel::Impl {
    void* booster = nullptr;
    std::vector<std::string> columns;
    std::string group;
    bool normalize = true;
    mutable std::mutex prediction_mutex;
    ~Impl() { if (booster) LGBM_BoosterFree(booster); }
};

LambdaRankModel::LambdaRankModel(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
LambdaRankModel::~LambdaRankModel() = default;
LambdaRankModel::LambdaRankModel(LambdaRankModel&&) noexcept = default;
LambdaRankModel& LambdaRankModel::operator=(LambdaRankModel&&) noexcept = default;

LambdaRankModel LambdaRankModel::Load(const std::string& directory)
{
    auto impl = std::make_unique<Impl>();
    Metadata metadata(readFile(std::filesystem::path(directory) / "artifact.json"));
    if (metadata.field("$.model_type", "lightgbm_lambdarank") != "lightgbm_lambdarank")
        throw std::runtime_error("unsupported model type");
    if (metadata.field("$.candidate_policy", "all_no_init") != "all_no_init")
        throw std::runtime_error("unsupported candidate policy");
    if (metadata.field("$.model_file", "model.txt") != "model.txt")
        throw std::runtime_error("unsupported model file");
    const auto normalization = metadata.field("$.normalization", "per_query_minmax");
    if (normalization != "per_query_minmax" && normalization != "none")
        throw std::runtime_error("unsupported normalization");
    impl->normalize = normalization == "per_query_minmax";
    impl->group = metadata.field("$.group_column", "instance_id");
    if (blank(impl->group) || impl->group == "file_id" || impl->group == "score" || impl->group == "rank")
        throw std::runtime_error("invalid query group column");
    impl->columns = metadata.features();

    int iterations = 0;
    checkLightGbm(LGBM_BoosterCreateFromModelfile(
        (std::filesystem::path(directory) / "model.txt").string().c_str(), &iterations, &impl->booster));
    int count = 0, classes = 0;
    checkLightGbm(LGBM_BoosterGetNumFeature(impl->booster, &count));
    checkLightGbm(LGBM_BoosterGetNumClasses(impl->booster, &classes));
    if (classes != 1) throw std::runtime_error("ranking model must return one score per file");
    if (count < 1 || static_cast<std::size_t>(count) != impl->columns.size())
        throw std::runtime_error("model feature count does not match metadata");
    std::size_t capacity = 256;
    for (;;) {
        std::vector<std::vector<char>> storage(count, std::vector<char>(capacity));
        std::vector<char*> names;
        for (auto& name : storage) names.push_back(name.data());
        std::size_t required = 0;
        int returned = 0;
        checkLightGbm(LGBM_BoosterGetFeatureNames(impl->booster, count, &returned,
                                                capacity, &required, names.data()));
        if (returned != count) throw std::runtime_error("invalid model feature names");
        if (required > capacity) { capacity = required; continue; }
        for (int i = 0; i < count; ++i)
            if (impl->columns[i] != names[i])
                throw std::runtime_error("model feature names/order do not match metadata");
        break;
    }
    return LambdaRankModel(std::move(impl));
}

const std::vector<std::string>& LambdaRankModel::featureColumns() const { return impl_->columns; }
const std::string& LambdaRankModel::groupColumn() const { return impl_->group; }

LtrFeatureTable LambdaRankModel::prepare(const LtrFeatureTable& input) const
{
    if (input.rows.empty()) throw std::invalid_argument("ranking input is empty");
    std::map<std::string, std::size_t> positions;
    for (std::size_t i = 0; i < input.columns.size(); ++i)
        if (!positions.emplace(input.columns[i], i).second)
            throw std::invalid_argument("duplicate input feature column");
    std::vector<std::size_t> select;
    for (const auto& name : impl_->columns) {
        const auto at = positions.find(name);
        if (at == positions.end()) throw std::invalid_argument("missing feature: " + name);
        select.push_back(at->second);
    }
    LtrFeatureTable out;
    out.columns = impl_->columns;
    std::set<std::pair<std::string, std::string>> candidates;
    std::set<std::string> queries, retained;
    for (const auto& row : input.rows) {
        if (row.values.size() != input.columns.size()) throw std::invalid_argument("invalid feature row width");
        if (blank(row.query_id)) throw std::invalid_argument("query ID must be non-empty");
        if (blank(row.file_id)) throw std::invalid_argument("file ID must be non-empty");
        if (!candidates.emplace(row.query_id, row.file_id).second)
            throw std::invalid_argument("duplicate (query, file) candidate");
        queries.insert(row.query_id);
        if (initializer(row.file_id)) continue;
        retained.insert(row.query_id);
        LtrFeatureRow chosen{row.query_id, row.file_id, {}};
        chosen.values.reserve(select.size());
        for (const auto index : select) {
            if (std::isinf(row.values[index])) throw std::invalid_argument("features must be finite or NaN");
            chosen.values.push_back(row.values[index]);
        }
        out.rows.push_back(std::move(chosen));
    }
    if (queries != retained) throw std::invalid_argument("query has no candidates after excluding __init__.py");
    if (!impl_->normalize) return out;

    struct Range { double low = INFINITY, high = -INFINITY; };
    std::map<std::string, std::vector<Range>> ranges;
    for (const auto& row : out.rows) {
        auto& group = ranges[row.query_id];
        if (group.empty()) group.resize(select.size());
        for (std::size_t i = 0; i < select.size(); ++i) {
            if (std::isnan(row.values[i])) continue;
            group[i].low = std::min(group[i].low, row.values[i]);
            group[i].high = std::max(group[i].high, row.values[i]);
        }
    }
    for (auto& row : out.rows) {
        const auto& group = ranges.at(row.query_id);
        for (std::size_t i = 0; i < select.size(); ++i) {
            if (std::isnan(row.values[i])) continue;
            row.values[i] = group[i].high > group[i].low
                ? (row.values[i] - group[i].low) / (group[i].high - group[i].low) : 0.0;
        }
    }
    return out;
}

std::vector<LtrRankedFile> LambdaRankModel::rank(const LtrFeatureTable& input) const
{
    const auto prepared = prepare(input);
    const auto rows = prepared.rows.size(), cols = prepared.columns.size();
    if (rows > static_cast<std::size_t>(INT32_MAX) || cols > static_cast<std::size_t>(INT32_MAX) ||
        rows > std::numeric_limits<std::size_t>::max() / cols)
        throw std::invalid_argument("ranking matrix is too large");
    std::vector<double> data;
    data.reserve(rows * cols);
    for (const auto& row : prepared.rows) data.insert(data.end(), row.values.begin(), row.values.end());
    std::vector<double> scores(rows);
    std::int64_t length = 0;
    {
        std::lock_guard<std::mutex> lock(impl_->prediction_mutex);
        checkLightGbm(LGBM_BoosterPredictForMat(impl_->booster, data.data(), LightGbmApi::Float64,
            static_cast<std::int32_t>(rows), static_cast<std::int32_t>(cols), 1,
            LightGbmApi::NormalPrediction, 0, -1, "num_threads=1", &length, scores.data()));
    }
    if (length != static_cast<std::int64_t>(rows)) throw std::runtime_error("invalid prediction count");
    std::vector<LtrRankedFile> result;
    result.reserve(rows);
    for (std::size_t i = 0; i < rows; ++i) {
        if (!std::isfinite(scores[i])) throw std::runtime_error("model returned a non-finite score");
        result.push_back({prepared.rows[i].query_id, prepared.rows[i].file_id, scores[i], 0});
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        if (a.query_id != b.query_id) return a.query_id < b.query_id;
        if (a.score != b.score) return a.score > b.score;
        return a.file_id < b.file_id;
    });
    int rank = 0;
    std::string query;
    for (auto& row : result) {
        if (row.query_id != query) { query = row.query_id; rank = 0; }
        row.rank = ++rank;
    }
    return result;
}

} // namespace TS
