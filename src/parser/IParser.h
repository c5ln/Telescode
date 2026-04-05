#pragma once

#include <string>
#include <vector>
#include "ParseResult.h"

class IParser {
public:
    virtual ~IParser() = default;

    // Parse a single file. repoRoot is used to compute the relative file_id.
    virtual ParseResult parseFile(const std::string& filePath,
                                  const std::string& repoRoot) = 0;

    // Recursively parse all language-appropriate files under dirPath.
    // repoRoot is set to dirPath itself.
    virtual std::vector<ParseResult> parseDirectory(const std::string& dirPath) = 0;
};
