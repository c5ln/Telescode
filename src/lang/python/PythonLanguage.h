#pragma once

#include "lang/ILanguage.h"

#include <string>
#include <unordered_set>
#include <vector>

// ── Python import resolution ────────────────────────────────────────────────
// These rules decide the entire IMPORTS edge set, and
// bench/features/extract.py has to reproduce them exactly -- if the two drift,
// in_deg/out_deg describe a different graph than the one PageRank ran on.
class PythonLanguage : public ILanguage {
public:
    const char* name() const override { return "python"; }

    bool handlesFile(const std::string& file_id) const override;

    // file_ids plus inferPackageRoots(file_ids).
    ProjectIndex buildIndex(std::unordered_set<std::string> file_ids) const override;

    // `spec` is recorded verbatim by the parser, so it is either
    //   absolute: "pkg.core.dataset"
    //   relative: ".mod", "..pkg.mod", or bare dots (".", "..") for `from . import x`
    // Relative specs resolve against `from_file_id`'s directory: one leading dot
    // is the current package, each further dot goes up one more level. Going
    // above the repo root is unresolvable, not an error.
    std::string resolveModule(const std::string& spec,
                              const std::string& from_file_id,
                              const ProjectIndex& index) const override;

    bool isBuiltin(const std::string& name) const override;

    // Every enclosing scope except class bodies: a method cannot see its class's
    // names unqualified, and neither can a class nested in another class. The
    // starting scope may itself be a class body; it is searched as usual.
    std::vector<std::string> parentScopes(
        const std::vector<LexicalScope>& chain) const override;

    // Relative (leading dots), or absolute with a top-level package or module
    // that is a project file under some layout root: `pkg.gone` when pkg/ is in
    // the project, but not `numpy.linalg`. A project module that shadows an
    // outside one by name (a root-level `json.py`) claims its submodules too.
    bool isProjectModule(const std::string& spec,
                         const std::string& from_file_id,
                         const ProjectIndex& index) const override;

    // "a.b" + "c" -> "a.b.c"; bare dots take no separator: "." + "c" -> ".c".
    std::string submoduleSpec(const std::string& spec,
                              const std::string& name) const override;

    // `import a.b.c` binds `a` to the package a; `import a.b as x` binds `x` to
    // a.b itself. The binding row does not record whether an alias was written,
    // so a local name equal to the spec's first component is taken as the
    // unaliased form (`import a.b as a` reads as `import a.b`).
    std::string boundModuleSpec(const std::string& local_name,
                                const std::string& module_spec) const override;

    // `self` and `cls`, by convention: Python itself would take the first
    // parameter, whatever its name.
    bool isReceiver(const std::string& name) const override;

    // "__init__".
    std::string constructorName(const std::string& class_name) const override;

    // Directory prefixes under which a top-level package may live, inferred
    // from where __init__.py files sit: if `D/P/__init__.py` exists and
    // `D/__init__.py` does not, then P is a top-level package and `D/` is a
    // layout root. Always contains "" (repo root). Sorted shortest-first, so a
    // root-layout repo resolves exactly as it did before this existed.
    // max_depth caps how deep a root may be; test-fixture trees are full of
    // package-looking directories and would otherwise become resolution candidates.
    static std::vector<std::string> inferPackageRoots(
        const std::unordered_set<std::string>& file_ids, int max_depth = 1);
};
