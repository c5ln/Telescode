#include "db/DbUpdater.h"
#include "parser/ParserRegistry.h"
#include "parser/PythonParser.h"
#include <sqlite3.h>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

static ParserRegistry makeRegistry() {
    ParserRegistry reg;
    reg.registerParser(".py", std::make_unique<PythonParser>());
    return reg;
}

static void printUsage(const char* prog) {
    std::fprintf(stderr,
        "Usage:\n"
        "  %s file     <db> <repo_root> <abs_path>\n"
        "  %s files    <db> <repo_root> <file1> [file2 ...]\n"
        "  %s delete   <db> <file_id>\n"
        "  %s rename   <db> <repo_root> <old_id> <new_abs_path>\n"
        "  %s dangling <db> <file_id>\n",
        prog, prog, prog, prog, prog);
}

int main(int argc, char* argv[]) {
    if (argc < 3) { printUsage(argv[0]); return 1; }

    const char* cmd    = argv[1];
    const char* dbPath = argv[2];

    if (std::strcmp(cmd, "file") == 0) {
        if (argc != 5) {
            std::fprintf(stderr, "Usage: %s file <db> <repo_root> <abs_path>\n", argv[0]);
            return 1;
        }
        auto registry = makeRegistry();
        ParseResult result = registry.parseFile(argv[4], argv[3]);
        int rc = DbUpdater::updateFile(dbPath, result);
        if (rc != SQLITE_OK) {
            std::fprintf(stderr, "[error] updateFile failed (code %d)\n", rc);
            return 1;
        }
        std::fprintf(stdout, "[update] %s\n", result.file.file_id.c_str());

    } else if (std::strcmp(cmd, "files") == 0) {
        if (argc < 5) {
            std::fprintf(stderr, "Usage: %s files <db> <repo_root> <file1> [file2...]\n", argv[0]);
            return 1;
        }
        const char* repoRoot = argv[3];
        auto registry = makeRegistry();
        std::vector<ParseResult> results;
        for (int i = 4; i < argc; ++i)
            results.push_back(registry.parseFile(argv[i], repoRoot));
        int rc = DbUpdater::updateFiles(dbPath, results);
        if (rc != SQLITE_OK) {
            std::fprintf(stderr, "[error] updateFiles failed (code %d)\n", rc);
            return 1;
        }
        std::fprintf(stdout, "[update] %zu files\n", results.size());

    } else if (std::strcmp(cmd, "delete") == 0) {
        if (argc != 4) {
            std::fprintf(stderr, "Usage: %s delete <db> <file_id>\n", argv[0]);
            return 1;
        }
        int rc = DbUpdater::deleteFile(dbPath, argv[3]);
        if (rc != SQLITE_OK) {
            std::fprintf(stderr, "[error] deleteFile failed (code %d)\n", rc);
            return 1;
        }
        std::fprintf(stdout, "[delete] %s\n", argv[3]);

    } else if (std::strcmp(cmd, "rename") == 0) {
        if (argc != 6) {
            std::fprintf(stderr, "Usage: %s rename <db> <repo_root> <old_id> <new_abs_path>\n", argv[0]);
            return 1;
        }
        auto registry = makeRegistry();
        ParseResult newResult = registry.parseFile(argv[5], argv[3]);
        int rc = DbUpdater::renameFile(dbPath, argv[4], newResult);
        if (rc != SQLITE_OK) {
            std::fprintf(stderr, "[error] renameFile failed (code %d)\n", rc);
            return 1;
        }
        std::fprintf(stdout, "[rename] %s -> %s\n", argv[4], newResult.file.file_id.c_str());

    } else if (std::strcmp(cmd, "dangling") == 0) {
        if (argc != 4) {
            std::fprintf(stderr, "Usage: %s dangling <db> <file_id>\n", argv[0]);
            return 1;
        }
        auto files = DbUpdater::findImportingFiles(dbPath, argv[3]);
        for (const auto& f : files)
            std::fprintf(stdout, "%s\n", f.c_str());

    } else {
        std::fprintf(stderr, "[error] Unknown command: %s\n", cmd);
        printUsage(argv[0]);
        return 1;
    }

    return 0;
}
