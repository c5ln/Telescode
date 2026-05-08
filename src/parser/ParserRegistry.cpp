#include "ParserRegistry.h"
#include <filesystem>

namespace fs = std::filesystem;

void ParserRegistry::registerParser(const std::string& ext,
                                     std::unique_ptr<IParser> parser) {
    parsers_[ext] = std::move(parser);
}

ParseResult ParserRegistry::parseFile(const std::string& filePath,
                                       const std::string& repoRoot) {
    std::string ext = fs::path(filePath).extension().string();
    auto it = parsers_.find(ext);
    if (it == parsers_.end()) return ParseResult{};
    return it->second->parseFile(filePath, repoRoot);
}

std::vector<ParseResult> ParserRegistry::parseDirectory(
    const std::string& dirPath,
    const std::string& allowedRoot)
{
    std::vector<ParseResult> all;
    for (auto& [ext, parser] : parsers_) {
        auto r = parser->parseDirectory(dirPath, allowedRoot);
        all.insert(all.end(),
                   std::make_move_iterator(r.begin()),
                   std::make_move_iterator(r.end()));
    }
    return all;
}
