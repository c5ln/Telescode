#pragma once

#include "ParseResult.h"
#include <string>
#include <vector>

class CsvExporter {
public:
    // Export all parse results to CSV files in outputDir.
    // Creates: file.csv, class.csv, base_class.csv, function.csv, param.csv, link.csv
    static void exportAll(const std::vector<ParseResult>& results,
                          const std::string& outputDir);

private:
    // Escape a CSV field value: wraps in quotes if it contains comma, quote, or newline.
    static std::string escapeCsvField(const std::string& value);
};
