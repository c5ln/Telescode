#pragma once

struct sqlite3;

// Opens (or creates) telescode.db at the given path.
// Applies WAL pragmas and creates all tables if they do not exist.
// Returns SQLITE_OK on success; caller must call sqlite3_close() when done.
int initDb(const char* dbPath, sqlite3** db);
