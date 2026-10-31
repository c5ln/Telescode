#pragma once

#include <string>

struct sqlite3;

// ── Format version ──────────────────────────────────────────────────────────
// PRAGMA user_version of every database this Telescode writes, set in the same
// transaction that first fills it. A database of any other version -- older,
// newer, or not a Telescode database at all -- is never read or changed: `scan`
// replaces it with a new one, every other command refuses it with
// kWrongDbVersionMessage. There is no migration; an old database is rescanned.
// Bump whenever the schema or the parser's output changes what a database holds.
//   6 -- link is a view over ref / import_resolution; the parser writes every
//        module spec of every import into import_resolution
inline constexpr int kDbVersion = 6;

// Returned instead of an SQLite code for a database of another version.
inline constexpr int kWrongDbVersion = -2;

// Returned instead of an SQLite code for a file at the path that is not a
// Telescode database (no `file` and `link`), SQLite or not: no command reads
// it, and scan never deletes it.
inline constexpr int kNotADatabase = -3;

// What a command says, after the path, when it gets kNotADatabase.
inline constexpr const char* kNotADatabaseMessage = "is not a Telescode database";

// What algo / graph / sequence / analyze say, as a warning, about a database
// with no files in it: an empty result there is not an empty project.
inline constexpr const char* kNoFilesWarning =
    "the database has no files; run `Telescode scan <repo_path> <db_path>` first";

// What a command says when it gets kWrongDbVersion.
inline constexpr const char* kWrongDbVersionMessage =
    "this database was created by a different Telescode version; rescan it"
    " (Telescode scan <repo_path> <db_path>)";

// SQLITE_OK for a database of kDbVersion, or for nothing at all -- no file, a
// 0-byte file, an SQLite database with nothing in it (*empty is then set);
// kWrongDbVersion for a Telescode database of another version; kNotADatabase
// for anything else, a file that is not an SQLite database included; any
// other code if the file could not be looked at -- never read it then. Changes and creates no file, -wal and -shm included, so a
// refusal leaves everything at the path byte for byte as it was.
int checkDbVersion(const char* dbPath, bool* empty = nullptr);

// What to tell the user when opening dbPath failed with rc: "<path> is not a
// Telescode database", "<path>: <kWrongDbVersionMessage>", or SQLite's text.
std::string dbOpenError(const std::string& dbPath, int rc);

// Opens (or creates) the database at the given path and applies the
// connection pragmas (WAL, foreign keys). A file with nothing in it gets the
// current schema. kWrongDbVersion / kNotADatabase (checkDbVersion), with the
// file left exactly as it was, for anything else. On success the caller must sqlite3_close().
int initDb(const char* dbPath, sqlite3** db);

// For `scan`: as initDb, except that a Telescode database of another version
// is deleted (with its -wal / -shm files) and a new, empty one opened in its
// place -- any other non-empty file, SQLite or not, is left untouched and
// kNotADatabase returned -- and
// that an empty database does not get the schema here: *empty is set, and the
// scan's own transaction calls createSchema, so that the version is only there
// once everything the scan writes is.
int openDbForScan(const char* dbPath, sqlite3** db, bool* empty);

// The tables, the `link` view and user_version = kDbVersion, for an empty
// database. No transaction of its own.
int createSchema(sqlite3* db);
