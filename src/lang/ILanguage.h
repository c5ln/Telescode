#pragma once

#include <string>
#include <unordered_set>
#include <vector>

// ── Language plugin boundary ────────────────────────────────────────────────
// Everything that differs between source languages when turning a reference
// into a project symbol lives behind ILanguage; the resolver core only talks to
// this interface and never names a language itself.
//
// Per-file extraction stays in the parser (IParser / ParserRegistry under
// src/parser/) for now. Once the parser emits scopes and bindings, each
// ILanguage will be paired with its IParser by the same file extension
// (handlesFile() <-> ParserRegistry::registerParser), rather than the plugin
// growing its own extract().

// Project-wide facts a language needs to resolve module specs, computed once
// per scan instead of once per lookup.
struct ProjectIndex {
    // Every file_id in the project (repo-relative, '/'-separated).
    std::unordered_set<std::string> file_ids;
    // Directory prefixes ("" or "src/") an absolute module spec is anchored on,
    // shortest first. What counts as a root is language-defined.
    std::vector<std::string> package_roots;
};

class ILanguage {
public:
    virtual ~ILanguage() = default;

    // Language id, as stored in file.language ("python").
    virtual const char* name() const = 0;

    // True if a file_id belongs to this language (by extension).
    virtual bool handlesFile(const std::string& file_id) const = 0;

    // Build the index resolveModule() works against.
    virtual ProjectIndex buildIndex(std::unordered_set<std::string> file_ids) const = 0;

    // Resolve a module spec as written in `from_file_id` to a project file_id,
    // or "" if it names no project file (external, or unresolvable).
    virtual std::string resolveModule(const std::string& spec,
                                      const std::string& from_file_id,
                                      const ProjectIndex& index) const = 0;

    // True if `name` is visible in the language's outermost (builtin) scope --
    // the last rung of the scope ladder, consulted after every project scope.
    virtual bool isBuiltin(const std::string& name) const = 0;

    // parentScopes() -- which enclosing scopes a lookup may climb through (e.g.
    // Python skips class bodies) -- is added together with the scope table.
};
