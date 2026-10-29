#pragma once

struct sqlite3;

// Opens (or creates) telescode.db at the given path.
// Applies WAL pragmas and creates all tables if they do not exist.
// Returns SQLITE_OK on success; caller must call sqlite3_close() when done.
int initDb(const char* dbPath, sqlite3** db);

// ── Scan stamp ──────────────────────────────────────────────────────────────
// meta[kScanFormatKey] says which parser output the file/symbol/scope/binding/
// ref rows follow. Every write of parser output -- scan, and each DbUpdater
// operation -- records it; the resolver refuses a database whose files carry no
// stamp or an older one (Resolver::needsRescan), since resolving it would drop
// links. Independent of user_version, which initDb stamps on any open.
// Bumped when a parser change leaves earlier rows unfit for the resolver.
//   3 -- symbol/scope/binding/ref, subscripted-base refs, binding.is_exported
inline constexpr const char* kScanFormatKey = "scan_format";
inline constexpr int         kScanFormat    = 3;

// Records kScanFormat. No transaction of its own.
int stampScanFormat(sqlite3* db);
