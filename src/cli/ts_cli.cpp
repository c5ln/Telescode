// src/cli/ts_cli.cpp
// Shared subcommand dispatch and option parsing.

#include "cli/ts_cli.h"

#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>

namespace TS {

void PrintCoreUsage(std::FILE* out)
{
    std::fprintf(out,
        "  Telescode scan     <repo_path> <db_path> [allowed_root]\n"
        "  Telescode algo     <db_path>\n"
        "  Telescode update   <op> <db_path> ...    (op: file|files|delete|rename|dangling)\n"
        "  Telescode graph    <db_path> [--json] [--pretty] [--algo] [-o <file>]\n"
        "  Telescode sequence <db_path> [--json] [--pretty] [--algo] [-o <file>]\n"
        "  Telescode analyze  <db_path> [--json] [--pretty] [--algo] [-o <file>]\n"
        "\n"
        "graph / sequence / analyze read an existing database and print JSON.\n"
        "Run `scan` first to create one. --algo recomputes the reading sequence\n"
        "and complexity scores before reading, as the viewer does at startup;\n"
        "without it the database is reported exactly as it stands.\n");
}

bool ParseJsonCmdOptions(int argc, char* argv[], const char* usage,
                         JsonCmdOptions& out)
{
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];

        // --json is accepted and is also the default: these commands exist to
        // produce JSON, and rejecting the flag that says so would be unhelpful.
        if (std::strcmp(a, "--json")   == 0) continue;
        if (std::strcmp(a, "--pretty") == 0) { out.pretty   = true; continue; }
        if (std::strcmp(a, "--algo")   == 0) { out.run_algo = true; continue; }

        if (std::strcmp(a, "-o") == 0 || std::strcmp(a, "--out") == 0) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "Telescode: %s needs a file path\n\n%s\n", a, usage);
                return false;
            }
            out.out_path = argv[++i];
            continue;
        }

        if (a[0] == '-') {
            std::fprintf(stderr, "Telescode: unknown option '%s'\n\n%s\n", a, usage);
            return false;
        }

        if (out.db_path.empty()) { out.db_path = a; continue; }

        std::fprintf(stderr, "Telescode: unexpected argument '%s'\n\n%s\n", a, usage);
        return false;
    }

    if (out.db_path.empty()) {
        std::fprintf(stderr, "Telescode: a database path is required\n\n%s\n", usage);
        return false;
    }

    // Writing the report over its own source destroys it. AnalysisService closes
    // the database before emit() runs, so nothing holds a lock by then and the
    // fopen(..., "wb") simply truncates it -- a 233KB database becomes 30KB of
    // JSON and the command still reports success. Refuse instead.
    if (!out.out_path.empty() && SameFilePath(out.out_path, out.db_path)) {
        std::fprintf(stderr,
            "Telescode: --out would overwrite the database '%s'\n"
            "           choose a different output path\n\n%s\n",
            out.db_path.c_str(), usage);
        return false;
    }
    return true;
}

bool SameFilePath(const std::string& a, const std::string& b)
{
    namespace fs = std::filesystem;

    if (a == b) return true;   // the common case, and needs no filesystem access

    // When both paths exist the filesystem can answer authoritatively, which also
    // settles hard links, symlinks, and the case-insensitive spellings Windows
    // accepts for one file.
    std::error_code ec;
    if (fs::exists(a, ec) && !ec && fs::exists(b, ec) && !ec) {
        const bool same = fs::equivalent(a, b, ec);
        if (!ec) return same;
    }

    // Otherwise compare resolved paths. The output file is normally absent until
    // it is written, so canonical() and equivalent() are not available -- and
    // weakly_canonical alone is not enough either: given a bare relative name
    // with no existing prefix it can return that name unchanged, which would
    // never match an absolute spelling of the same file. Make both absolute
    // first, then resolve what exists and normalise the rest away.
    const auto resolve = [](const std::string& p) -> fs::path {
        std::error_code e;
        fs::path abs = fs::absolute(fs::path(p), e);
        if (e) return fs::path(p).lexically_normal();
        fs::path wc = fs::weakly_canonical(abs, e);
        return e ? abs.lexically_normal() : wc;
    };

    return resolve(a) == resolve(b);
}

bool DispatchCoreCommand(int argc, char* argv[], int& exit_code)
{
    if (argc <= 1) return false;

    // Shifting argv by one puts the subcommand name in argv[0], so each Cmd*
    // keeps the index arithmetic it had when it was its own binary.
    const char* cmd = argv[1];
    const int   sub_argc = argc - 1;
    char** const sub_argv = argv + 1;

    if (std::strcmp(cmd, "scan")     == 0) { exit_code = CmdScan    (sub_argc, sub_argv); return true; }
    if (std::strcmp(cmd, "algo")     == 0) { exit_code = CmdAlgo    (sub_argc, sub_argv); return true; }
    if (std::strcmp(cmd, "update")   == 0) { exit_code = CmdUpdate  (sub_argc, sub_argv); return true; }
    if (std::strcmp(cmd, "graph")    == 0) { exit_code = CmdGraph   (sub_argc, sub_argv); return true; }
    if (std::strcmp(cmd, "sequence") == 0) { exit_code = CmdSequence(sub_argc, sub_argv); return true; }
    if (std::strcmp(cmd, "analyze")  == 0) { exit_code = CmdAnalyze (sub_argc, sub_argv); return true; }

    return false;
}

} // namespace TS
