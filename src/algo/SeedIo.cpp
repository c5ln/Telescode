#include "SeedIo.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

std::vector<std::string> splitCsvLine(const std::string& line)
{
    std::vector<std::string> out;
    std::string cur;
    bool in_quotes = false;

    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (in_quotes) {
            if (c == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') { cur += '"'; ++i; }
                else in_quotes = false;
            } else {
                cur += c;
            }
        } else if (c == '"') {
            in_quotes = true;
        } else if (c == ',') {
            out.push_back(cur);
            cur.clear();
        } else if (c != '\r') {          // tolerate CRLF
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

namespace {

int columnIndex(const std::vector<std::string>& header, const std::string& name)
{
    for (std::size_t i = 0; i < header.size(); ++i)
        if (header[i] == name) return static_cast<int>(i);
    return -1;
}

std::string trim(const std::string& s)
{
    std::size_t b = s.find_first_not_of(" \t");
    if (b == std::string::npos) return {};
    std::size_t e = s.find_last_not_of(" \t");
    return s.substr(b, e - b + 1);
}

}  // namespace

SeedLoadResult loadSeedCsv(const std::string& path)
{
    SeedLoadResult r;

    std::ifstream in(path);
    if (!in) {
        r.error = "cannot open seed csv: " + path;
        return r;
    }

    std::string line;
    if (!std::getline(in, line)) {
        r.error = "seed csv is empty (no header): " + path;
        return r;
    }

    const std::vector<std::string> header = splitCsvLine(line);
    const int i_file = columnIndex(header, "file_id");
    const int i_w    = columnIndex(header, "weight");
    if (i_file < 0 || i_w < 0) {
        r.error = "seed csv header must contain file_id and weight: " + path;
        return r;
    }

    int lineno = 1;
    while (std::getline(in, line)) {
        ++lineno;
        if (trim(line).empty()) continue;

        const std::vector<std::string> f = splitCsvLine(line);
        if (static_cast<int>(f.size()) <= i_file || static_cast<int>(f.size()) <= i_w) {
            r.error = "seed csv line " + std::to_string(lineno) + ": too few columns";
            return r;
        }

        const std::string fid = trim(f[i_file]);
        const std::string ws  = trim(f[i_w]);
        if (fid.empty()) {
            r.error = "seed csv line " + std::to_string(lineno) + ": empty file_id";
            return r;
        }
        if (ws.empty()) {
            r.error = "seed csv line " + std::to_string(lineno) + ": empty weight";
            return r;
        }

        errno = 0;
        char* end = nullptr;
        const double w = std::strtod(ws.c_str(), &end);
        if (end == ws.c_str() || *end != '\0' || errno == ERANGE) {
            r.error = "seed csv line " + std::to_string(lineno) +
                      ": weight is not a number: '" + ws + "'";
            return r;
        }

        r.seeds.push_back({fid, w});
    }

    return r;
}

std::string writePprCsv(const std::string& path, const std::vector<PprEntry>& rows)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return "cannot open output csv for writing: " + path;

    out << "file_id,ppr\n";
    char buf[64];
    for (const PprEntry& e : rows) {
        const bool needs_quotes =
            e.file_id.find_first_of(",\"\n\r") != std::string::npos;
        if (needs_quotes) {
            out << '"';
            for (char c : e.file_id) { if (c == '"') out << '"'; out << c; }
            out << '"';
        } else {
            out << e.file_id;
        }
        std::snprintf(buf, sizeof(buf), "%.17g", e.ppr);
        out << ',' << buf << '\n';
    }

    out.flush();
    if (!out) return "write failed: " + path;
    return {};
}
