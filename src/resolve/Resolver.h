#pragma once

#include "lang/LanguageRegistry.h"

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

struct sqlite3;

// ── Name resolver ───────────────────────────────────────────────────────────
// Turns the parser's per-file extraction (symbol/scope/binding/ref) into
// project-wide answers: what each module spec in import_resolution names,
// resolved_id/status/confidence/emits_link for every ref. The `link` view
// (db.cpp) is derived from those two: CALLS / INHERITS / DECORATES from the refs
// marked emits_link, IMPORTS from import_resolution. Nothing writes links.
//
// resolveAll answers every ref; resolveChanged, after an update, only the refs
// the update can have changed the answer of. Both give the same database: to
// know which refs those are, every answer records the facts it was decided on
// (ref_dep / fact in db.cpp):
//   B  -- the bindings of one name in a module scope (also when there are none)
//   S  -- the wildcard imports of a scope
//   Y  -- one symbol row: kind, name, file, parent (also when there is none)
//   C  -- the member of a class by one name (also when there is none)
//   K  -- the bases of a class: the answers of its INHERITS refs
//   M  -- what a module spec names from a file (a project file, or why not)
//   E  -- what a module-level name of a file comes to (re-exports and
//         wildcard imports followed): not a row but an answer many refs
//         share, standing for the facts it read (fact_edge). A wildcard
//         import of a package that wildcard-imports hundreds of modules is
//         one E fact per name asked, not hundreds of facts per ref.
// A ref's own file's facts are not recorded: an update of that file
// re-resolves every ref in it anyway. Answering the same ref from the same
// facts takes the same path and so consults the same facts, so a ref none of
// whose facts changed keeps its answer and its facts. A fact no ref reads any
// more, directly or through an E fact, goes, so that the tables stay what a
// scan of the same files stores.
//
// Language rules come only from the ILanguage registered for each file's
// `file.language`; a file whose language has no plugin has its refs set back
// to 'pending' and contributes no links.
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
        int imports = 0;                     // import_resolution rows answered
        std::map<std::string, int> status;   // ref.status -> refs decided
        std::map<std::string, int> links;    // link_type  -> links the refs now give
        long long ms = 0;                    // wall time of the whole pass
        int inheritsRounds = 0;              // rounds the INHERITS fixpoint took
        bool inheritsCapHit = false;         // stopped at the round cap, not at a fixpoint
        bool incremental = false;            // resolveChanged answered only some refs
    };

    // What an update is about to change, taken before it changes anything:
    // the files there were, and what the files it touches held that other
    // files' answers can rest on. Filled by beginChange, read by
    // resolveChanged; nothing else looks inside.
    struct Change {
        std::vector<std::pair<std::string, std::string>> files;   // file_id, language
        std::set<std::string> touched;              // files deleted, re-parsed or added
        std::map<std::string, std::string> facts;   // fact key -> value, of touched files
        std::map<std::string, std::string> bases;   // class -> its bases: of touched files,
                                                    // and of classes with a base in one
        std::vector<long long> released;            // fact ids the touched files' refs read
    };

    // One line on stderr if stats say the INHERITS fixpoint was cut off, so its
    // answers may still depend on ref order. `who` prefixes it ("scan").
    static void warnIfCapped(const Stats& stats, const char* who);

    // Resolve an open database in place. Caller is responsible for
    // BEGIN/COMMIT/ROLLBACK.
    static int resolveAll(sqlite3* db, Stats* stats = nullptr,
                          const LanguageRegistry& languages = LanguageRegistry::builtin());

    // An update in two calls, in the caller's transaction: beginChange before
    // the rows of `files` (every file the update deletes, re-parses or adds)
    // are deleted or replaced, resolveChanged once the new rows are in. The
    // database then holds what resolveAll would have made of it.
    static int beginChange(sqlite3* db, const std::vector<std::string>& files, Change& change);
    static int resolveChanged(sqlite3* db, const Change& change, Stats* stats = nullptr,
                              const LanguageRegistry& languages = LanguageRegistry::builtin());

    // The files other than fileId that bind a name to the module fileId by
    // `from pkg import mod` -- an import of pkg as far as import_resolution
    // goes, but of pkg/mod.py once the name is looked up. Appended to `files`.
    // Read-only.
    static int findSubmoduleImporters(sqlite3* db, const std::string& fileId,
                                      std::vector<std::string>& files,
                                      const LanguageRegistry& languages = LanguageRegistry::builtin());
};
