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

// One rung of a lexical scope chain, as stored in the scope table.
struct LexicalScope {
    std::string id;
    std::string kind; // scope.kind: "file", "namespace", "class", "function", "block"
};

// Stands for the language's builtin scope in a parentScopes() result. It has no
// scope row; a name looked up there is answered by isBuiltin().
inline constexpr const char* kBuiltinScope = "<builtins>";

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

    // The scopes a name lookup climbs through after the scope it starts in.
    // `chain` is the lexical chain from that scope outwards: chain[0] is where
    // the reference sits, each next entry its parent_scope_id, chain.back() the
    // file scope. Returns the ids to search in order, innermost first, ending
    // with kBuiltinScope. chain[0] itself is not included -- it is always
    // searched first. Python, for instance, leaves out enclosing class bodies.
    virtual std::vector<std::string> parentScopes(
        const std::vector<LexicalScope>& chain) const = 0;

    // ── Module specs, as the resolver walks them ─────────────────────────────

    // True if `spec` can only name a project module, whether or not that module
    // exists: it is anchored on the importing file, or the package it starts
    // from is a project one. When resolveModule() finds nothing for such a spec
    // the import is broken ('unresolved'), not external.
    virtual bool isProjectModule(const std::string& spec,
                                 const std::string& from_file_id,
                                 const ProjectIndex& index) const = 0;

    // The spec of submodule `name` of module `spec`: what an attribute access on
    // a module, or `from spec import name`, falls back to when the module itself
    // binds no `name`.
    virtual std::string submoduleSpec(const std::string& spec,
                                      const std::string& name) const = 0;

    // The module an import_module binding's local name stands for. Not always
    // binding.module_spec: an import may bind only the outermost package of the
    // module it loads.
    virtual std::string boundModuleSpec(const std::string& local_name,
                                        const std::string& module_spec) const = 0;

    // ── Classes ──────────────────────────────────────────────────────────────

    // True if `name`, inside a method, denotes the object the method was
    // called on, so `name.x` is looked up on the method's class.
    virtual bool isReceiver(const std::string& name) const = 0;

    // Name of the method a call to the class itself runs.
    virtual std::string constructorName(const std::string& class_name) const = 0;
};
