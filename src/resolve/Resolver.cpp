#include "Resolver.h"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

// Re-export chains (`from .a import x` where a.py itself imports x) are followed
// for this many hops: a name imported from a.py resolves if a.py re-exports it
// from b.py, b.py from c.py, ... up to five such hops. The base-class search for
// a receiver's method has its own limit.
constexpr int kMaxDepth = 5;
constexpr int kMaxBaseDepth = 10;
// Rounds of the INHERITS pass before giving up on a fixpoint (see fixpoint()).
constexpr int kMaxInheritsRounds = 10;

const char* const kResolved   = "resolved";
const char* const kBuiltin    = "builtin";
const char* const kExternal   = "external";
const char* const kUnresolved = "unresolved";

// The status constant a stored status stands for; nullptr for 'pending'.
const char* statusOf(const std::string& s)
{
    if (s == kResolved) return kResolved;
    if (s == kBuiltin) return kBuiltin;
    if (s == kExternal) return kExternal;
    if (s == kUnresolved) return kUnresolved;
    return nullptr;
}

// ── What is in the database ─────────────────────────────────────────────────

struct Symbol {
    std::string file_id;
    std::string parent_id;
    std::string kind;
    std::string name;
    bool exported = true;
};

struct Scope {
    std::string file_id;
    std::string parent_id;
    std::string kind;
};

struct Binding {
    std::string local_name;
    std::string kind;
    std::string file_id;
    std::string target_id;
    std::string module_spec;
    std::string imported_name;
    int line = 0;
    bool exported = true;
};

struct Ref {
    std::int64_t id = 0;
    std::string file_id;
    std::string source_id;
    std::string scope_id;
    std::string kind;
    std::string ns;
    std::string name_path;
    int line = 0;
};

struct Import {
    std::string file_id;   // the project file, "" if none
    const char* status = kUnresolved;
};

// A class's bases, in source order: the resolved class id, or "" with the
// status of a base that is not a project class.
using Bases = std::vector<std::pair<std::string, const char*>>;

// Kinds whose members are reached as `Name.member`.
bool isTypeKind(const std::string& kind)
{
    return kind == "class" || kind == "interface" || kind == "struct" ||
           kind == "trait" || kind == "enum" || kind == "namespace";
}

bool isClassKind(const std::string& kind)
{
    return isTypeKind(kind) && kind != "namespace";
}

bool isCallableKind(const std::string& kind)
{
    return kind == "function" || kind == "method";
}

// Whether a ref of `refKind` that resolved to a symbol of `symKind` is a link:
// CALLS to a function or method, INHERITS to a class, DECORATES to any symbol
// (a variable included, on purpose).
bool linksTo(const std::string& refKind, const std::string& symKind)
{
    return refKind == "CALLS"    ? isCallableKind(symKind)
         : refKind == "INHERITS" ? isClassKind(symKind)
         : refKind == "DECORATES";
}

std::string key(const std::string& a, const std::string& b)
{
    std::string k;
    k.reserve(a.size() + b.size() + 1);
    k += a;
    k += '\n';
    k += b;
    return k;
}

std::vector<std::string> splitPath(const std::string& path)
{
    std::vector<std::string> out;
    std::size_t start = 0;
    while (true) {
        const std::size_t dot = path.find('.', start);
        out.push_back(path.substr(start, dot - start));
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    return out;
}

const char* columnText(sqlite3_stmt* stmt, int col)
{
    const unsigned char* t = sqlite3_column_text(stmt, col);
    return t ? reinterpret_cast<const char*>(t) : "";
}

// The file a symbol or scope id belongs to: ids are `<file_id>::<path>`, and a
// file scope's id is the file_id itself.
std::string fileOf(const std::string& id)
{
    return id.substr(0, id.find("::"));
}

struct Stmt {
    sqlite3_stmt* s = nullptr;
    Stmt() = default;
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;
    ~Stmt() { sqlite3_finalize(s); }
    int prepare(sqlite3* db, const char* sql) { return sqlite3_prepare_v2(db, sql, -1, &s, nullptr); }
};

int done(int rc) { return rc == SQLITE_DONE ? SQLITE_OK : rc; }

// SQLITE_STATIC: v must outlive the statement's next step.
void bindText(sqlite3_stmt* s, int i, const std::string& v)
{
    if (v.empty()) sqlite3_bind_null(s, i);
    else sqlite3_bind_text(s, i, v.c_str(), -1, SQLITE_STATIC);
}

// Runs stmt (already bound) to the end, calling row for each row.
template <typename F>
int each(sqlite3_stmt* stmt, F&& row)
{
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) row(stmt);
    sqlite3_reset(stmt);
    return done(rc);
}

// What a spec names from a file, by one language's index.
Import answer(const ILanguage* lang, const ProjectIndex* index,
              const std::string& spec, const std::string& from)
{
    Import imp;
    if (lang && index) {
        imp.file_id = lang->resolveModule(spec, from, *index);
        imp.status = !imp.file_id.empty()                      ? kResolved
                   : lang->isProjectModule(spec, from, *index) ? kUnresolved
                                                                : kExternal;
    }
    return imp;
}

// ── Facts ───────────────────────────────────────────────────────────────────
// Keys of the facts an answer rests on (Resolver.h), as stored in fact.key.
// One letter, then the parts, '\n'-separated. A fact's owner is the file it
// is read off, "" for K and M, which no single file decides.

std::string factB(const std::string& scope, const std::string& ns, const std::string& name)
{
    return "B\n" + scope + '\n' + ns + '\n' + name;
}
std::string factS(const std::string& scope) { return "S\n" + scope; }
std::string factY(const std::string& id) { return "Y\n" + id; }
std::string factC(const std::string& cls, const std::string& name)
{
    return "C\n" + cls + '\n' + name;
}
std::string factK(const std::string& cls) { return "K\n" + cls; }
std::string factM(const std::string& from, const std::string& spec)
{
    return "M\n" + from + '\n' + spec;
}
// Not read off any row: the answer of exportOf(file, ns, name), standing for
// every fact that answer read (fact_edge). Shared by all refs that ask it.
std::string factE(const std::string& file, const std::string& ns, const std::string& name)
{
    return "E\n" + file + '\n' + ns + '\n' + name;
}

// Fact values, to tell a change: field separator, record separator.
constexpr char kFs = '\x1f';
constexpr char kRs = '\x1e';

std::string basesValue(const Bases& bases)
{
    std::string v;
    for (const auto& [id, status] : bases) {
        v += id;
        v += kFs;
        v += status;
        v += kRs;
    }
    return v;
}

// The B / S / Y / C facts one file's rows hold, by key. Lines are left out:
// only their order within one name's bindings is read.
int loadFileFacts(sqlite3* db, const std::string& fileId, std::map<std::string, std::string>& facts)
{
    Stmt syms;
    int rc = syms.prepare(db,
        "SELECT symbol_id, parent_id, kind, name, is_exported FROM symbol"
        " WHERE file_id = ? ORDER BY rowid;");
    if (rc != SQLITE_OK) return rc;
    bindText(syms.s, 1, fileId);
    rc = each(syms.s, [&](sqlite3_stmt* s) {
        const std::string id = columnText(s, 0);
        const std::string parent = columnText(s, 1);
        const std::string name = columnText(s, 3);
        facts[factY(id)] = std::string(columnText(s, 2)) + kFs + fileId + kFs + name + kFs +
                           parent + kFs + (sqlite3_column_int(s, 4) ? '1' : '0');
        if (!parent.empty()) facts[factC(parent, name)] = id;
    });
    if (rc != SQLITE_OK) return rc;

    Stmt binds;
    rc = binds.prepare(db,
        "SELECT namespace, local_name, kind, target_id, module_spec, imported_name, is_exported"
        "  FROM binding WHERE file_id = ?1 AND scope_id = ?1 ORDER BY line;");
    if (rc != SQLITE_OK) return rc;
    bindText(binds.s, 1, fileId);
    return each(binds.s, [&](sqlite3_stmt* s) {
        const std::string kind = columnText(s, 2);
        if (kind == "import_all") {
            facts[factS(fileId)] += std::string(columnText(s, 4)) + kRs;
            return;
        }
        facts[factB(fileId, columnText(s, 0), columnText(s, 1))] +=
            kind + kFs + columnText(s, 3) + kFs + columnText(s, 4) + kFs + columnText(s, 5) +
            kFs + (sqlite3_column_int(s, 6) ? '1' : '0') + kRs;
    });
}

// INHERITS answers, as the class's bases: (source_id, resolved_id, status,
// kind of the resolved symbol). One file's, in source order.
const char* const kBasesSql =
    "SELECT r.source_id, r.resolved_id, r.status, s.kind"
    "  FROM ref r LEFT JOIN symbol s ON s.symbol_id = r.resolved_id"
    " WHERE r.file_id = ? AND r.kind = 'INHERITS' AND r.status <> 'pending'"
    " ORDER BY r.line, r.ref_id;";

// One row of kBasesSql as a base.
std::pair<std::string, const char*> baseOf(sqlite3_stmt* s)
{
    const char* status = statusOf(columnText(s, 2));
    std::string base;
    if (status == kResolved && isClassKind(columnText(s, 3))) base = columnText(s, 1);
    return {std::move(base), status};
}

// ── One answer ──────────────────────────────────────────────────────────────

// What a name, or a name followed by attribute accesses, denotes. A module is
// kept as its spec too: an attribute on it may be a submodule, and a module
// with no project file (external, or a directory without an init file) can
// still have project submodules.
struct Outcome {
    enum Kind { None, Symbol, Module };
    Kind kind = None;
    std::string id;           // symbol id, or a module's project file ("" if none)
    std::string spec;         // Module: the spec, as written in `from`
    std::string from;
    const char* status = nullptr;  // nullptr = not found here, look elsewhere
    bool heuristic = false;
    bool maybeExternal = false;    // not found, but an external wildcard import could supply it

    bool found() const { return status != nullptr; }
    bool resolved() const { return status == kResolved; }

    static Outcome symbol(std::string id)
    {
        Outcome o;
        o.kind = Symbol;
        o.id = std::move(id);
        o.status = kResolved;
        return o;
    }
    static Outcome failed(const char* status, bool heuristic = false)
    {
        Outcome o;
        o.status = status;
        o.heuristic = heuristic;
        return o;
    }
};

// ── The resolver proper ─────────────────────────────────────────────────────
// Holds the rows answers are read off: all of them (loadAll, for resolveAll),
// or each file's the first time an answer looks into it (resolveChanged).

class Engine {
public:
    Engine(sqlite3* db, const LanguageRegistry& languages) : db_(db), languages_(languages) {}

    int loadFiles();
    // All rows. `fresh`: the refs are these, not the table's (resolveScan).
    int loadAll(const std::vector<ParseResult>* fresh = nullptr);
    // Files whose ref rows the update replaced: their stored INHERITS answers
    // are not bases yet (they are 'pending').
    void setTouched(const std::set<std::string>& touched) { touched_ = touched; }

    // Refs to answer: adds the rows `stmt` (bound, kRefColumns) returns that
    // are not there yet.
    int addRefs(sqlite3_stmt* stmt);
    std::size_t refCount() const { return refs_.size(); }
    const Ref& ref(std::size_t i) const { return refs_[i]; }

    void resolveImports();
    // INHERITS refs to a fixpoint, then everything else.
    void resolve();
    bool fixpoint();
    void resolveRest();

    int writeAll(Resolver::Stats* stats);
    int writeChanged(const std::vector<std::pair<std::string, std::string>>& imports,
                     Resolver::Stats* stats);
    // Facts whose ref_dep rows the update deleted before this run: candidates
    // for collectGarbage, with those writeDeps(false) releases.
    void release(const std::vector<long long>& facts) { released_.insert(facts.begin(), facts.end()); }

    const Import& module(const std::string& spec, const std::string& from);
    const ILanguage* languageOf(const std::string& fileId) const;
    const std::unordered_map<std::string, const ILanguage*>& files() const { return fileLanguage_; }

    // INHERITS classes of this run, their bases now, and before (as stored).
    const std::unordered_set<std::string>& affectedClasses() const { return affected_; }
    std::string basesNow(const std::string& cls) const;
    std::string basesStored(const std::string& cls);

    // What binding b makes its name denote, from b's own file.
    Outcome bindingOutcome(const Binding& b, const std::string& ns) { return fromBinding(b, ns, 0); }

    int error() const { return rc_; }
    int inheritsRounds() const { return inheritsRounds_; }
    bool inheritsCapHit() const { return inheritsCapHit_; }

    // kRefColumns: the columns addRefs reads, from `ref`.
    static const char* const kRefColumns;

private:
    // Name lookup
    void resolveOne(std::size_t i);
    Outcome resolveRef(const Ref& ref);
    Outcome lookup(const Ref& ref, const std::vector<std::string>& segs);
    Outcome receiverMember(const Ref& ref, const std::vector<std::string>& segs);
    Outcome member(const std::string& classId, const std::string& name, int depth,
                   std::unordered_set<std::string>& seen, bool& externalBase);
    Outcome choose(std::vector<Outcome>& candidates);
    int rank(const Outcome& o);
    Outcome fromBinding(const Binding& b, const std::string& ns, int depth);
    Outcome walk(Outcome o, const std::vector<std::string>& segs, std::size_t from,
                 const std::string& ns, int depth);
    Outcome exportOf(const std::string& fileId, const std::string& name,
                     const std::string& ns, int depth);
    Outcome starLookup(const std::string& scopeId, const std::string& name,
                       const std::string& ns, int depth);
    bool isExported(const std::string& fileId, const std::string& name,
                    const std::string& ns);

    // Modules
    Outcome moduleOutcome(const std::string& spec, const std::string& from);

    const std::vector<std::string>& scopeChain(const std::string& scopeId,
                                               const std::string& fileId);

    // The rows, through which every fact an answer reads is recorded.
    void ensure(const std::string& fileId);
    const Symbol* symbol(const std::string& id);
    const std::string* child(const std::string& classId, const std::string& name);
    const std::vector<Binding>* bound(const std::string& scopeId, const std::string& ns,
                                      const std::string& name);
    const std::vector<Binding>* stars(const std::string& scopeId);
    const Bases* basesOf(const std::string& classId);

    // Facts: interned keys, and the frames answers collect them in.
    int intern(std::string key, std::string owner);
    // Whether a fact of `owner` read now is worth recording: not outside any
    // answer, nor one of the ref's own file read by the ref itself rather
    // than by an E answer other refs may share (writeDeps drops those).
    bool wanted(const std::string& owner) const
    {
        return !frames_.empty() && (frames_.size() > 1 || owner.empty() || owner != refFile_);
    }
    void dep(std::string key, const std::string& owner)
    {
        frames_.back().push_back(intern(std::move(key), owner));
    }
    std::vector<int> popFrame();
    int writeDeps(bool all);
    int collectGarbage();
    std::int64_t storedFact(int fact);

    sqlite3* db_;
    const LanguageRegistry& languages_;
    const ILanguage* lang_ = nullptr;   // the language of the ref being resolved
    std::string refFile_;               // and its file
    int rc_ = SQLITE_OK;                // first error of a lazy load

    std::unordered_map<std::string, const ILanguage*> fileLanguage_;
    std::unordered_map<const ILanguage*, ProjectIndex> index_;

    bool all_ = false;                           // loadAll: every row is in
    std::unordered_set<std::string> loaded_;     // else the files loaded so far
    std::set<std::string> touched_;
    Stmt fileSymbols_, fileScopes_, fileBindings_, fileBases_;

    std::unordered_map<std::string, Symbol> symbols_;
    std::unordered_map<std::string, std::string> children_;   // key(parent, name) -> symbol
    std::unordered_map<std::string, Scope> scopes_;
    // scope -> key(namespace, name) -> bindings in source order
    std::unordered_map<std::string,
                       std::unordered_map<std::string, std::vector<Binding>>> bindings_;
    std::unordered_map<std::string, std::vector<Binding>> stars_;   // scope -> import_all
    std::vector<Ref> refs_;
    std::unordered_set<std::int64_t> refIds_;
    // resolveScan: the parser's ref behind each of refs_, which writeAll
    // inserts rather than updates.
    std::vector<const RefEntity*> fresh_;

    std::unordered_map<std::string, Import> imports_;      // factM(from, spec)
    std::vector<std::pair<std::string, std::string>> importSpecs_;   // import_resolution keys (file, spec)
    std::unordered_map<std::string, std::vector<std::string>> chains_;
    std::unordered_set<std::string> exporting_;            // re-export cycle guard
    // exportOf answers, keyed like exporting_, with the number of hops the
    // answer took (height) and its E fact. Only complete answers are kept --
    // one cut short by the cycle guard or kMaxDepth (truncated_ moved while it
    // was computed) depends on where it was asked from -- and one is reused
    // only where its hops still fit under kMaxDepth, so the result never
    // depends on the order refs are resolved in.
    struct Exported {
        Outcome out;
        int height = 0;
        int fact = 0;
    };
    std::unordered_map<std::string, Exported> exported_;
    long truncated_ = 0;
    int reached_ = 0;   // deepest exportOf depth reached so far in the current subtree

    // class -> its bases. The classes of this run's INHERITS refs get theirs
    // from fixpoint(); any other class's are its stored answers.
    std::unordered_map<std::string, Bases> bases_;
    std::unordered_map<std::string, Bases> storedBases_;
    std::unordered_set<std::string> affected_;

    std::vector<Outcome> outcomes_;            // parallel to refs_
    std::vector<std::vector<int>> deps_;       // parallel to refs_: facts read
    std::vector<char> answered_;               // parallel to refs_
    int inheritsRounds_ = 0;
    bool inheritsCapHit_ = false;

    std::unordered_map<std::string, int> factIds_;
    std::vector<const std::string*> factKeys_;
    std::vector<std::string> factOwners_;
    std::vector<std::int64_t> storedFacts_;    // fact -> fact.fact_id, 0 if not looked up
    Stmt factInsert_, factSelect_;
    std::vector<std::vector<int>> frames_;
    // E fact -> the facts its answer read, for every answer kept this run.
    std::unordered_map<int, std::vector<int>> edges_;
    // fact.fact_id of every stored fact that lost a ref_dep row or an
    // incoming fact_edge row this update, or that writeDeps(false) gave edges:
    // the only facts that can have become unreachable.
    std::unordered_set<std::int64_t> released_;
};

const char* const Engine::kRefColumns =
    "ref_id, file_id, source_id, scope_id, kind, namespace, name_path, line";

const ILanguage* Engine::languageOf(const std::string& fileId) const
{
    auto it = fileLanguage_.find(fileId);
    return it == fileLanguage_.end() ? nullptr : it->second;
}

// ── Loading ─────────────────────────────────────────────────────────────────
// The same columns whether one file's rows are loaded or all of them.

namespace rows {

void symbol(std::unordered_map<std::string, Symbol>& symbols,
            std::unordered_map<std::string, std::string>& children, sqlite3_stmt* s)
{
    Symbol sym;
    sym.file_id   = columnText(s, 1);
    sym.parent_id = columnText(s, 2);
    sym.kind      = columnText(s, 3);
    sym.name      = columnText(s, 4);
    sym.exported  = sqlite3_column_int(s, 5) != 0;
    const std::string id = columnText(s, 0);
    if (!sym.parent_id.empty()) children[key(sym.parent_id, sym.name)] = id;
    symbols.emplace(id, std::move(sym));
}

void scope(std::unordered_map<std::string, Scope>& scopes, sqlite3_stmt* s)
{
    Scope sc;
    sc.file_id   = columnText(s, 1);
    sc.parent_id = columnText(s, 2);
    sc.kind      = columnText(s, 3);
    scopes.emplace(columnText(s, 0), std::move(sc));
}

void binding(std::unordered_map<std::string,
                                std::unordered_map<std::string, std::vector<Binding>>>& bindings,
             std::unordered_map<std::string, std::vector<Binding>>& stars, sqlite3_stmt* s)
{
    Binding b;
    b.local_name    = columnText(s, 1);
    b.kind          = columnText(s, 3);
    b.file_id       = columnText(s, 4);
    b.target_id     = columnText(s, 5);
    b.module_spec   = columnText(s, 6);
    b.imported_name = columnText(s, 7);
    b.line          = sqlite3_column_int(s, 8);
    b.exported      = sqlite3_column_int(s, 9) != 0;
    const std::string scope = columnText(s, 0);
    // Each name's bindings in source order. The rows come in key order
    // (kBindingOrder), which is line order for one name; the wildcard
    // imports of a scope, named '*' || line, come in text order instead.
    if (b.kind == "import_all") {
        auto& all = stars[scope];
        auto at = std::upper_bound(all.begin(), all.end(), b.line,
                                   [](int line, const Binding& x) { return line < x.line; });
        all.insert(at, std::move(b));
    } else {
        bindings[scope][key(columnText(s, 2), b.local_name)].push_back(std::move(b));
    }
}

void ref(std::vector<Ref>& refs, sqlite3_stmt* s)
{
    Ref r;
    r.id        = sqlite3_column_int64(s, 0);
    r.file_id   = columnText(s, 1);
    r.source_id = columnText(s, 2);
    r.scope_id  = columnText(s, 3);
    r.kind      = columnText(s, 4);
    r.ns        = columnText(s, 5);
    r.name_path = columnText(s, 6);
    r.line      = sqlite3_column_int(s, 7);
    refs.push_back(std::move(r));
}

}  // namespace rows

const char* const kSymbolSql =
    "SELECT symbol_id, file_id, parent_id, kind, name, is_exported FROM symbol";
const char* const kScopeSql =
    "SELECT scope_id, file_id, parent_scope_id, kind FROM scope";
const char* const kBindingSql =
    "SELECT scope_id, local_name, namespace, kind, file_id, target_id, module_spec,"
    "       imported_name, line, is_exported FROM binding";
// binding's key, so that the rows are read straight off the table.
const char* const kBindingOrder = " ORDER BY file_id, scope_id, namespace, local_name, line;";

int Engine::loadFiles()
{
    Stmt stmt;
    int rc = stmt.prepare(db_, "SELECT file_id, language FROM file;");
    if (rc != SQLITE_OK) return rc;
    rc = each(stmt.s, [&](sqlite3_stmt* s) {
        fileLanguage_[columnText(s, 0)] = languages_.find(columnText(s, 1));
    });
    if (rc != SQLITE_OK) return rc;

    // One index per language, over that language's files only.
    std::unordered_map<const ILanguage*, std::unordered_set<std::string>> files;
    for (const auto& [fileId, lang] : fileLanguage_)
        if (lang) files[lang].insert(fileId);
    for (auto& [lang, ids] : files) index_.emplace(lang, lang->buildIndex(std::move(ids)));

    const std::string where = " WHERE file_id = ?;";
    const std::string bindingWhere = std::string(" WHERE file_id = ?") + kBindingOrder;
    if ((rc = fileSymbols_.prepare(db_, (kSymbolSql + where).c_str())) != SQLITE_OK) return rc;
    if ((rc = fileScopes_.prepare(db_, (kScopeSql + where).c_str())) != SQLITE_OK) return rc;
    if ((rc = fileBindings_.prepare(db_, (kBindingSql + bindingWhere).c_str())) != SQLITE_OK) return rc;
    return fileBases_.prepare(db_, kBasesSql);
}

int Engine::loadAll(const std::vector<ParseResult>* fresh)
{
    int rc = loadFiles();
    if (rc != SQLITE_OK) return rc;
    all_ = true;

    const std::string symbolSql = std::string(kSymbolSql) + ';';
    const std::string scopeSql = std::string(kScopeSql) + ';';
    const std::string bindingSql = std::string(kBindingSql) + kBindingOrder;
    const std::string refSql = std::string("SELECT ") + kRefColumns + " FROM ref ORDER BY ref_id;";
    struct Query {
        const char* sql;
        void (*row)(Engine&, sqlite3_stmt*);
    };
    const Query queries[] = {
        { symbolSql.c_str(),
          [](Engine& e, sqlite3_stmt* s) { rows::symbol(e.symbols_, e.children_, s); } },
        { scopeSql.c_str(), [](Engine& e, sqlite3_stmt* s) { rows::scope(e.scopes_, s); } },
        { bindingSql.c_str(),
          [](Engine& e, sqlite3_stmt* s) { rows::binding(e.bindings_, e.stars_, s); } },
        { "SELECT file_id, module_spec FROM import_resolution;",
          [](Engine& e, sqlite3_stmt* s) {
              e.importSpecs_.emplace_back(columnText(s, 0), columnText(s, 1));
          } },
        { refSql.c_str(), [](Engine& e, sqlite3_stmt* s) { rows::ref(e.refs_, s); } },
    };
    for (const Query& q : queries) {
        if (fresh && q.sql == refSql.c_str()) continue;
        Stmt stmt;
        rc = stmt.prepare(db_, q.sql);
        if (rc != SQLITE_OK) return rc;
        rc = each(stmt.s, [&](sqlite3_stmt* s) { q.row(*this, s); });
        if (rc != SQLITE_OK) return rc;
    }
    if (fresh) {
        std::unordered_set<std::string> seen;
        for (const ParseResult& pr : *fresh) {
            for (const RefEntity& e : pr.refs) {
                if (!seen.insert(refKey(e)).second) continue;
                Ref r;
                r.id        = static_cast<std::int64_t>(refs_.size()) + 1;
                r.file_id   = e.file_id;
                r.source_id = e.source_id;
                r.scope_id  = e.scope_id;
                r.kind      = e.kind;
                r.ns        = e.ns;
                r.name_path = e.name_path;
                r.line      = e.line;
                refs_.push_back(std::move(r));
                fresh_.push_back(&e);
            }
        }
    }
    for (const Ref& r : refs_) refIds_.insert(r.id);
    return SQLITE_OK;
}

void Engine::ensure(const std::string& fileId)
{
    if (all_ || !loaded_.insert(fileId).second || !fileLanguage_.count(fileId)) return;
    auto run = [&](sqlite3_stmt* stmt, auto&& row) {
        sqlite3_bind_text(stmt, 1, fileId.c_str(), -1, SQLITE_STATIC);
        const int rc = each(stmt, row);
        if (rc != SQLITE_OK && rc_ == SQLITE_OK) rc_ = rc;
    };
    run(fileSymbols_.s, [&](sqlite3_stmt* s) { rows::symbol(symbols_, children_, s); });
    run(fileScopes_.s, [&](sqlite3_stmt* s) { rows::scope(scopes_, s); });
    run(fileBindings_.s, [&](sqlite3_stmt* s) { rows::binding(bindings_, stars_, s); });
    if (touched_.count(fileId)) return;
    run(fileBases_.s, [&](sqlite3_stmt* s) {
        const std::string cls = columnText(s, 0);
        auto base = baseOf(s);
        if (!affected_.count(cls)) bases_[cls].push_back(base);
        storedBases_[cls].push_back(std::move(base));
    });
}

int Engine::addRefs(sqlite3_stmt* stmt)
{
    std::vector<Ref> found;
    const int rc = each(stmt, [&](sqlite3_stmt* s) { rows::ref(found, s); });
    for (Ref& r : found)
        if (refIds_.insert(r.id).second) refs_.push_back(std::move(r));
    return rc;
}

// ── Facts ───────────────────────────────────────────────────────────────────

int Engine::intern(std::string key, std::string owner)
{
    auto [it, fresh] = factIds_.try_emplace(std::move(key), static_cast<int>(factKeys_.size()));
    if (fresh) {
        factKeys_.push_back(&it->first);
        factOwners_.push_back(std::move(owner));
    }
    return it->second;
}

std::vector<int> Engine::popFrame()
{
    std::vector<int> deps = std::move(frames_.back());
    frames_.pop_back();
    std::sort(deps.begin(), deps.end());
    deps.erase(std::unique(deps.begin(), deps.end()), deps.end());
    return deps;
}

const Symbol* Engine::symbol(const std::string& id)
{
    const std::string file = fileOf(id);
    ensure(file);
    if (wanted(file)) dep(factY(id), file);
    auto it = symbols_.find(id);
    return it == symbols_.end() ? nullptr : &it->second;
}

const std::string* Engine::child(const std::string& classId, const std::string& name)
{
    const std::string file = fileOf(classId);
    ensure(file);
    if (wanted(file)) dep(factC(classId, name), file);
    auto it = children_.find(key(classId, name));
    return it == children_.end() ? nullptr : &it->second;
}

const std::vector<Binding>* Engine::bound(const std::string& scopeId, const std::string& ns,
                                          const std::string& name)
{
    const std::string file = fileOf(scopeId);
    ensure(file);
    if (wanted(file)) dep(factB(scopeId, ns, name), file);
    auto scope = bindings_.find(scopeId);
    if (scope == bindings_.end()) return nullptr;
    auto it = scope->second.find(key(ns, name));
    return it == scope->second.end() ? nullptr : &it->second;
}

const std::vector<Binding>* Engine::stars(const std::string& scopeId)
{
    const std::string file = fileOf(scopeId);
    ensure(file);
    if (wanted(file)) dep(factS(scopeId), file);
    auto it = stars_.find(scopeId);
    return it == stars_.end() ? nullptr : &it->second;
}

const Bases* Engine::basesOf(const std::string& classId)
{
    ensure(fileOf(classId));
    if (wanted("")) dep(factK(classId), "");
    auto it = bases_.find(classId);
    return it == bases_.end() ? nullptr : &it->second;
}

// ── Modules ─────────────────────────────────────────────────────────────────

const Import& Engine::module(const std::string& spec, const std::string& from)
{
    std::string k = factM(from, spec);
    if (wanted("")) dep(k, "");
    auto it = imports_.find(k);
    if (it != imports_.end()) return it->second;

    const ILanguage* lang = languageOf(from);
    auto index = lang ? index_.find(lang) : index_.end();
    Import imp = answer(lang, index == index_.end() ? nullptr : &index->second, spec, from);
    return imports_.emplace(std::move(k), std::move(imp)).first->second;
}

Outcome Engine::moduleOutcome(const std::string& spec, const std::string& from)
{
    const Import& imp = module(spec, from);
    Outcome o;
    o.kind   = Outcome::Module;
    o.id     = imp.file_id;
    o.spec   = spec;
    o.from   = from;
    o.status = imp.status;
    return o;
}

// ── Name lookup ─────────────────────────────────────────────────────────────

// Only ever the ref's own file's scopes: no fact is recorded for them.
const std::vector<std::string>& Engine::scopeChain(const std::string& scopeId,
                                                   const std::string& fileId)
{
    auto it = chains_.find(scopeId);
    if (it != chains_.end()) return it->second;

    std::vector<LexicalScope> lexical;
    std::string cur = scopeId;
    while (!cur.empty() && lexical.size() < 256) {
        auto sc = scopes_.find(cur);
        if (sc == scopes_.end()) {
            // No row (the parser keeps one scope per id): climb straight to
            // the file scope.
            lexical.push_back({cur, ""});
            cur = (cur == fileId) ? std::string() : fileId;
            continue;
        }
        lexical.push_back({cur, sc->second.kind});
        cur = sc->second.parent_id;
    }

    std::vector<std::string> chain{scopeId};
    for (std::string& s : lang_->parentScopes(lexical)) chain.push_back(std::move(s));
    return chains_.emplace(scopeId, std::move(chain)).first->second;
}

// A project definition (class, function, module) beats an outside origin,
// which beats a project variable or field, which beats not knowing at all. A
// variable bound next to an import is almost always the fallback for when the
// import fails (`except ImportError: X = object`), so the import is preferred.
int Engine::rank(const Outcome& o)
{
    if (o.resolved()) {
        if (o.kind != Outcome::Symbol) return 4;
        const Symbol* sym = symbol(o.id);
        if (sym && (isCallableKind(sym->kind) || isTypeKind(sym->kind))) return 4;
        return 2;
    }
    if (o.status == kBuiltin || o.status == kExternal) return 3;
    return 1;
}

// Several bindings of one name in one scope: take the best-ranked, the last in
// source order among equals. Only a choice between different answers is
// heuristic -- `import a.b` and `import a.c` both bind `a` to the same thing.
Outcome Engine::choose(std::vector<Outcome>& candidates)
{
    std::size_t best = 0;
    int bestRank = -1;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const int r = rank(candidates[i]);
        if (r >= bestRank) { best = i; bestRank = r; }
    }
    Outcome out = std::move(candidates[best]);
    for (const Outcome& c : candidates) {
        if (&c == &candidates[best]) continue;
        if (c.status != out.status || c.id != out.id) { out.heuristic = true; break; }
    }
    return out;
}

// What one binding makes its name denote. `depth` is the number of re-export
// hops taken to reach the binding's file.
Outcome Engine::fromBinding(const Binding& b, const std::string& ns, int depth)
{
    if (b.kind == "def") {
        // A local with no symbol row: a parameter or an assignment in a body.
        // It shadows every outer binding, and its value is unknown.
        if (b.target_id.empty()) return Outcome::failed(kUnresolved);
        return Outcome::symbol(b.target_id);
    }
    // Module specs are the binding's file's language, whoever asks.
    const ILanguage* lang = languageOf(b.file_id);
    if (!lang) return Outcome::failed(kUnresolved);
    if (b.kind == "import_module")
        return moduleOutcome(lang->boundModuleSpec(b.local_name, b.module_spec), b.file_id);

    // import_symbol / reexport: the name as the module binds it, else the
    // module's submodule of that name.
    const Import imp = module(b.module_spec, b.file_id);
    Outcome inModule;
    if (!imp.file_id.empty()) {
        inModule = exportOf(imp.file_id, b.imported_name, ns, depth);
        if (inModule.resolved()) return inModule;
    }
    Outcome sub = moduleOutcome(lang->submoduleSpec(b.module_spec, b.imported_name), b.file_id);
    if (sub.resolved()) return sub;
    if (imp.file_id.empty()) return Outcome::failed(imp.status);
    if (inModule.found()) return inModule;
    return Outcome::failed(inModule.maybeExternal ? kExternal : kUnresolved);
}

// `name` as a module-level name of fileId: its own bindings, then its wildcard
// imports. `depth` is the number of re-export hops already taken to get here:
// 0 for the module an import names directly. Each binding or wildcard import of
// fileId that passes the name on is one more hop, up to kMaxDepth.
Outcome Engine::exportOf(const std::string& fileId, const std::string& name,
                         const std::string& ns, int depth)
{
    if (depth > kMaxDepth) {
        ++truncated_;
        return Outcome::failed(kUnresolved);
    }
    const std::string guard = factB(fileId, ns, name);
    auto memo = exported_.find(guard);
    if (memo != exported_.end() && depth + memo->second.height <= kMaxDepth) {
        reached_ = std::max(reached_, depth + memo->second.height);
        if (!frames_.empty()) frames_.back().push_back(memo->second.fact);
        return memo->second.out;
    }
    if (!exporting_.insert(guard).second) {
        ++truncated_;
        return Outcome::failed(kUnresolved);
    }
    const long truncatedBefore = truncated_;
    const int reachedBefore = reached_;
    reached_ = depth;
    frames_.emplace_back();

    Outcome out;
    if (const std::vector<Binding>* found = bound(fileId, ns, name)) {
        std::vector<Outcome> candidates;
        for (const Binding& b : *found) candidates.push_back(fromBinding(b, ns, depth + 1));
        out = choose(candidates);
    } else {
        out = starLookup(fileId, name, ns, depth + 1);
    }
    exporting_.erase(guard);
    const int height = reached_ - depth;
    reached_ = std::max(reachedBefore, reached_);
    // A complete answer is an E fact of its own, which the asker reads; one
    // cut short passes what it read straight on.
    std::vector<int> deps = popFrame();
    if (truncated_ == truncatedBefore) {
        const int fact = intern(factE(fileId, ns, name), "");
        edges_[fact] = std::move(deps);
        exported_.insert_or_assign(guard, Exported{out, height, fact});
        if (!frames_.empty()) frames_.back().push_back(fact);
    } else if (!frames_.empty()) {
        frames_.back().insert(frames_.back().end(), deps.begin(), deps.end());
    }
    return out;
}

// `name` through the wildcard imports of one scope, the last import first (a
// later one overwrites an earlier one). `depth` is as for exportOf, for the
// modules imported. A name the module binds but does not export is not
// imported by a wildcard.
Outcome Engine::starLookup(const std::string& scopeId, const std::string& name,
                           const std::string& ns, int depth)
{
    Outcome none;
    const std::vector<Binding>* all = stars(scopeId);
    if (!all) return none;
    for (auto b = all->rbegin(); b != all->rend(); ++b) {
        const Import imp = module(b->module_spec, b->file_id);
        if (imp.file_id.empty()) {
            if (imp.status == kExternal) none.maybeExternal = true;
            continue;
        }
        if (!isExported(imp.file_id, name, ns)) continue;
        Outcome o = exportOf(imp.file_id, name, ns, depth);
        if (!o.found()) {
            none.maybeExternal |= o.maybeExternal;
            continue;
        }
        return o;
    }
    return none;
}

// False if fileId binds `name` at module level but exports none of those
// bindings. A name it does not bind itself (one its own wildcard imports
// supply) counts as exported.
bool Engine::isExported(const std::string& fileId, const std::string& name,
                        const std::string& ns)
{
    const std::vector<Binding>* found = bound(fileId, ns, name);
    if (!found) return true;
    for (const Binding& b : *found)
        if (b.exported) return true;
    return false;
}

// Follow segs[from..] as attribute accesses on o.
Outcome Engine::walk(Outcome o, const std::vector<std::string>& segs, std::size_t from,
                     const std::string& ns, int depth)
{
    for (std::size_t i = from; i < segs.size(); ++i) {
        const std::string& seg = segs[i];
        const bool heuristic = o.heuristic;

        if (o.kind == Outcome::Module) {
            Outcome inModule;
            if (!o.id.empty()) {
                inModule = exportOf(o.id, seg, ns, depth);
                if (inModule.resolved()) {
                    o = std::move(inModule);
                    o.heuristic |= heuristic;
                    continue;
                }
            }
            const ILanguage* lang = languageOf(o.from);
            if (!lang) return Outcome::failed(kUnresolved, heuristic);
            Outcome sub = moduleOutcome(lang->submoduleSpec(o.spec, seg), o.from);
            if (sub.resolved() || o.id.empty()) {
                // A module with no project file may still have project
                // submodules: keep walking specs until one resolves.
                o = std::move(sub);
                o.heuristic |= heuristic;
                continue;
            }
            if (inModule.found()) return inModule;
            return Outcome::failed(inModule.maybeExternal ? kExternal : kUnresolved, heuristic);
        }

        if (!o.resolved()) return o;   // nothing more is known about an outside name

        const Symbol* sym = symbol(o.id);
        if (!sym || !isTypeKind(sym->kind))
            return Outcome::failed(kUnresolved, heuristic);   // no type inference
        // `Class.attr`: the class's own member, else one it inherits.
        std::unordered_set<std::string> seen;
        bool externalBase = false;
        Outcome m = member(o.id, seg, 0, seen, externalBase);
        if (!m.found()) return Outcome::failed(externalBase ? kExternal : kUnresolved, heuristic);
        m.heuristic |= heuristic;
        o = std::move(m);
    }
    return o;
}

// The head of the path through the scope ladder, then the rest as attributes.
Outcome Engine::lookup(const Ref& ref, const std::vector<std::string>& segs)
{
    const std::string& head = segs[0];
    bool maybeExternal = false;

    for (const std::string& scopeId : scopeChain(ref.scope_id, ref.file_id)) {
        if (scopeId == kBuiltinScope) {
            if (lang_->isBuiltin(head)) return Outcome::failed(kBuiltin);
            break;
        }
        if (const std::vector<Binding>* found = bound(scopeId, ref.ns, head)) {
            std::vector<Outcome> candidates;
            for (const Binding& b : *found) {
                Outcome o = walk(fromBinding(b, ref.ns, 0), segs, 1, ref.ns, 0);
                // A class cannot inherit from, or decorate, itself: in
                // `from m import A` + `class A(A)` the base is the import.
                if (ref.kind != "CALLS" && o.kind == Outcome::Symbol && o.id == ref.source_id)
                    continue;
                candidates.push_back(std::move(o));
            }
            if (!candidates.empty()) return choose(candidates);
        }
        Outcome star = starLookup(scopeId, head, ref.ns, 0);
        if (star.found()) return walk(std::move(star), segs, 1, ref.ns, 0);
        maybeExternal |= star.maybeExternal;
    }
    return Outcome::failed(maybeExternal ? kExternal : kUnresolved);
}

// `name` on classId or, failing that, on its resolved bases, depth-first and
// left to right. externalBase is set if some base on the way is not a project
// class but comes from outside, which may well define the member.
Outcome Engine::member(const std::string& classId, const std::string& name, int depth,
                       std::unordered_set<std::string>& seen, bool& externalBase)
{
    if (const std::string* child = this->child(classId, name)) return Outcome::symbol(*child);
    if (depth >= kMaxBaseDepth || !seen.insert(classId).second) return {};

    const Bases* bases = basesOf(classId);
    if (!bases) return {};
    for (const auto& [baseId, status] : *bases) {
        if (baseId.empty()) {
            if (status == kExternal) externalBase = true;
            continue;
        }
        Outcome o = member(baseId, name, depth + 1, seen, externalBase);
        if (o.found()) {
            o.heuristic = true;
            return o;
        }
    }
    return {};
}

// `self.x...`: x on the class of the method whose parameter `self` is -- the
// method the ref is in, or the one enclosing the closure it is in. A receiver
// name bound anywhere else (a plain function's parameter) gets no special
// treatment.
Outcome Engine::receiverMember(const Ref& ref, const std::vector<std::string>& segs)
{
    std::string classId;
    for (const std::string& scopeId : scopeChain(ref.scope_id, ref.file_id)) {
        if (!bound(scopeId, ref.ns, segs[0])) continue;
        // A function scope's id is its symbol's.
        const Symbol* sym = symbol(scopeId);
        if (sym && sym->kind == "method") classId = sym->parent_id;
        break;
    }
    if (classId.empty()) return {};
    const Symbol* cls = symbol(classId);
    if (!cls || !isTypeKind(cls->kind)) return {};

    std::unordered_set<std::string> seen;
    bool externalBase = false;
    Outcome o = member(classId, segs[1], 0, seen, externalBase);
    if (!o.found()) return Outcome::failed(externalBase ? kExternal : kUnresolved);
    return walk(std::move(o), segs, 2, ref.ns, 0);
}

Outcome Engine::resolveRef(const Ref& ref)
{
    const std::vector<std::string> segs = splitPath(ref.name_path);
    Outcome o;
    if (segs.size() > 1 && lang_->isReceiver(segs[0])) o = receiverMember(ref, segs);
    if (!o.found()) o = lookup(ref, segs);

    // Calling a class runs its constructor: the class's own (exact), else the
    // nearest one it inherits, searched like any member (heuristic). With none
    // in the project the class itself is the answer, but no CALLS link -- unless
    // an outside base may supply it, which makes the call external.
    if (ref.kind == "CALLS" && o.resolved() && o.kind == Outcome::Symbol) {
        const Symbol* sym = symbol(o.id);
        const ILanguage* classLang =
            sym && isClassKind(sym->kind) ? languageOf(sym->file_id) : nullptr;
        if (classLang) {
            std::unordered_set<std::string> seen;
            bool externalBase = false;
            Outcome ctor = member(o.id, classLang->constructorName(sym->name), 0, seen,
                                  externalBase);
            if (ctor.found()) {
                ctor.heuristic |= o.heuristic;
                o = std::move(ctor);
            } else if (externalBase) {
                o = Outcome::failed(kExternal, o.heuristic);
            }
        }
    }
    return o;
}

// One ref's answer, and the facts it read. Which symbol a resolved ref names
// is read too: whether it is a link (and, for INHERITS, a base) depends on it.
void Engine::resolveOne(std::size_t i)
{
    const Ref& ref = refs_[i];
    answered_[i] = 1;
    lang_ = languageOf(ref.file_id);
    refFile_ = ref.file_id;
    if (!lang_) {
        // Stays 'pending'.
        outcomes_[i] = Outcome();
        deps_[i].clear();
        return;
    }
    ensure(ref.file_id);
    frames_.emplace_back();
    Outcome o = resolveRef(ref);
    if (o.resolved() && o.kind == Outcome::Symbol) symbol(o.id);
    deps_[i] = popFrame();
    outcomes_[i] = std::move(o);
}

void Engine::resolveImports()
{
    for (const auto& [file, spec] : importSpecs_) module(spec, file);
}

void Engine::resolve()
{
    resolveImports();
    fixpoint();
    resolveRest();
}

// INHERITS, to a fixpoint. A base may itself depend on bases -- in `class
// Y(Derived.Inner)` Inner may be inherited by Derived -- so one pass in ref
// order would answer differently depending on which class came first. Each
// round resolves every base against the bases the previous round found, never
// against a half-built set, and rounds repeat until nothing changes. A class's
// bases are kept in source order (line, then position on the line), which is
// the order the base search walks them. Every class with an INHERITS ref here
// starts from no bases; any other class keeps its stored ones. False if the
// round cap was hit.
bool Engine::fixpoint()
{
    outcomes_.resize(refs_.size());
    deps_.resize(refs_.size());
    answered_.resize(refs_.size());

    std::vector<std::size_t> inherits;
    for (std::size_t i = 0; i < refs_.size(); ++i)
        if (refs_[i].kind == "INHERITS" && languageOf(refs_[i].file_id)) inherits.push_back(i);
    std::sort(inherits.begin(), inherits.end(), [&](std::size_t a, std::size_t b) {
        const Ref& ra = refs_[a];
        const Ref& rb = refs_[b];
        if (ra.file_id != rb.file_id) return ra.file_id < rb.file_id;
        if (ra.line != rb.line) return ra.line < rb.line;
        return ra.id < rb.id;
    });
    for (std::size_t i : inherits) {
        affected_.insert(refs_[i].source_id);
        bases_.erase(refs_[i].source_id);
    }

    auto same = [](const Outcome& a, const Outcome& b) {
        return a.status == b.status && a.kind == b.kind && a.id == b.id && a.heuristic == b.heuristic;
    };
    inheritsCapHit_ = true;
    for (int round = 0; round < kMaxInheritsRounds; ++round) {
        inheritsRounds_ = round + 1;
        bool changed = false;
        std::unordered_map<std::string, Bases> next;
        for (std::size_t i : inherits) {
            Outcome previous = std::move(outcomes_[i]);
            resolveOne(i);
            if (round == 0 || !same(outcomes_[i], previous)) changed = true;

            std::string base;
            const Outcome& r = outcomes_[i];
            if (r.resolved() && r.kind == Outcome::Symbol) {
                auto sym = symbols_.find(r.id);
                if (sym != symbols_.end() && isClassKind(sym->second.kind)) base = r.id;
            }
            next[refs_[i].source_id].emplace_back(std::move(base), r.status);
        }
        for (auto& [cls, bases] : next) bases_[cls] = std::move(bases);
        if (!changed) {
            inheritsCapHit_ = false;
            break;
        }
    }
    return !inheritsCapHit_;
}

// Everything else, against the final bases.
void Engine::resolveRest()
{
    outcomes_.resize(refs_.size());
    deps_.resize(refs_.size());
    answered_.resize(refs_.size());
    for (std::size_t i = 0; i < refs_.size(); ++i)
        if (refs_[i].kind != "INHERITS") resolveOne(i);
}

std::string Engine::basesNow(const std::string& cls) const
{
    auto it = bases_.find(cls);
    return it == bases_.end() ? std::string() : basesValue(it->second);
}

std::string Engine::basesStored(const std::string& cls)
{
    ensure(fileOf(cls));
    auto it = storedBases_.find(cls);
    return it == storedBases_.end() ? std::string() : basesValue(it->second);
}

// ── Writing back ────────────────────────────────────────────────────────────

const char* const kImportUpdateSql =
    "UPDATE import_resolution SET resolved_file_id = ?, status = ?"
    " WHERE file_id = ? AND module_spec = ?;";

// The parser wrote each spec; only the answer is the resolver's. A file with
// no language plugin gets 'pending', as its refs do.
int writeImport(Engine& e, sqlite3_stmt* upd, const std::string& file, const std::string& spec,
                Resolver::Stats* stats)
{
    sqlite3_reset(upd);
    bindText(upd, 3, file);
    bindText(upd, 4, spec);
    if (e.languageOf(file)) {
        const Import& imp = e.module(spec, file);
        bindText(upd, 1, imp.file_id);
        sqlite3_bind_text(upd, 2, imp.status, -1, SQLITE_STATIC);
        if (stats) ++stats->imports;
    } else {
        sqlite3_bind_null(upd, 1);
        sqlite3_bind_text(upd, 2, "pending", -1, SQLITE_STATIC);
    }
    return done(sqlite3_step(upd));
}

// One ref's answer, emits_link as given. No answer (no language plugin):
// pending, whatever an earlier run decided, so that `link` holds nothing for it.
int writeOutcome(sqlite3_stmt* upd, const Ref& ref, const Outcome& o, bool emits,
                 Resolver::Stats* stats)
{
    sqlite3_reset(upd);
    sqlite3_bind_int64(upd, 6, ref.id);
    if (!o.found()) {
        for (int col = 1; col <= 4; ++col) sqlite3_bind_null(upd, col);
        sqlite3_bind_text(upd, 2, "pending", -1, SQLITE_STATIC);
        sqlite3_bind_int(upd, 5, 0);
        return done(sqlite3_step(upd));
    }
    if (o.resolved()) bindText(upd, 1, o.id);
    else sqlite3_bind_null(upd, 1);
    sqlite3_bind_text(upd, 2, o.status, -1, SQLITE_STATIC);
    if (o.status == kUnresolved)
        sqlite3_bind_null(upd, 3);
    else
        sqlite3_bind_text(upd, 3, o.heuristic ? "heuristic" : "exact", -1, SQLITE_STATIC);
    sqlite3_bind_text(upd, 4, Resolver::kName, -1, SQLITE_STATIC);
    sqlite3_bind_int(upd, 5, emits ? 1 : 0);
    if (stats) ++stats->status[o.status];
    return done(sqlite3_step(upd));
}

const char* const kRefUpdateSql =
    "UPDATE ref SET resolved_id = ?, status = ?, confidence = ?, resolver = ?, emits_link = ?"
    " WHERE ref_id = ?;";
// The same answer, for a ref not in the table yet (resolveScan): 1-6 as in
// kRefUpdateSql, then the columns DbInserter would have written.
const char* const kRefInsertSql =
    "INSERT INTO ref(resolved_id, status, confidence, resolver, emits_link, ref_id,"
    "                file_id, source_id, scope_id, kind, namespace, name_path, raw_text, line)"
    " VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14);";

int Engine::writeAll(Resolver::Stats* stats)
{
    Stmt upd;
    int rc = upd.prepare(db_, kImportUpdateSql);
    if (rc != SQLITE_OK) return rc;
    for (const auto& [file, spec] : importSpecs_) {
        rc = writeImport(*this, upd.s, file, spec, stats);
        if (rc != SQLITE_OK) return rc;
    }

    Stmt refUpd;
    rc = refUpd.prepare(db_, fresh_.empty() ? kRefUpdateSql : kRefInsertSql);
    if (rc != SQLITE_OK) return rc;

    // Each link once, on its first ref: refs_ is in ref_id order, and the refs
    // of one link share their source, so their file. Every ref is written, so
    // the refs marked are every `link` row but IMPORTS.
    std::unordered_set<std::string> linked;
    std::map<std::string, int> links;
    for (std::size_t i = 0; i < refs_.size(); ++i) {
        const Ref& ref = refs_[i];
        const Outcome& o = outcomes_[i];
        bool emits = false;
        if (o.resolved() && o.kind == Outcome::Symbol) {
            auto sym = symbols_.find(o.id);
            if (sym != symbols_.end()) emits = linksTo(ref.kind, sym->second.kind);
        }
        if (emits) emits = linked.insert(key(ref.kind, key(ref.source_id, o.id))).second;
        if (emits) ++links[ref.kind];
        if (!fresh_.empty()) {
            // The parser's columns; writeOutcome binds the answer and ref_id.
            const RefEntity& e = *fresh_[i];
            sqlite3_reset(refUpd.s);
            sqlite3_bind_text(refUpd.s, 7, e.file_id.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_text(refUpd.s, 8, e.source_id.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_text(refUpd.s, 9, e.scope_id.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_text(refUpd.s, 10, e.kind.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_text(refUpd.s, 11, e.ns.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_text(refUpd.s, 12, e.name_path.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_text(refUpd.s, 13, e.raw_text.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_int(refUpd.s, 14, e.line);
        }
        rc = writeOutcome(refUpd.s, ref, o, emits, stats);
        if (rc != SQLITE_OK) return rc;
    }

    rc = writeDeps(true);
    if (rc != SQLITE_OK) return rc;
    if (stats)
        for (const auto& [type, n] : links) stats->links[type] = n;
    return SQLITE_OK;
}

// fact.fact_id of an interned fact, adding the row if there is none.
std::int64_t Engine::storedFact(int fact)
{
    storedFacts_.resize(factKeys_.size(), 0);
    if (storedFacts_[fact]) return storedFacts_[fact];
    if (!factInsert_.s &&
        (factInsert_.prepare(db_, "INSERT OR IGNORE INTO fact(key) VALUES(?);") != SQLITE_OK ||
         factSelect_.prepare(db_, "SELECT fact_id FROM fact WHERE key = ?;") != SQLITE_OK))
        return 0;
    bindText(factInsert_.s, 1, *factKeys_[fact]);
    const int rc = sqlite3_step(factInsert_.s);
    sqlite3_reset(factInsert_.s);
    if (rc != SQLITE_DONE) return 0;
    bindText(factSelect_.s, 1, *factKeys_[fact]);
    if (sqlite3_step(factSelect_.s) == SQLITE_ROW)
        storedFacts_[fact] = sqlite3_column_int64(factSelect_.s, 0);
    sqlite3_reset(factSelect_.s);
    return storedFacts_[fact];
}

// ref_dep for every answered ref: the facts it read, less those of its own
// file. `all`: every ref was answered, so fact and ref_dep start over. Only
// facts some ref_dep row reaches, directly or through fact_edge, are kept --
// an E answer worked out but read by no ref in the end (an INHERITS round
// that took another path) is not -- which is what collectGarbage leaves after
// an update.
int Engine::writeDeps(bool all)
{
    auto kept = [&](int fact, const Ref& ref) {
        const std::string& owner = factOwners_[fact];
        return owner.empty() || owner != ref.file_id;
    };
    int rc;
    if (all) {
        rc = sqlite3_exec(db_, "DELETE FROM ref_dep; DELETE FROM fact_edge; DELETE FROM fact;",
                          nullptr, nullptr, nullptr);
        if (rc != SQLITE_OK) return rc;
        std::vector<char> used(factKeys_.size(), 0);
        std::vector<std::pair<int, std::int64_t>> pairs;
        for (std::size_t i = 0; i < refs_.size(); ++i)
            for (int fact : deps_[i])
                if (kept(fact, refs_[i])) {
                    used[fact] = 1;
                    pairs.emplace_back(fact, refs_[i].id);
                }
        std::vector<int> todo;
        for (std::size_t f = 0; f < used.size(); ++f)
            if (used[f]) todo.push_back(static_cast<int>(f));
        std::vector<std::pair<int, int>> edges;
        while (!todo.empty()) {
            const int parent = todo.back();
            todo.pop_back();
            auto it = edges_.find(parent);
            if (it == edges_.end()) continue;
            for (int child : it->second) {
                edges.emplace_back(parent, child);
                if (!used[child]) {
                    used[child] = 1;
                    todo.push_back(child);
                }
            }
        }
        Stmt ins;
        rc = ins.prepare(db_, "INSERT INTO fact(fact_id, key) VALUES(?, ?);");
        if (rc != SQLITE_OK) return rc;
        for (std::size_t f = 0; f < used.size(); ++f) {
            if (!used[f]) continue;
            sqlite3_reset(ins.s);
            sqlite3_bind_int64(ins.s, 1, static_cast<std::int64_t>(f) + 1);
            bindText(ins.s, 2, *factKeys_[f]);
            if ((rc = done(sqlite3_step(ins.s))) != SQLITE_OK) return rc;
        }
        // In key order: the table is clustered on (fact_id, ref_id).
        std::sort(pairs.begin(), pairs.end());
        Stmt dep;
        rc = dep.prepare(db_, "INSERT INTO ref_dep(fact_id, ref_id) VALUES(?, ?);");
        if (rc != SQLITE_OK) return rc;
        for (const auto& [fact, ref] : pairs) {
            sqlite3_reset(dep.s);
            sqlite3_bind_int64(dep.s, 1, static_cast<std::int64_t>(fact) + 1);
            sqlite3_bind_int64(dep.s, 2, ref);
            if ((rc = done(sqlite3_step(dep.s))) != SQLITE_OK) return rc;
        }
        std::sort(edges.begin(), edges.end());
        Stmt edge;
        rc = edge.prepare(db_, "INSERT INTO fact_edge(parent_id, child_id) VALUES(?, ?);");
        if (rc != SQLITE_OK) return rc;
        for (const auto& [parent, child] : edges) {
            sqlite3_reset(edge.s);
            sqlite3_bind_int64(edge.s, 1, static_cast<std::int64_t>(parent) + 1);
            sqlite3_bind_int64(edge.s, 2, static_cast<std::int64_t>(child) + 1);
            if ((rc = done(sqlite3_step(edge.s))) != SQLITE_OK) return rc;
        }
        return SQLITE_OK;
    }

    Stmt del, dep;
    rc = del.prepare(db_, "DELETE FROM ref_dep WHERE ref_id = ? RETURNING fact_id;");
    if (rc != SQLITE_OK) return rc;
    rc = dep.prepare(db_, "INSERT OR IGNORE INTO ref_dep(fact_id, ref_id) VALUES(?, ?);");
    if (rc != SQLITE_OK) return rc;
    auto failed = [&] { return sqlite3_errcode(db_) == SQLITE_OK ? SQLITE_ERROR : sqlite3_errcode(db_); };
    for (std::size_t i = 0; i < refs_.size(); ++i) {
        if (!answered_[i]) continue;
        sqlite3_bind_int64(del.s, 1, refs_[i].id);
        rc = each(del.s, [&](sqlite3_stmt* s) { released_.insert(sqlite3_column_int64(s, 0)); });
        if (rc != SQLITE_OK) return rc;
        for (int fact : deps_[i]) {
            if (!kept(fact, refs_[i])) continue;
            const std::int64_t id = storedFact(fact);
            if (!id) return failed();
            sqlite3_reset(dep.s);
            sqlite3_bind_int64(dep.s, 1, id);
            sqlite3_bind_int64(dep.s, 2, refs_[i].id);
            if ((rc = done(sqlite3_step(dep.s))) != SQLITE_OK) return rc;
        }
    }

    // Every E fact answered again this run: what it reads now replaces what
    // it read before. (One not answered again kept its facts, or its refs
    // would have been re-answered and found it again.)
    Stmt delEdges, edge;
    rc = delEdges.prepare(db_, "DELETE FROM fact_edge WHERE parent_id = ? RETURNING child_id;");
    if (rc != SQLITE_OK) return rc;
    rc = edge.prepare(db_, "INSERT OR IGNORE INTO fact_edge(parent_id, child_id) VALUES(?, ?);");
    if (rc != SQLITE_OK) return rc;
    for (const auto& [parent, children] : edges_) {
        const std::int64_t id = storedFact(parent);
        if (!id) return failed();
        released_.insert(id);
        sqlite3_bind_int64(delEdges.s, 1, id);
        rc = each(delEdges.s, [&](sqlite3_stmt* s) { released_.insert(sqlite3_column_int64(s, 0)); });
        if (rc != SQLITE_OK) return rc;
        for (int child : children) {
            const std::int64_t childId = storedFact(child);
            if (!childId) return failed();
            sqlite3_reset(edge.s);
            sqlite3_bind_int64(edge.s, 1, id);
            sqlite3_bind_int64(edge.s, 2, childId);
            if ((rc = done(sqlite3_step(edge.s))) != SQLITE_OK) return rc;
        }
    }
    return SQLITE_OK;
}

// After writeDeps(false): drops every fact no ref_dep row reaches any more,
// directly or through fact_edge, with its edges -- what a scan of the same
// files would not have stored. Only facts that lost a row this update
// (released_) can have become unreachable, and a fact only by all of its
// parents going too, so the work is the released facts and what they lead
// to, not the tables. A released fact is kept if a search up its fact_edge
// parents meets one with a ref_dep row; if it does not, every fact the search
// met is unreachable too (their parents were all searched), and their
// children are released in turn. Cycles of E facts are no different.
int Engine::collectGarbage()
{
    Stmt hasDep, parents, dropEdges, dropFact;
    int rc = hasDep.prepare(db_, "SELECT 1 FROM ref_dep WHERE fact_id = ? LIMIT 1;");
    if (rc != SQLITE_OK) return rc;
    rc = parents.prepare(db_, "SELECT parent_id FROM fact_edge WHERE child_id = ?;");
    if (rc != SQLITE_OK) return rc;
    rc = dropEdges.prepare(db_, "DELETE FROM fact_edge WHERE parent_id = ? RETURNING child_id;");
    if (rc != SQLITE_OK) return rc;
    rc = dropFact.prepare(db_, "DELETE FROM fact WHERE fact_id = ?;");
    if (rc != SQLITE_OK) return rc;

    std::unordered_set<std::int64_t> live, dead;
    std::vector<std::int64_t> todo(released_.begin(), released_.end());
    released_.clear();
    while (!todo.empty()) {
        const std::int64_t fact = todo.back();
        todo.pop_back();
        if (live.count(fact) || dead.count(fact)) continue;

        bool reached = false;
        std::vector<std::int64_t> met, up{fact};
        std::unordered_set<std::int64_t> seen;
        while (!up.empty() && !reached) {
            const std::int64_t f = up.back();
            up.pop_back();
            if (dead.count(f) || !seen.insert(f).second) continue;
            if (live.count(f)) {
                reached = true;
                break;
            }
            met.push_back(f);
            sqlite3_bind_int64(hasDep.s, 1, f);
            rc = each(hasDep.s, [&](sqlite3_stmt*) { reached = true; });
            if (rc != SQLITE_OK) return rc;
            if (reached) break;
            sqlite3_bind_int64(parents.s, 1, f);
            rc = each(parents.s, [&](sqlite3_stmt* s) { up.push_back(sqlite3_column_int64(s, 0)); });
            if (rc != SQLITE_OK) return rc;
        }
        if (reached) {
            live.insert(fact);
            continue;
        }
        for (std::int64_t f : met) dead.insert(f);
        for (std::int64_t f : met) {
            sqlite3_bind_int64(dropEdges.s, 1, f);
            rc = each(dropEdges.s, [&](sqlite3_stmt* s) { todo.push_back(sqlite3_column_int64(s, 0)); });
            if (rc != SQLITE_OK) return rc;
            sqlite3_bind_int64(dropFact.s, 1, f);
            if ((rc = done(sqlite3_step(dropFact.s))) != SQLITE_OK) return rc;
            sqlite3_reset(dropFact.s);
        }
    }
    return SQLITE_OK;
}

// The answered refs and the given import_resolution rows. emits_link is then
// worked out again for every file an answered ref is in, from what the file's
// refs now say: each link on its first ref, as writeAll has it.
int Engine::writeChanged(const std::vector<std::pair<std::string, std::string>>& imports,
                         Resolver::Stats* stats)
{
    Stmt upd;
    int rc = upd.prepare(db_, kImportUpdateSql);
    if (rc != SQLITE_OK) return rc;
    for (const auto& [file, spec] : imports) {
        rc = writeImport(*this, upd.s, file, spec, stats);
        if (rc != SQLITE_OK) return rc;
    }

    Stmt refUpd;
    rc = refUpd.prepare(db_, kRefUpdateSql);
    if (rc != SQLITE_OK) return rc;
    std::set<std::string> files;
    for (std::size_t i = 0; i < refs_.size(); ++i) {
        if (!answered_[i]) continue;
        rc = writeOutcome(refUpd.s, refs_[i], outcomes_[i], false, stats);
        if (rc != SQLITE_OK) return rc;
        files.insert(refs_[i].file_id);
    }
    rc = writeDeps(false);
    if (rc != SQLITE_OK) return rc;
    rc = collectGarbage();
    if (rc != SQLITE_OK) return rc;

    Stmt sel, mark;
    rc = sel.prepare(db_,
        "SELECT r.ref_id, r.kind, r.source_id, r.resolved_id, r.status, r.emits_link, s.kind"
        "  FROM ref r LEFT JOIN symbol s ON s.symbol_id = r.resolved_id"
        " WHERE r.file_id = ? AND (r.status = 'resolved' OR r.emits_link = 1)"
        " ORDER BY r.ref_id;");
    if (rc != SQLITE_OK) return rc;
    rc = mark.prepare(db_, "UPDATE ref SET emits_link = ? WHERE ref_id = ?;");
    if (rc != SQLITE_OK) return rc;
    for (const std::string& file : files) {
        std::unordered_set<std::string> linked;
        std::vector<std::pair<std::int64_t, int>> marks;
        bindText(sel.s, 1, file);
        rc = each(sel.s, [&](sqlite3_stmt* s) {
            const std::string kind = columnText(s, 1);
            bool emits = std::string(columnText(s, 4)) == kResolved &&
                         sqlite3_column_type(s, 6) != SQLITE_NULL &&
                         linksTo(kind, columnText(s, 6));
            if (emits) emits = linked.insert(key(kind, key(columnText(s, 2), columnText(s, 3)))).second;
            if (emits != (sqlite3_column_int(s, 5) != 0))
                marks.emplace_back(sqlite3_column_int64(s, 0), emits ? 1 : 0);
        });
        if (rc != SQLITE_OK) return rc;
        for (const auto& [id, emits] : marks) {
            sqlite3_reset(mark.s);
            sqlite3_bind_int(mark.s, 1, emits);
            sqlite3_bind_int64(mark.s, 2, id);
            if ((rc = done(sqlite3_step(mark.s))) != SQLITE_OK) return rc;
        }
    }
    return SQLITE_OK;
}

// ── An update ───────────────────────────────────────────────────────────────

// The refs that read any of `facts`, by their stored ref_dep: directly, or
// through E facts that read them (fact_edge, followed up to its roots).
int addDependents(sqlite3* db, Engine& e, const std::set<std::string>& facts)
{
    if (facts.empty()) return SQLITE_OK;
    Stmt byKey, parents, ids, ref;
    int rc = byKey.prepare(db, "SELECT fact_id FROM fact WHERE key = ?;");
    if (rc != SQLITE_OK) return rc;
    rc = parents.prepare(db, "SELECT parent_id FROM fact_edge WHERE child_id = ?;");
    if (rc != SQLITE_OK) return rc;
    rc = ids.prepare(db, "SELECT ref_id FROM ref_dep WHERE fact_id = ?;");
    if (rc != SQLITE_OK) return rc;
    rc = ref.prepare(db, (std::string("SELECT ") + Engine::kRefColumns +
                          " FROM ref WHERE ref_id = ?;").c_str());
    if (rc != SQLITE_OK) return rc;

    std::unordered_set<std::int64_t> seen;
    std::vector<std::int64_t> todo;
    for (const std::string& fact : facts) {
        bindText(byKey.s, 1, fact);
        rc = each(byKey.s, [&](sqlite3_stmt* s) {
            if (seen.insert(sqlite3_column_int64(s, 0)).second) todo.push_back(sqlite3_column_int64(s, 0));
        });
        if (rc != SQLITE_OK) return rc;
    }
    std::set<std::int64_t> found;
    while (!todo.empty()) {
        const std::int64_t fact = todo.back();
        todo.pop_back();
        sqlite3_bind_int64(ids.s, 1, fact);
        rc = each(ids.s, [&](sqlite3_stmt* s) { found.insert(sqlite3_column_int64(s, 0)); });
        if (rc != SQLITE_OK) return rc;
        sqlite3_bind_int64(parents.s, 1, fact);
        rc = each(parents.s, [&](sqlite3_stmt* s) {
            if (seen.insert(sqlite3_column_int64(s, 0)).second) todo.push_back(sqlite3_column_int64(s, 0));
        });
        if (rc != SQLITE_OK) return rc;
    }
    for (std::int64_t id : found) {
        sqlite3_bind_int64(ref.s, 1, id);
        if ((rc = e.addRefs(ref.s)) != SQLITE_OK) return rc;
    }
    return SQLITE_OK;
}

// Every INHERITS ref of each class that has one among e's refs: a class's
// bases are answered together.
int addClassBases(sqlite3* db, Engine& e, std::set<std::string>& classes)
{
    Stmt stmt;
    int rc = stmt.prepare(db, (std::string("SELECT ") + Engine::kRefColumns +
                               " FROM ref WHERE source_id = ? AND kind = 'INHERITS';").c_str());
    if (rc != SQLITE_OK) return rc;
    for (std::size_t i = 0; i < e.refCount(); ++i) {
        if (e.ref(i).kind != "INHERITS" || !classes.insert(e.ref(i).source_id).second) continue;
        const std::string cls = e.ref(i).source_id;
        bindText(stmt.s, 1, cls);
        if ((rc = e.addRefs(stmt.s)) != SQLITE_OK) return rc;
    }
    return SQLITE_OK;
}

}  // namespace

void Resolver::warnIfCapped(const Stats& stats, const char* who)
{
    if (!stats.inheritsCapHit) return;
    std::fprintf(stderr,
                 "[%s] warning: base classes did not settle within %d rounds;"
                 " some INHERITS answers may depend on file order\n",
                 who, stats.inheritsRounds);
}

int Resolver::resolveAll(sqlite3* db, Stats* stats, const LanguageRegistry& languages)
{
    const auto t0 = std::chrono::steady_clock::now();
    Engine engine(db, languages);
    int rc = engine.loadAll();
    if (rc != SQLITE_OK) return rc;
    engine.resolve();
    if (stats) {
        stats->inheritsRounds = engine.inheritsRounds();
        stats->inheritsCapHit = engine.inheritsCapHit();
    }
    rc = engine.writeAll(stats);
    if (stats)
        stats->ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
    return rc;
}

int Resolver::resolveScan(sqlite3* db, const std::vector<ParseResult>& results, Stats* stats,
                          const LanguageRegistry& languages)
{
    const auto t0 = std::chrono::steady_clock::now();
    Engine engine(db, languages);
    int rc = engine.loadAll(&results);
    if (rc != SQLITE_OK) return rc;
    engine.resolve();
    if (stats) {
        stats->inheritsRounds = engine.inheritsRounds();
        stats->inheritsCapHit = engine.inheritsCapHit();
    }
    rc = engine.writeAll(stats);
    if (stats)
        stats->ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
    return rc;
}

int Resolver::beginChange(sqlite3* db, const std::vector<std::string>& files, Change& change)
{
    change = Change();
    Stmt all;
    int rc = all.prepare(db, "SELECT file_id, language FROM file;");
    if (rc != SQLITE_OK) return rc;
    rc = each(all.s, [&](sqlite3_stmt* s) {
        change.files.emplace_back(columnText(s, 0), columnText(s, 1));
    });
    if (rc != SQLITE_OK) return rc;

    Stmt bases, released;
    rc = bases.prepare(db, kBasesSql);
    if (rc != SQLITE_OK) return rc;
    // The refs of the files go, and their ref_dep rows with them: what they
    // read may be read by nothing else any more (Engine::collectGarbage).
    rc = released.prepare(db,
        "SELECT DISTINCT d.fact_id FROM ref r JOIN ref_dep d ON d.ref_id = r.ref_id"
        " WHERE r.file_id = ?;");
    if (rc != SQLITE_OK) return rc;
    for (const std::string& file : files) {
        if (!change.touched.insert(file).second) continue;
        rc = loadFileFacts(db, file, change.facts);
        if (rc != SQLITE_OK) return rc;
        bindText(released.s, 1, file);
        rc = each(released.s, [&](sqlite3_stmt* s) {
            change.released.push_back(sqlite3_column_int64(s, 0));
        });
        if (rc != SQLITE_OK) return rc;
        std::map<std::string, Bases> found;
        bindText(bases.s, 1, file);
        rc = each(bases.s, [&](sqlite3_stmt* s) {
            found[columnText(s, 0)].push_back(baseOf(s));
        });
        if (rc != SQLITE_OK) return rc;
        for (const auto& [cls, b] : found) change.bases[cls] = basesValue(b);
    }

    // A class elsewhere with a base in the files: its stored answers stay,
    // but whether one is a base is read off the symbol it names, which goes.
    // What the bases were is only known now.
    Stmt subclasses;
    rc = subclasses.prepare(db,
        "SELECT DISTINCT r.source_id FROM symbol s JOIN ref r ON r.resolved_id = s.symbol_id"
        " WHERE s.file_id = ? AND r.kind = 'INHERITS';");
    if (rc != SQLITE_OK) return rc;
    std::map<std::string, std::set<std::string>> outside;   // file -> its classes
    for (const std::string& file : change.touched) {
        bindText(subclasses.s, 1, file);
        rc = each(subclasses.s, [&](sqlite3_stmt* s) {
            const std::string cls = columnText(s, 0);
            if (!change.touched.count(fileOf(cls))) outside[fileOf(cls)].insert(cls);
        });
        if (rc != SQLITE_OK) return rc;
    }
    for (const auto& [file, classes] : outside) {
        std::map<std::string, Bases> found;
        bindText(bases.s, 1, file);
        rc = each(bases.s, [&](sqlite3_stmt* s) {
            if (classes.count(columnText(s, 0))) found[columnText(s, 0)].push_back(baseOf(s));
        });
        if (rc != SQLITE_OK) return rc;
        for (const auto& [cls, b] : found) change.bases[cls] = basesValue(b);
    }
    return SQLITE_OK;
}

// After an update: re-answer only what it can have changed.
//   1. The facts the touched files held before (beginChange) and hold now:
//      every key whose value differs is a changed fact.
//   2. If the set of files changed, every module spec may name something
//      else: each M fact and import_resolution row is answered again by the
//      old and the new file set, and every one that differs is changed.
//   3. To answer: every ref of a touched file, and every ref whose ref_dep
//      holds a changed fact.
//   4. The INHERITS refs among them, with every other INHERITS ref of their
//      classes, to a fixpoint; any other class keeps its stored bases. A
//      class whose bases came out different is a changed K fact, whose
//      dependents are added -- and if any of them is an INHERITS ref, step 4
//      runs again from the start with the larger set.
//   5. The other refs, against those bases. Then emits_link again for the
//      files of every ref answered.
// A fixpoint that hits the round cap, or a file whose language changed, falls
// back to resolveAll.
int Resolver::resolveChanged(sqlite3* db, const Change& change, Stats* stats,
                             const LanguageRegistry& languages)
{
    const auto t0 = std::chrono::steady_clock::now();
    Engine e(db, languages);
    int rc = e.loadFiles();
    if (rc != SQLITE_OK) return rc;

    std::unordered_map<std::string, std::string> before(change.files.begin(), change.files.end());
    bool filesChanged = before.size() != e.files().size();
    {
        Stmt now;
        rc = now.prepare(db, "SELECT file_id, language FROM file;");
        if (rc != SQLITE_OK) return rc;
        bool languageChanged = false;
        rc = each(now.s, [&](sqlite3_stmt* s) {
            auto it = before.find(columnText(s, 0));
            if (it == before.end()) filesChanged = true;
            else if (it->second != columnText(s, 1)) languageChanged = true;
        });
        if (rc != SQLITE_OK) return rc;
        if (languageChanged) return resolveAll(db, stats, languages);
    }
    e.setTouched(change.touched);
    e.release(change.released);

    // 1. Facts of the touched files.
    std::set<std::string> changed;
    std::map<std::string, std::string> now;
    for (const std::string& file : change.touched) {
        rc = loadFileFacts(db, file, now);
        if (rc != SQLITE_OK) return rc;
    }
    for (const auto& [k, v] : change.facts) {
        auto it = now.find(k);
        if (it == now.end() || it->second != v) changed.insert(k);
    }
    for (const auto& [k, v] : now)
        if (!change.facts.count(k)) changed.insert(k);

    // 2. Module specs.
    std::vector<std::pair<std::string, std::string>> imports;
    if (filesChanged) {
        std::unordered_map<std::string, const ILanguage*> oldLanguage;
        std::unordered_map<const ILanguage*, std::unordered_set<std::string>> oldIds;
        for (const auto& [file, language] : change.files) {
            const ILanguage* lang = languages.find(language);
            oldLanguage[file] = lang;
            if (lang) oldIds[lang].insert(file);
        }
        std::unordered_map<const ILanguage*, ProjectIndex> oldIndex;
        for (auto& [lang, ids] : oldIds) oldIndex.emplace(lang, lang->buildIndex(std::move(ids)));
        auto oldAnswer = [&](const std::string& spec, const std::string& from) {
            auto lang = oldLanguage.find(from);
            if (lang == oldLanguage.end() || !lang->second) return Import();
            return answer(lang->second, &oldIndex.at(lang->second), spec, from);
        };
        auto differ = [](const Import& a, const Import& b) {
            return a.file_id != b.file_id || a.status != b.status;
        };

        Stmt facts;
        rc = facts.prepare(db, "SELECT key FROM fact WHERE key >= 'M\n' AND key < 'M\x0b';");
        if (rc != SQLITE_OK) return rc;
        rc = each(facts.s, [&](sqlite3_stmt* s) {
            const std::string k = columnText(s, 0);
            const std::size_t sep = k.find('\n', 2);
            if (sep == std::string::npos) return;
            const std::string from = k.substr(2, sep - 2);
            const std::string spec = k.substr(sep + 1);
            if (differ(oldAnswer(spec, from), e.module(spec, from))) changed.insert(k);
        });
        if (rc != SQLITE_OK) return rc;

        Stmt rows;
        rc = rows.prepare(db,
            "SELECT file_id, module_spec, resolved_file_id, status FROM import_resolution;");
        if (rc != SQLITE_OK) return rc;
        rc = each(rows.s, [&](sqlite3_stmt* s) {
            const std::string file = columnText(s, 0);
            const std::string spec = columnText(s, 1);
            if (!change.touched.count(file)) {
                const char* status = e.languageOf(file) ? e.module(spec, file).status : "pending";
                const std::string target = e.languageOf(file) ? e.module(spec, file).file_id : "";
                if (target == columnText(s, 2) && std::string(status) == columnText(s, 3)) return;
            }
            imports.emplace_back(file, spec);
        });
        if (rc != SQLITE_OK) return rc;
    } else {
        Stmt rows;
        rc = rows.prepare(db, "SELECT file_id, module_spec FROM import_resolution WHERE file_id = ?;");
        if (rc != SQLITE_OK) return rc;
        for (const std::string& file : change.touched) {
            bindText(rows.s, 1, file);
            rc = each(rows.s, [&](sqlite3_stmt* s) { imports.emplace_back(file, columnText(s, 1)); });
            if (rc != SQLITE_OK) return rc;
        }
    }

    // 3. What to answer.
    {
        Stmt refs;
        rc = refs.prepare(db, (std::string("SELECT ") + Engine::kRefColumns +
                               " FROM ref WHERE file_id = ? ORDER BY ref_id;").c_str());
        if (rc != SQLITE_OK) return rc;
        for (const std::string& file : change.touched) {
            bindText(refs.s, 1, file);
            if ((rc = e.addRefs(refs.s)) != SQLITE_OK) return rc;
        }
    }
    rc = addDependents(db, e, changed);
    if (rc != SQLITE_OK) return rc;

    // 4. Bases.
    std::set<std::string> classes, askedK;
    while (true) {
        rc = addClassBases(db, e, classes);
        if (rc != SQLITE_OK) return rc;
        // Known gap (docs): a scan that hit the cap stored bases short of a
        // fixpoint, from which a later update can settle further than a scan.
        if (!e.fixpoint()) return resolveAll(db, stats, languages);
        if (e.error() != SQLITE_OK) return e.error();

        // A class of a touched file, or one whose INHERITS refs were answered:
        // no other class's bases can have changed. What they were is what
        // beginChange saw, else (a class of an untouched file none of whose
        // bases was in a touched file) what is stored.
        std::set<std::string> candidates(e.affectedClasses().begin(), e.affectedClasses().end());
        for (const auto& [cls, value] : change.bases)
            if (change.touched.count(fileOf(cls))) candidates.insert(cls);
        std::set<std::string> changedK;
        for (const std::string& cls : candidates) {
            if (askedK.count(cls)) continue;
            auto old = change.bases.find(cls);
            const std::string was = old != change.bases.end()          ? old->second
                                    : change.touched.count(fileOf(cls)) ? std::string()
                                                                        : e.basesStored(cls);
            if (was != e.basesNow(cls)) {
                changedK.insert(factK(cls));
                askedK.insert(cls);
            }
        }
        const std::size_t count = e.refCount();
        rc = addDependents(db, e, changedK);
        if (rc != SQLITE_OK) return rc;
        bool moreInherits = false;
        for (std::size_t i = count; i < e.refCount(); ++i)
            if (e.ref(i).kind == "INHERITS") moreInherits = true;
        if (!moreInherits) break;
    }

    // 5. The rest, and writing it all.
    e.resolveRest();
    if (e.error() != SQLITE_OK) return e.error();
    if (stats) {
        stats->incremental = true;
        stats->inheritsRounds = e.inheritsRounds();
        stats->inheritsCapHit = e.inheritsCapHit();
    }
    rc = e.writeChanged(imports, stats);
    if (stats)
        stats->ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
    return rc;
}

int Resolver::findSubmoduleImporters(sqlite3* db, const std::string& fileId,
                                     std::vector<std::string>& files,
                                     const LanguageRegistry& languages)
{
    Engine e(db, languages);
    int rc = e.loadFiles();
    if (rc != SQLITE_OK) return rc;

    // `from pkg import mod` imports mod under the last part of its module name.
    std::string name;
    Stmt mod;
    rc = mod.prepare(db, "SELECT module_name FROM file WHERE file_id = ?;");
    if (rc != SQLITE_OK) return rc;
    bindText(mod.s, 1, fileId);
    rc = each(mod.s, [&](sqlite3_stmt* s) {
        const std::string dotted = columnText(s, 0);
        name = dotted.substr(dotted.rfind('.') + 1);
    });
    if (rc != SQLITE_OK || name.empty()) return rc;

    Stmt stmt;
    rc = stmt.prepare(db,
        "SELECT file_id, local_name, namespace, kind, target_id, module_spec, imported_name,"
        "       line, is_exported"
        "  FROM binding WHERE kind IN ('import_symbol', 'reexport') AND imported_name = ?"
        "   AND file_id <> ?;");
    if (rc != SQLITE_OK) return rc;
    bindText(stmt.s, 1, name);
    bindText(stmt.s, 2, fileId);
    std::vector<std::pair<Binding, std::string>> found;
    rc = each(stmt.s, [&](sqlite3_stmt* s) {
        Binding b;
        b.file_id       = columnText(s, 0);
        b.local_name    = columnText(s, 1);
        b.kind          = columnText(s, 3);
        b.target_id     = columnText(s, 4);
        b.module_spec   = columnText(s, 5);
        b.imported_name = columnText(s, 6);
        b.line          = sqlite3_column_int(s, 7);
        b.exported      = sqlite3_column_int(s, 8) != 0;
        found.emplace_back(std::move(b), columnText(s, 2));
    });
    if (rc != SQLITE_OK) return rc;
    for (const auto& [b, ns] : found) {
        const Outcome o = e.bindingOutcome(b, ns);
        if (o.kind == Outcome::Module && o.resolved() && o.id == fileId) files.push_back(b.file_id);
    }
    return e.error();
}
