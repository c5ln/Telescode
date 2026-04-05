#pragma once

#include "IParser.h"
#include <string>
#include <vector>

class PythonParser : public IParser {
public:
    PythonParser();
    ~PythonParser() override;

    ParseResult parseFile(const std::string& filePath,
                          const std::string& repoRoot) override;

    std::vector<ParseResult> parseDirectory(
        const std::string& dirPath,
        const std::string& allowedRoot = "") override;

private:
    // Opaque tree-sitter parser handle stored as void* to avoid including
    // tree_sitter/api.h in the header (implementation detail).
    void* m_parser;
};
