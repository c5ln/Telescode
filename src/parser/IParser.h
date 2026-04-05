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
    // allowedRoot: if non-empty, only files whose absolute path starts with
    // allowedRoot are parsed. Acts as a whitelist boundary — dirPath must be
    // within allowedRoot, otherwise parsing is rejected and an empty result
    // is returned.
    // If allowedRoot is empty, no boundary check is applied.
    virtual std::vector<ParseResult> parseDirectory(
        const std::string& dirPath,
        const std::string& allowedRoot = "") = 0;
};
