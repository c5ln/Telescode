#include "parser/PythonParser.h"
#include "parser/CsvExporter.h"
#include "parser/ParseResult.h"

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0]
                  << " <repo_path> <output_dir> [allowed_root]\n"
                  << "  allowed_root: whitelist boundary (default: repos/ relative to CWD)\n";
        return 1;
    }

    std::string repoPath  = argv[1];
    std::string outputDir = argv[2];

    // allowedRoot: whitelist boundary. repo_path must be inside this directory.
    // Default: "repos/" relative to CWD — ensures the parser never touches files
    // outside the designated repos directory.
    std::string allowedRoot;
    if (argc >= 4) {
        allowedRoot = argv[3];
    } else {
        allowedRoot = fs::absolute("repos/").string();
    }

    PythonParser parser;

    std::cout << "Allowed root : " << fs::absolute(allowedRoot).string() << "\n";
    std::cout << "Parsing      : " << repoPath << "\n";
    std::vector<ParseResult> results = parser.parseDirectory(repoPath, allowedRoot);

    // Compute summary counts
    size_t totalClasses   = 0;
    size_t totalFunctions = 0;
    size_t totalLinks     = 0;
    for (const auto& pr : results) {
        totalClasses   += pr.classes.size();
        totalFunctions += pr.functions.size();
        totalLinks     += pr.links.size();
    }

    std::cout << "Parsed       : " << results.size() << " file(s)\n";
    std::cout << "  Classes:   " << totalClasses   << "\n";
    std::cout << "  Functions: " << totalFunctions << "\n";
    std::cout << "  Links:     " << totalLinks     << "\n";

    std::cout << "Exporting    : " << outputDir << "\n";
    try {
        CsvExporter::exportAll(results, outputDir);
        std::cout << "Export complete.\n";
    } catch (const std::exception& ex) {
        std::cerr << "Export failed: " << ex.what() << "\n";
        return 1;
    }

    return 0;
}
