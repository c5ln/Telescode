#include "parser/PythonParser.h"
#include "parser/CsvExporter.h"
#include "parser/ParseResult.h"

#include <iostream>
#include <string>
#include <vector>

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <repo_path> <output_dir>\n";
        return 1;
    }

    std::string repoPath  = argv[1];
    std::string outputDir = argv[2];

    PythonParser parser;

    std::cout << "Parsing Python files in: " << repoPath << "\n";
    std::vector<ParseResult> results = parser.parseDirectory(repoPath);

    // Compute summary counts
    size_t totalClasses   = 0;
    size_t totalFunctions = 0;
    size_t totalLinks     = 0;
    for (const auto& pr : results) {
        totalClasses   += pr.classes.size();
        totalFunctions += pr.functions.size();
        totalLinks     += pr.links.size();
    }

    std::cout << "Parsed " << results.size()   << " file(s)\n";
    std::cout << "  Classes:   " << totalClasses   << "\n";
    std::cout << "  Functions: " << totalFunctions << "\n";
    std::cout << "  Links:     " << totalLinks     << "\n";

    std::cout << "Exporting CSV files to: " << outputDir << "\n";
    try {
        CsvExporter::exportAll(results, outputDir);
        std::cout << "Export complete.\n";
    } catch (const std::exception& ex) {
        std::cerr << "Export failed: " << ex.what() << "\n";
        return 1;
    }

    return 0;
}
