#include "Resolver.h"

#include "db/db.h"

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
// Rounds of the INHERITS pass before giving up on a fixpoint (see resolve()).
constexpr int kMaxInheritsRounds = 10;

const char* const kResolved   = "resolved";
const char* const kBuiltin    = "builtin";
const char* const kExternal   = "external";
const char* const kUnresolved = "unresolved";

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

class Engine {
public:
    Engine(sqlite3* db, const LanguageRegistry& languages) : db_(db), languages_(languages) {}

    int load();
    void resolve();
    int write(Resolver::Stats* stats);

    int inheritsRounds() const { return inheritsRounds_; }
    bool inheritsCapHit() const { return inheritsCapHit_; }

private:
    // Name lookup
    Outcome resolveRef(const Ref& ref);
    Outcome lookup(const Ref& ref, const std::vector<std::string>& segs);
    Outcome receiverMember(const Ref& ref, const std::vector<std::string>& segs);
    Outcome member(const std::string& classId, const std::string& name, int depth,
                   std::unordered_set<std::string>& seen, bool& externalBase);
    Outcome choose(std::vector<Outcome>& candidates) const;
    Outcome fromBinding(const Binding& b, const std::string& ns, int depth);
    Outcome walk(Outcome o, const std::vector<std::string>& segs, std::size_t from,
                 const std::string& ns, int depth);
    Outcome exportOf(const std::string& fileId, const std::string& name,
                     const std::string& ns, int depth);
    Outcome starLookup(const std::string& scopeId, const std::string& name,
                       const std::string& ns, int depth);
    bool isExported(const std::string& fileId, const std::string& name,
                    const std::string& ns) const;

    // Modules
    const Import& module(const std::string& spec, const std::string& from);
    Outcome moduleOutcome(const std::string& spec, const std::string& from);

    const std::vector<std::string>& scopeChain(const std::string& scopeId,
                                               const std::string& fileId);
    const ILanguage* languageOf(const std::string& fileId) const;

    sqlite3* db_;
    const LanguageRegistry& languages_;
    const ILanguage* lang_ = nullptr;   // the language of the ref being resolved

    std::unordered_map<std::string, const ILanguage*> fileLanguage_;
    std::unordered_map<const ILanguage*, ProjectIndex> index_;

    std::unordered_map<std::string, Symbol> symbols_;
    std::unordered_map<std::string, std::string> children_;   // key(parent, name) -> symbol
    std::unordered_map<std::string, Scope> scopes_;
    // scope -> key(namespace, name) -> bindings in source order
    std::unordered_map<std::string,
                       std::unordered_map<std::string, std::vector<Binding>>> bindings_;
    std::unordered_map<std::string, std::vector<Binding>> stars_;   // scope -> import_all
    std::vector<Ref> refs_;

    std::unordered_map<std::string, Import> imports_;      // key(from, spec)
    std::vector<std::pair<std::string, std::string>> bindingSpecs_;  // (file, spec), for import_resolution
    std::unordered_map<std::string, std::vector<std::string>> chains_;
    std::unordered_set<std::string> exporting_;            // re-export cycle guard
    // exportOf answers, keyed like exporting_, with the number of hops the
    // answer took (height). Only complete answers are kept -- one cut short by
    // the cycle guard or kMaxDepth (truncated_ moved while it was computed)
    // depends on where it was asked from -- and one is reused only where its
    // hops still fit under kMaxDepth, so the result never depends on the order
    // refs are resolved in.
    struct Exported {
        Outcome out;
        int height = 0;
    };
    std::unordered_map<std::string, Exported> exported_;
    long truncated_ = 0;
    int reached_ = 0;   // deepest exportOf depth reached so far in the current subtree

    // class -> its bases, in source order: the resolved class id, or "" with
    // the status of a base that is not a project class.
    std::unordered_map<std::string,
                       std::vector<std::pair<std::string, const char*>>> bases_;

    std::vector<Outcome> outcomes_;   // parallel to refs_
    int inheritsRounds_ = 0;
    bool inheritsCapHit_ = false;
};

const ILanguage* Engine::languageOf(const std::string& fileId) const
{
    auto it = fileLanguage_.find(fileId);
    return it == fileLanguage_.end() ? nullptr : it->second;
}

int Engine::load()
{
    struct Query {
        const char* sql;
        void (*row)(Engine&, sqlite3_stmt*);
    };
    static const Query kQueries[] = {
        { "SELECT file_id, language FROM file;",
          [](Engine& e, sqlite3_stmt* s) {
              e.fileLanguage_[columnText(s, 0)] = e.languages_.find(columnText(s, 1));
          } },
        { "SELECT symbol_id, file_id, parent_id, kind, name, is_exported FROM symbol;",
          [](Engine& e, sqlite3_stmt* s) {
              Symbol sym;
              sym.file_id   = columnText(s, 1);
              sym.parent_id = columnText(s, 2);
              sym.kind      = columnText(s, 3);
              sym.name      = columnText(s, 4);
              sym.exported  = sqlite3_column_int(s, 5) != 0;
              const std::string id = columnText(s, 0);
              if (!sym.parent_id.empty()) e.children_[key(sym.parent_id, sym.name)] = id;
              e.symbols_.emplace(id, std::move(sym));
          } },
        { "SELECT scope_id, file_id, parent_scope_id, kind FROM scope;",
          [](Engine& e, sqlite3_stmt* s) {
              Scope sc;
              sc.file_id   = columnText(s, 1);
              sc.parent_id = columnText(s, 2);
              sc.kind      = columnText(s, 3);
              e.scopes_.emplace(columnText(s, 0), std::move(sc));
          } },
        { "SELECT scope_id, local_name, namespace, kind, file_id, target_id, module_spec,"
          "       imported_name, line, is_exported"
          "  FROM binding ORDER BY scope_id, line;",
          [](Engine& e, sqlite3_stmt* s) {
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
              if (!b.module_spec.empty()) e.bindingSpecs_.emplace_back(b.file_id, b.module_spec);
              if (b.kind == "import_all")
                  e.stars_[scope].push_back(std::move(b));
              else
                  e.bindings_[scope][key(columnText(s, 2), b.local_name)].push_back(std::move(b));
          } },
        { "SELECT ref_id, file_id, source_id, scope_id, kind, namespace, name_path, line"
          "  FROM ref ORDER BY ref_id;",
          [](Engine& e, sqlite3_stmt* s) {
              Ref r;
              r.id        = sqlite3_column_int64(s, 0);
              r.file_id   = columnText(s, 1);
              r.source_id = columnText(s, 2);
              r.scope_id  = columnText(s, 3);
              r.kind      = columnText(s, 4);
              r.ns        = columnText(s, 5);
              r.name_path = columnText(s, 6);
              r.line      = sqlite3_column_int(s, 7);
              e.refs_.push_back(std::move(r));
          } },
    };

    for (const Query& q : kQueries) {
        sqlite3_stmt* stmt = nullptr;
        int rc = sqlite3_prepare_v2(db_, q.sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) return rc;
        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) q.row(*this, stmt);
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE) return rc;
    }

    // One index per language, over that language's files only.
    std::unordered_map<const ILanguage*, std::unordered_set<std::string>> files;
    for (const auto& [fileId, lang] : fileLanguage_)
        if (lang) files[lang].insert(fileId);
    for (auto& [lang, ids] : files) index_.emplace(lang, lang->buildIndex(std::move(ids)));
    return SQLITE_OK;
}

// ── Modules ─────────────────────────────────────────────────────────────────

const Import& Engine::module(const std::string& spec, const std::string& from)
{
    const std::string k = key(from, spec);
    auto it = imports_.find(k);
    if (it != imports_.end()) return it->second;

    Import imp;
    const ILanguage* lang = languageOf(from);
    if (lang) {
        imp.file_id = lang->resolveModule(spec, from, index_.at(lang));
        imp.status = !imp.file_id.empty()        ? kResolved
                   : lang->isProjectModule(spec, from, index_.at(lang)) ? kUnresolved
                                                  : kExternal;
    }
    return imports_.emplace(k, std::move(imp)).first->second;
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
static int rank(const Outcome& o, const std::unordered_map<std::string, Symbol>& symbols)
{
    if (o.resolved()) {
        if (o.kind != Outcome::Symbol) return 4;
        auto it = symbols.find(o.id);
        if (it != symbols.end() && (isCallableKind(it->second.kind) || isTypeKind(it->second.kind)))
            return 4;
        return 2;
    }
    if (o.status == kBuiltin || o.status == kExternal) return 3;
    return 1;
}

// Several bindings of one name in one scope: take the best-ranked, the last in
// source order among equals. Only a choice between different answers is
// heuristic -- `import a.b` and `import a.c` both bind `a` to the same thing.
Outcome Engine::choose(std::vector<Outcome>& candidates) const
{
    std::size_t best = 0;
    int bestRank = -1;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const int r = rank(candidates[i], symbols_);
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
    const std::string guard = key(fileId, key(ns, name));
    auto memo = exported_.find(guard);
    if (memo != exported_.end() && depth + memo->second.height <= kMaxDepth) {
        reached_ = std::max(reached_, depth + memo->second.height);
        return memo->second.out;
    }
    if (!exporting_.insert(guard).second) {
        ++truncated_;
        return Outcome::failed(kUnresolved);
    }
    const long truncatedBefore = truncated_;
    const int reachedBefore = reached_;
    reached_ = depth;

    Outcome out;
    auto scope = bindings_.find(fileId);
    const std::vector<Binding>* found = nullptr;
    if (scope != bindings_.end()) {
        auto it = scope->second.find(key(ns, name));
        if (it != scope->second.end()) found = &it->second;
    }
    if (found) {
        std::vector<Outcome> candidates;
        for (const Binding& b : *found) candidates.push_back(fromBinding(b, ns, depth + 1));
        out = choose(candidates);
    } else {
        out = starLookup(fileId, name, ns, depth + 1);
    }
    exporting_.erase(guard);
    const int height = reached_ - depth;
    reached_ = std::max(reachedBefore, reached_);
    if (truncated_ == truncatedBefore) exported_.insert_or_assign(guard, Exported{out, height});
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
    auto it = stars_.find(scopeId);
    if (it == stars_.end()) return none;
    for (auto b = it->second.rbegin(); b != it->second.rend(); ++b) {
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
                        const std::string& ns) const
{
    auto scope = bindings_.find(fileId);
    if (scope == bindings_.end()) return true;
    auto it = scope->second.find(key(ns, name));
    if (it == scope->second.end()) return true;
    for (const Binding& b : it->second)
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

        auto sym = symbols_.find(o.id);
        if (sym == symbols_.end() || !isTypeKind(sym->second.kind))
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
    const std::string nsKey = key(ref.ns, head);
    bool maybeExternal = false;

    for (const std::string& scopeId : scopeChain(ref.scope_id, ref.file_id)) {
        if (scopeId == kBuiltinScope) {
            if (lang_->isBuiltin(head)) return Outcome::failed(kBuiltin);
            break;
        }
        auto scope = bindings_.find(scopeId);
        if (scope != bindings_.end()) {
            auto it = scope->second.find(nsKey);
            if (it != scope->second.end()) {
                std::vector<Outcome> candidates;
                for (const Binding& b : it->second) {
                    Outcome o = walk(fromBinding(b, ref.ns, 0), segs, 1, ref.ns, 0);
                    // A class cannot inherit from, or decorate, itself: in
                    // `from m import A` + `class A(A)` the base is the import.
                    if (ref.kind != "CALLS" && o.kind == Outcome::Symbol && o.id == ref.source_id)
                        continue;
                    candidates.push_back(std::move(o));
                }
                if (!candidates.empty()) return choose(candidates);
            }
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
    auto child = children_.find(key(classId, name));
    if (child != children_.end()) return Outcome::symbol(child->second);
    if (depth >= kMaxBaseDepth || !seen.insert(classId).second) return {};

    auto bases = bases_.find(classId);
    if (bases == bases_.end()) return {};
    for (const auto& [baseId, status] : bases->second) {
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
    const std::string nsKey = key(ref.ns, segs[0]);
    std::string classId;
    for (const std::string& scopeId : scopeChain(ref.scope_id, ref.file_id)) {
        auto scope = bindings_.find(scopeId);
        if (scope == bindings_.end() || !scope->second.count(nsKey)) continue;
        // A function scope's id is its symbol's.
        auto sym = symbols_.find(scopeId);
        if (sym != symbols_.end() && sym->second.kind == "method") classId = sym->second.parent_id;
        break;
    }
    auto cls = symbols_.find(classId);
    if (cls == symbols_.end() || !isTypeKind(cls->second.kind)) return {};

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
        auto sym = symbols_.find(o.id);
        const ILanguage* classLang =
            sym != symbols_.end() && isClassKind(sym->second.kind) ? languageOf(sym->second.file_id)
                                                                   : nullptr;
        if (classLang) {
            std::unordered_set<std::string> seen;
            bool externalBase = false;
            Outcome ctor = member(o.id, classLang->constructorName(sym->second.name), 0, seen,
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

void Engine::resolve()
{
    // 1. Imports: every module spec a binding names.
    for (const auto& [file, spec] : bindingSpecs_) module(spec, file);
    outcomes_.resize(refs_.size());

    // 2. INHERITS, to a fixpoint. A base may itself depend on bases -- in
    // `class Y(Derived.Inner)` Inner may be inherited by Derived -- so one pass
    // in ref order would answer differently depending on which class came
    // first. Each round resolves every base against the bases the previous
    // round found, never against a half-built set, and rounds repeat until
    // nothing changes. A class's bases are kept in source order (line, then
    // position on the line), which is the order the base search walks them.
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
    auto same = [](const Outcome& a, const Outcome& b) {
        return a.status == b.status && a.kind == b.kind && a.id == b.id && a.heuristic == b.heuristic;
    };
    inheritsCapHit_ = true;
    for (int round = 0; round < kMaxInheritsRounds; ++round) {
        inheritsRounds_ = round + 1;
        bool changed = false;
        std::unordered_map<std::string,
                           std::vector<std::pair<std::string, const char*>>> next;
        for (std::size_t i : inherits) {
            const Ref& ref = refs_[i];
            lang_ = languageOf(ref.file_id);
            Outcome o = resolveRef(ref);
            if (round == 0 || !same(o, outcomes_[i])) changed = true;
            outcomes_[i] = std::move(o);

            std::string base;
            const Outcome& r = outcomes_[i];
            if (r.resolved() && r.kind == Outcome::Symbol) {
                auto sym = symbols_.find(r.id);
                if (sym != symbols_.end() && isClassKind(sym->second.kind)) base = r.id;
            }
            next[ref.source_id].emplace_back(std::move(base), r.status);
        }
        bases_ = std::move(next);
        if (!changed) {
            inheritsCapHit_ = false;
            break;
        }
    }

    // 3. Everything else, against the final bases.
    for (std::size_t i = 0; i < refs_.size(); ++i) {
        const Ref& ref = refs_[i];
        if (ref.kind == "INHERITS") continue;
        lang_ = languageOf(ref.file_id);
        if (!lang_) continue;   // stays 'pending'
        outcomes_[i] = resolveRef(ref);
    }
}

// ── Writing back ────────────────────────────────────────────────────────────

int Engine::write(Resolver::Stats* stats)
{
    struct Stmt {
        sqlite3_stmt* s = nullptr;
        ~Stmt() { sqlite3_finalize(s); }
    };
    auto exec = [&](const char* sql) { return sqlite3_exec(db_, sql, nullptr, nullptr, nullptr); };
    auto done = [](int rc) { return rc == SQLITE_DONE ? SQLITE_OK : rc; };
    // SQLITE_STATIC: v must outlive the statement's next step.
    auto text = [](sqlite3_stmt* s, int i, const std::string& v) {
        if (v.empty()) sqlite3_bind_null(s, i);
        else sqlite3_bind_text(s, i, v.c_str(), -1, SQLITE_STATIC);
    };

    int rc = exec("DELETE FROM import_resolution;");
    if (rc != SQLITE_OK) return rc;
    {
        Stmt ins;
        rc = sqlite3_prepare_v2(db_,
            "INSERT OR IGNORE INTO import_resolution(file_id, module_spec, resolved_file_id, status)"
            " VALUES(?,?,?,?);", -1, &ins.s, nullptr);
        if (rc != SQLITE_OK) return rc;
        for (const auto& [file, spec] : bindingSpecs_) {
            const Import& imp = module(spec, file);
            if (!languageOf(file)) continue;
            sqlite3_reset(ins.s);
            text(ins.s, 1, file);
            text(ins.s, 2, spec);
            text(ins.s, 3, imp.file_id);
            sqlite3_bind_text(ins.s, 4, imp.status, -1, SQLITE_STATIC);
            rc = done(sqlite3_step(ins.s));
            if (rc != SQLITE_OK) return rc;
            if (stats && sqlite3_changes(db_) > 0) ++stats->imports;
        }
    }

    rc = exec("DELETE FROM link WHERE link_type IN ('CALLS', 'INHERITS', 'DECORATES');");
    if (rc != SQLITE_OK) return rc;

    Stmt upd, link;
    rc = sqlite3_prepare_v2(db_,
        "UPDATE ref SET resolved_id = ?, status = ?, confidence = ?, resolver = ?"
        " WHERE ref_id = ?;", -1, &upd.s, nullptr);
    if (rc != SQLITE_OK) return rc;
    rc = sqlite3_prepare_v2(db_,
        "INSERT OR IGNORE INTO link(source_id, target_id, link_type) VALUES(?,?,?);",
        -1, &link.s, nullptr);
    if (rc != SQLITE_OK) return rc;

    for (std::size_t i = 0; i < refs_.size(); ++i) {
        const Ref& ref = refs_[i];
        const Outcome& o = outcomes_[i];
        if (!o.found()) continue;   // no language plugin: left pending

        const bool resolved = o.resolved();
        sqlite3_reset(upd.s);
        if (resolved) text(upd.s, 1, o.id);
        else sqlite3_bind_null(upd.s, 1);
        sqlite3_bind_text(upd.s, 2, o.status, -1, SQLITE_STATIC);
        if (o.status == kUnresolved)
            sqlite3_bind_null(upd.s, 3);
        else
            sqlite3_bind_text(upd.s, 3, o.heuristic ? "heuristic" : "exact", -1, SQLITE_STATIC);
        sqlite3_bind_text(upd.s, 4, Resolver::kName, -1, SQLITE_STATIC);
        sqlite3_bind_int64(upd.s, 5, ref.id);
        rc = done(sqlite3_step(upd.s));
        if (rc != SQLITE_OK) return rc;
        if (stats) ++stats->status[o.status];

        // Links only between project symbols, and only of the kind each link
        // type means: CALLS to a function or method, INHERITS to a class.
        if (!resolved || o.kind != Outcome::Symbol) continue;
        auto sym = symbols_.find(o.id);
        if (sym == symbols_.end()) continue;
        const std::string& kind = sym->second.kind;
        const std::string* source = &ref.source_id;
        const std::string* target = &o.id;
        if (ref.kind == "CALLS") {
            if (!isCallableKind(kind)) continue;
        } else if (ref.kind == "INHERITS") {
            if (!isClassKind(kind)) continue;
        } else if (ref.kind == "DECORATES") {
            // The ref runs decorated -> decorator; the link decorator -> decorated.
            // Any kind of decorator symbol is linked, a variable included, on purpose.
            std::swap(source, target);
        } else {
            continue;
        }
        sqlite3_reset(link.s);
        text(link.s, 1, *source);
        text(link.s, 2, *target);
        text(link.s, 3, ref.kind);
        rc = done(sqlite3_step(link.s));
        if (rc != SQLITE_OK) return rc;
        if (stats && sqlite3_changes(db_) > 0) ++stats->links[ref.kind];
    }
    return SQLITE_OK;
}

}  // namespace

bool Resolver::needsRescan(sqlite3* db)
{
    // Files without a current scan stamp (db.h): written by a Telescode whose
    // parser did not yet emit everything the resolver relies on. An empty
    // database has nothing to lose. No meta table at all means no stamp; no
    // file table means an empty database.
    const std::string sql =
        std::string("SELECT EXISTS(SELECT 1 FROM file) AND"
                    " COALESCE((SELECT CAST(value AS INTEGER) FROM meta WHERE key = '") +
        kScanFormatKey + "'), 0) < " + std::to_string(kScanFormat) + ";";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        stmt = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT EXISTS(SELECT 1 FROM file);", -1, &stmt, nullptr) != SQLITE_OK)
            return false;
    }
    const bool stale = sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) != 0;
    sqlite3_finalize(stmt);
    return stale;
}

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
    if (needsRescan(db)) return kNeedsRescan;
    const auto t0 = std::chrono::steady_clock::now();
    Engine engine(db, languages);
    int rc = engine.load();
    if (rc != SQLITE_OK) return rc;
    engine.resolve();
    if (stats) {
        stats->inheritsRounds = engine.inheritsRounds();
        stats->inheritsCapHit = engine.inheritsCapHit();
    }
    rc = engine.write(stats);
    if (stats)
        stats->ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
    return rc;
}

int Resolver::run(sqlite3* db, Stats* stats, const LanguageRegistry& languages)
{
    int rc = sqlite3_exec(db, "BEGIN;", nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK) return rc;
    rc = resolveAll(db, stats, languages);
    if (rc == SQLITE_OK) rc = sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK) sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
    return rc;
}
