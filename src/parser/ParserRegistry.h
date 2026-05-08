#pragma once

#include "IParser.h"
#include <map>
#include <memory>
#include <string>

class ParserRegistry : public IParser {
public:
    void registerParser(const std::string& ext, std::unique_ptr<IParser> parser);

    ParseResult parseFile(const std::string& filePath,
                          const std::string& repoRoot) override;

    std::vector<ParseResult> parseDirectory(
        const std::string& dirPath,
        const std::string& allowedRoot = "") override;

private:
    std::map<std::string, std::unique_ptr<IParser>> parsers_;
};
