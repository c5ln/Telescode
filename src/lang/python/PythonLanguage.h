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
