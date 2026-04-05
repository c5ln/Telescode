#include "CsvExporter.h"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// ─── CSV escaping ─────────────────────────────────────────────────────────────
// RFC 4180: if a field contains a comma, double-quote, or newline, wrap it in
// double-quotes and double any embedded double-quotes.

std::string CsvExporter::escapeCsvField(const std::string& value) {
    bool needsQuoting = (value.find(',')  != std::string::npos ||
                         value.find('"')  != std::string::npos ||
                         value.find('\n') != std::string::npos ||
                         value.find('\r') != std::string::npos);
    if (!needsQuoting) return value;

    std::string out;
    out.reserve(value.size() + 2);
    out += '"';
    for (char c : value) {
        if (c == '"') out += '"'; // double the quote
        out += c;
    }
    out += '"';
    return out;
}

// ─── exportAll ────────────────────────────────────────────────────────────────

void CsvExporter::exportAll(const std::vector<ParseResult>& results,
                            const std::string& outputDir) {
    fs::path outPath = fs::absolute(outputDir);
    fs::create_directories(outPath);

    // ── file.csv ──────────────────────────────────────────────────────────────
    {
        std::ofstream ofs(outPath / "file.csv");
        if (!ofs) throw std::runtime_error("Cannot open file.csv for writing");
        ofs << "file_id,file_name,path,language,loc\n";
        for (const auto& pr : results) {
            const auto& f = pr.file;
            ofs << escapeCsvField(f.file_id)   << ','
                << escapeCsvField(f.file_name) << ','
                << escapeCsvField(f.path)      << ','
                << escapeCsvField(f.language)  << ','
                << f.loc                       << '\n';
        }
    }

    // ── class.csv ─────────────────────────────────────────────────────────────
    {
        std::ofstream ofs(outPath / "class.csv");
        if (!ofs) throw std::runtime_error("Cannot open class.csv for writing");
        ofs << "class_id,file_id,class_name,start_line,end_line\n";
        for (const auto& pr : results) {
            for (const auto& c : pr.classes) {
                ofs << escapeCsvField(c.class_id)   << ','
                    << escapeCsvField(c.file_id)    << ','
                    << escapeCsvField(c.class_name) << ','
                    << c.start_line                 << ','
                    << c.end_line                   << '\n';
            }
        }
    }

    // ── base_class.csv ────────────────────────────────────────────────────────
    {
        std::ofstream ofs(outPath / "base_class.csv");
        if (!ofs) throw std::runtime_error("Cannot open base_class.csv for writing");
        ofs << "class_id,base_class_name,ordinal\n";
        for (const auto& pr : results) {
            for (const auto& bc : pr.base_classes) {
                ofs << escapeCsvField(bc.class_id)        << ','
                    << escapeCsvField(bc.base_class_name) << ','
                    << bc.ordinal                         << '\n';
            }
        }
    }

    // ── function.csv ──────────────────────────────────────────────────────────
    {
        std::ofstream ofs(outPath / "function.csv");
        if (!ofs) throw std::runtime_error("Cannot open function.csv for writing");
        ofs << "function_id,parent_id,function_name,nesting_depth,is_async,start_line,end_line,parent_type\n";
        for (const auto& pr : results) {
            for (const auto& fn : pr.functions) {
                ofs << escapeCsvField(fn.function_id)   << ','
                    << escapeCsvField(fn.parent_id)     << ','
                    << escapeCsvField(fn.function_name) << ','
                    << fn.nesting_depth                 << ','
                    << fn.is_async                      << ','
                    << fn.start_line                    << ','
                    << fn.end_line                      << ','
                    << escapeCsvField(fn.parent_type)   << '\n';
            }
        }
    }

    // ── param.csv ─────────────────────────────────────────────────────────────
    {
        std::ofstream ofs(outPath / "param.csv");
        if (!ofs) throw std::runtime_error("Cannot open param.csv for writing");
        ofs << "function_id,param_name,ordinal\n";
        for (const auto& pr : results) {
            for (const auto& p : pr.params) {
                ofs << escapeCsvField(p.function_id) << ','
                    << escapeCsvField(p.param_name)  << ','
                    << p.ordinal                     << '\n';
            }
        }
    }

    // ── link.csv ──────────────────────────────────────────────────────────────
    {
        std::ofstream ofs(outPath / "link.csv");
        if (!ofs) throw std::runtime_error("Cannot open link.csv for writing");
        ofs << "source_id,target_id,link_type\n";
        for (const auto& pr : results) {
            for (const auto& lk : pr.links) {
                ofs << escapeCsvField(lk.source_id) << ','
                    << escapeCsvField(lk.target_id) << ','
                    << escapeCsvField(lk.link_type) << '\n';
            }
        }
    }
}
