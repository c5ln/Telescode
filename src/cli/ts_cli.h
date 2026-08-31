// src/cli/ts_cli.h
// Subcommand entry points, dispatched from main().
//
// Each of these was a standalone executable's main(). main() forwards to them
// with argv shifted by one, so argv[0] is the subcommand name and every index
// after it keeps the meaning it had as a separate binary.

#pragma once

namespace TS {

int CmdScan  (int argc, char* argv[]);   // scan   <repo_path> <db_path> [allowed_root]
int CmdUpdate(int argc, char* argv[]);   // update <op> <db> ...
int CmdAlgo  (int argc, char* argv[]);   // algo   <db_path>

} // namespace TS
