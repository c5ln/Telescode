#pragma once

#include <memory>
#include <string>
#include <vector>

namespace TS {

struct LtrFeatureRow {
    std::string query_id;
    std::string file_id;
    std::vector<double> values;
};

struct LtrFeatureTable {
    std::vector<std::string> columns;
    std::vector<LtrFeatureRow> rows;
};

struct LtrRankedFile {
    std::string query_id;
    std::string file_id;
    double score;
    int rank;
};

// Owns a LightGBM booster. Loading validates metadata and model feature order;
// preparation/ranking never change caller data or write the artifact.
class LambdaRankModel {
public:
    static LambdaRankModel Load(const std::string& artifact_directory);
    ~LambdaRankModel();
    LambdaRankModel(LambdaRankModel&&) noexcept;
    LambdaRankModel& operator=(LambdaRankModel&&) noexcept;
    LambdaRankModel(const LambdaRankModel&) = delete;
    LambdaRankModel& operator=(const LambdaRankModel&) = delete;

    const std::vector<std::string>& featureColumns() const;
    const std::string& groupColumn() const;
    LtrFeatureTable prepare(const LtrFeatureTable&) const;
    std::vector<LtrRankedFile> rank(const LtrFeatureTable&) const;

private:
    struct Impl;
    explicit LambdaRankModel(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace TS
