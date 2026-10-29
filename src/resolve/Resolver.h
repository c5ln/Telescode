#pragma once

#include "lang/LanguageRegistry.h"

#include <map>
#include <string>

struct sqlite3;

// ── Name resolver ───────────────────────────────────────────────────────────
// Turns the parser's per-file extraction (symbol/scope/binding/ref) into
// project-wide answers: import_resolution for every module spec a binding names,
// resolved_id/status/confidence for every ref, and from those the CALLS /
// INHERITS / DECORATES rows of `link`. It is the only writer of those three
// link types; IMPORTS links are still the parser's.
//
// Works on the whole database every time: everything is loaded into memory
// once, resolved, and written back. Language rules come only from the
// ILanguage registered for each file's `file.language`; a file whose language
// has no plugin keeps its refs 'pending' and contributes no links.
//
// ref.status:
//   resolved   -- resolved_id is a project symbol, or a project file when the
//                 name is a module
//   builtin    -- the name is the language's builtin (ILanguage::isBuiltin)
//   external   -- the name comes from a module outside the project
//   unresolved -- anything else: a local with no symbol (no type inference),
//                 an attribute the target does not define, a broken import
// ref.confidence is 'heuristic' when the answer depended on a choice the code
// does not settle (several bindings of the name in one scope that lead to
// different places, or a method found on a base class rather than the class
// itself), 'exact' otherwise, and NULL for 'unresolved'.
//
// import_resolution.status: 'resolved' (resolved_file_id set), 'unresolved'
// for a spec that can only name a project module but names none
// (ILanguage::isProjectModule: a relative spec, or a missing module of a
// project package), and 'external' for any other spec with no project file.
class Resolver {
public:
    // ref.resolver for every ref this resolver decides. The answers come from
    // the scopes and bindings the tree-sitter parser extracted, nothing more.
    static constexpr const char* kName = "treesitter";

    struct Stats {
        int imports = 0;                     // import_resolution rows written
        std::map<std::string, int> status;   // ref.status -> refs decided
        std::map<std::string, int> links;    // link_type  -> links written
        long long ms = 0;                    // wall time of the whole pass
        int inheritsRounds = 0;              // rounds the INHERITS fixpoint took
        bool inheritsCapHit = false;         // stopped at the round cap, not at a fixpoint
    };

    // One line on stderr if stats say the INHERITS fixpoint was cut off, so its
    // answers may still depend on ref order. `who` prefixes it ("scan").
    static void warnIfCapped(const Stats& stats, const char* who);

    // Returned instead of an SQLite code when the database's files were written
    // by an older parser (see needsRescan): they lack rows the resolver works
    // from, so resolving would drop links. Only a full scan brings it up to date.
    static constexpr int kNeedsRescan = -2;

    // True if the database holds files but no current scan stamp (db.h). Works
    // on a database initDb has not migrated, too.
    static bool needsRescan(sqlite3* db);

    // Resolve an open database in place. Caller is responsible for
    // BEGIN/COMMIT/ROLLBACK. kNeedsRescan, and nothing changed, on an old database.
    static int resolveAll(sqlite3* db, Stats* stats = nullptr,
                          const LanguageRegistry& languages = LanguageRegistry::builtin());

    // resolveAll inside its own transaction, rolled back on failure.
    static int run(sqlite3* db, Stats* stats = nullptr,
                   const LanguageRegistry& languages = LanguageRegistry::builtin());
};
