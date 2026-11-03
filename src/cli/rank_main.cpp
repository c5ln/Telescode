#include "ts_cli.h"
#include "ranking/LambdaRankModel.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace TS {
namespace {
namespace fs = std::filesystem;

constexpr const char* usage =
    "Usage: Telescode rank --model <dir> --features <csv> --output <csv>\n";

// Read one RFC 4180 record, including quoted delimiters/newlines. Identifiers
// remain strings: e.g. NA and 0001 must never become missing/numeric values.
bool readRecord(std::istream& in, std::vector<std::string>& fields)
{
    enum class State { Start, Text, Quoted, AfterQuote };
    State state = State::Start;
    fields.clear();
    std::string value;
    bool read = false;
    char c;
    while (in.get(c)) {
        read = true;
        if (state == State::Quoted) {
            if (c == '"') state = State::AfterQuote;
            else value += c;
            continue;
        }
        if (state == State::AfterQuote && c == '"') {
            value += '"'; state = State::Quoted; continue;
        }
        if (c == ',' || c == '\n' || c == '\r') {
            fields.push_back(std::move(value));
            value.clear();
            state = State::Start;
            if (c == ',') continue;
            if (c == '\r' && in.peek() == '\n') in.get();
            return true;
        }
        if (state == State::AfterQuote) throw std::invalid_argument("invalid CSV after closing quote");
        if (c == '"') {
            if (state != State::Start) throw std::invalid_argument("invalid CSV quote inside field");
            state = State::Quoted;
        } else {
            value += c;
            state = State::Text;
        }
    }
    if (in.bad()) throw std::runtime_error("error reading feature CSV");
    if (state == State::Quoted) throw std::invalid_argument("unterminated CSV quote");
    if (!read) return false;
    fields.push_back(std::move(value));
    return true;
}

bool blankRecord(const std::vector<std::string>& row)
{
    return row.size() == 1 && std::all_of(row[0].begin(), row[0].end(),
                                        [](unsigned char c) { return std::isspace(c); });
}

double number(const std::string& text)
{
    // Match the missing feature spellings accepted by the Python CSV reader.
    static const std::set<std::string> missing = {"", "#N/A", "#N/A N/A", "#NA", "-1.#IND",
        "-1.#QNAN", "-NaN", "-nan", "1.#IND", "1.#QNAN", "<NA>", "N/A", "NA", "NULL",
        "NaN", "None", "n/a", "nan", "null"};
    if (missing.count(text)) return std::numeric_limits<double>::quiet_NaN();
    auto begin = text.data(), end = begin + text.size();
    while (begin != end && std::isspace(static_cast<unsigned char>(*begin))) ++begin;
    while (begin != end && std::isspace(static_cast<unsigned char>(end[-1]))) --end;
    if (begin != end && *begin == '+') ++begin;
    double value = 0;
    const auto parsed = std::from_chars(begin, end, value, std::chars_format::general);
    if (parsed.ec != std::errc{} || parsed.ptr != end)
        throw std::invalid_argument("invalid numeric feature: " + text);
    return value;
}

LtrFeatureTable readFeatures(const fs::path& path, const LambdaRankModel& model)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open feature CSV: " + path.string());
    char bom[3]{};
    in.read(bom, 3);
    if (in.gcount() != 3 || std::string(bom, 3) != "\xef\xbb\xbf") { in.clear(); in.seekg(0); }
    std::vector<std::string> header;
    while (readRecord(in, header) && blankRecord(header)) {}
    if (header.empty() || blankRecord(header)) throw std::invalid_argument("missing feature CSV header");
    const std::set<std::string> names(header.begin(), header.end());
    if (names.size() != header.size()) throw std::invalid_argument("duplicate CSV column name");
    const auto groupAt = std::find(header.begin(), header.end(), model.groupColumn());
    const auto fileAt = std::find(header.begin(), header.end(), "file_id");
    if (groupAt == header.end() || fileAt == header.end()) throw std::invalid_argument("missing query/file ID column");
    const auto group = std::distance(header.begin(), groupAt), file = std::distance(header.begin(), fileAt);
    const std::set<std::string> required(model.featureColumns().begin(), model.featureColumns().end());
    LtrFeatureTable table;
    std::vector<std::size_t> selected;
    for (std::size_t i = 0; i < header.size(); ++i) {
        if (i == static_cast<std::size_t>(group) || i == static_cast<std::size_t>(file)) continue;
        table.columns.push_back(header[i]);
        selected.push_back(i);
    }
    std::vector<std::string> fields;
    while (readRecord(in, fields)) {
        if (blankRecord(fields)) continue;
        if (fields.size() != header.size()) throw std::invalid_argument("invalid CSV row width");
        LtrFeatureRow row{fields[group], fields[file], {}};
        const auto slash = row.file_id.find_last_of("/\\");
        const bool excluded = row.file_id.substr(slash == std::string::npos ? 0 : slash + 1) == "__init__.py";
        for (const auto index : selected)
            row.values.push_back(!excluded && required.count(header[index])
                ? number(fields[index]) : std::numeric_limits<double>::quiet_NaN());
        table.rows.push_back(std::move(row));
    }
    return table;
}

std::string csv(const std::string& text)
{
    if (text.find_first_of(",\"\r\n") == std::string::npos) return text;
    std::string out = "\"";
    for (char c : text) { if (c == '"') out += '"'; out += c; }
    return out + '"';
}

// Claim a fresh directory in the destination filesystem, then atomically
// replace the output only after parsing, inference and writing all succeed.
class OutputTransaction {
public:
    explicit OutputTransaction(const fs::path& output) : target_(fs::absolute(output))
    {
        static std::atomic<unsigned> serial{0};
        for (int attempt = 0; attempt < 100; ++attempt) {
            directory_ = target_.parent_path() / (".telescode-rank-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                "-" + std::to_string(serial++));
            if (fs::create_directory(directory_)) return;
        }
        throw std::runtime_error("cannot create temporary output directory");
    }
    ~OutputTransaction() { std::error_code ec; fs::remove_all(directory_, ec); }
    fs::path temporary() const { return directory_ / "ranked.csv"; }
    void commit() { fs::rename(temporary(), target_); }
private:
    fs::path target_, directory_;
};

} // namespace

int CmdRank(int argc, char* argv[])
{
    std::string modelPath, featurePath, outputPath;
    for (int i = 1; i < argc; ++i) {
        const std::string option = argv[i];
        if (option == "--help" || option == "-h") { std::fputs(usage, stdout); return 0; }
        std::string* value = option == "--model" ? &modelPath : option == "--features" ? &featurePath
                           : (option == "--output" || option == "-o") ? &outputPath : nullptr;
        if (!value || !value->empty() || i + 1 == argc) { std::fputs(usage, stderr); return 2; }
        *value = argv[++i];
    }
    if (modelPath.empty() || featurePath.empty() || outputPath.empty()) { std::fputs(usage, stderr); return 2; }
    try {
        for (const auto& input : {fs::path(featurePath), fs::path(modelPath) / "model.txt",
                                   fs::path(modelPath) / "artifact.json"})
            if (SameFilePath(outputPath, input.string())) throw std::invalid_argument("output would overwrite an input/model file");
        const auto model = LambdaRankModel::Load(modelPath);
        const auto ranked = model.rank(readFeatures(featurePath, model));
        OutputTransaction transaction(outputPath);
        std::ofstream out(transaction.temporary(), std::ios::binary);
        if (!out) throw std::runtime_error("cannot write ranking output");
        out.imbue(std::locale::classic());
        out << std::setprecision(17) << csv(model.groupColumn()) << ",file_id,score,rank\n";
        for (const auto& row : ranked)
            out << csv(row.query_id) << ',' << csv(row.file_id) << ',' << row.score << ',' << row.rank << '\n';
        out.close();
        if (!out) throw std::runtime_error("error writing ranking output");
        transaction.commit();
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Telescode rank: %s\n", e.what());
        return 1;
    }
}

} // namespace TS
