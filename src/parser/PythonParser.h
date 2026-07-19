#pragma once

#include "IParser.h"
#include <string>
#include <vector>

class PythonParser : public IParser {
public:
    ParseResult parseFile(const std::string& filePath,
                          const std::string& repoRoot) override;

    std::vector<ParseResult> parseDirectory(
        const std::string& dirPath,
        const std::string& allowedRoot = "") override;
};
