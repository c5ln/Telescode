// src/cli/ts_cli.h
// Subcommand entry points and the shared dispatcher.
//
// Each Cmd* was a standalone executable's main(). Callers forward to them with
// argv shifted by one, so argv[0] is the subcommand name and every index after
// it keeps the meaning it had as a separate binary.
//
// DispatchCoreCommand is the single dispatch table: src/cli/headless_main.cpp
// routes every subcommand through it, and PrintCoreUsage is the matching usage
// text.

#pragma once

#include <cstdio>
#include <string>

namespace TS {

// ── Subcommands ──────────────────────────────────────────────────────────────

int CmdScan  (int argc, char* argv[]);   // scan     <repo_path> <db_path> [allowed_root]
int CmdUpdate(int argc, char* argv[]);   // update   <op> <db> ...
int CmdAlgo  (int argc, char* argv[]);   // algo     <db_path>
int CmdGraph (int argc, char* argv[]);   // graph    <db_path> [options]
int CmdSequence(int argc, char* argv[]); // sequence <db_path> [options]
int CmdAnalyze (int argc, char* argv[]); // analyze  <db_path> [options]

// ── Shared options for the JSON-emitting commands ────────────────────────────

struct JsonCmdOptions {
    std::string db_path;
    std::string out_path;            // empty = stdout
    bool        pretty   = false;
    bool        run_algo = false;
};

// Parses <db_path> plus --json / --pretty / --algo / -o <file>.
// Returns false and reports the problem on stderr if the arguments are unusable;
// `usage` is the one-line synopsis shown in that case. Rejects an --out path that
// resolves to the database, which would otherwise be truncated by the report.
bool ParseJsonCmdOptions(int argc, char* argv[], const char* usage,
                         JsonCmdOptions& out);

// Whether two path spellings name the same file, comparing resolved paths so
// "foo.db" and "./foo.db" match. Neither path needs to exist. Exposed for the
// option check above and for its tests.
bool SameFilePath(const std::string& a, const std::string& b);

// ── Dispatch ─────────────────────────────────────────────────────────────────

// The core subcommands, one line each.
void PrintCoreUsage(std::FILE* out);

// Runs argv[1] as a core subcommand when it names one.
// Returns true and fills exit_code if it handled the call; false when argv[1] is
// not a core subcommand, leaving the decision to the caller.
bool DispatchCoreCommand(int argc, char* argv[], int& exit_code);

} // namespace TS
