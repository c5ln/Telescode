#pragma once

#include <string>
#include <vector>

struct FileEntity {
    std::string file_id;   // relative path from repo root
    std::string file_name; // basename
    std::string language;
    std::string module_name; // dotted module name derived from the path; "" if none
    int raw_loc     = 0;   // total line count (including blanks and comments)
    int logical_loc = 0;   // non-blank, non-comment line count
    int is_generated = 0;  // 1 if vendored/generated, 0 otherwise
    // zero-function files must not produce nulls; these six fields derive
    // from the function list and could otherwise be left unset
    int    max_cyclomatic_complexity = 0;
    double avg_cyclomatic_complexity = 0.0;
    int    max_block_depth           = 0;
    double avg_block_depth           = 0.0;
    int    max_function_loc          = 0;
    double avg_function_loc          = 0.0;
};

struct ClassEntity {
    std::string class_id;   // file_id::ClassName
    std::string file_id;
    std::string class_name;
    int start_line;         // 0-indexed
    int end_line;           // 0-indexed
};

struct BaseClassEntity {
    std::string class_id;
    std::string base_class_name;
    int ordinal; // 0-indexed
};

struct FunctionEntity {
    std::string function_id;
    std::string file_id;
    std::string class_id;
    std::string function_name;
    int nesting_depth;
    int is_async;              // 0 or 1
    int cyclomatic_complexity; // base 1 + branch count
    int max_block_depth;       // deepest control-flow nesting inside this function
    int loc;                   // raw line count (end_line - start_line + 1)
    int start_line;            // 0-indexed
    int end_line;              // 0-indexed
};

struct ParamEntity {
    std::string function_id;
    std::string param_name;
    int ordinal; // 0-indexed
};

struct FieldEntity {
    std::string class_id;
    std::string field_name;
    char access; // '+' public  '-' private  '#' protected
};

// ── Name resolution input ──────────────────────────────────────────────────────
// What the parser extracts so a later pass can resolve names across files.
// Lines are 0-indexed, like start_line/end_line above.

struct SymbolEntity {
    std::string symbol_id;  // class_id / function_id, or <parent>::<name> for fields and variables
    std::string file_id;
    std::string parent_id;  // enclosing symbol; "" at module level
    std::string kind;       // class, function, method, field, variable
    std::string name;
    std::string ns = "value";
    int is_exported = 1;
    int start_line;
    int end_line;
};

struct ScopeEntity {
    std::string scope_id;        // file_id for the file scope, else owner_id
    std::string file_id;
    std::string parent_scope_id; // lexically enclosing scope; "" for the file scope
    std::string kind;            // file, class, function
    std::string owner_id;        // the class/function symbol; file_id for the file scope
    int start_line;
    int end_line;
};

struct BindingEntity {
    std::string scope_id;
    std::string local_name;
    std::string ns = "value";
    std::string kind;          // def, import_module, import_symbol, import_all
    std::string file_id;
    std::string target_id;     // def: the symbol, "" for a local with no symbol row
    std::string module_spec;   // imports: the module as written ("a.b", ".", "..pkg")
    std::string imported_name; // import_symbol: the name taken from module_spec
    int line;
    // Module-level bindings only: 0 if the module's __all__ leaves the name out,
    // so a wildcard import of the module does not see it. Like symbol.is_exported.
    int is_exported = 1;
};

struct RefEntity {
    std::string file_id;
    std::string source_id;  // the entity the link would start from
    std::string scope_id;   // scope the name is looked up in
    std::string kind;       // CALLS, INHERITS, DECORATES
    std::string ns = "value";
    std::string name_path;  // dotted identifier chain, e.g. "self.run"
    std::string raw_text;   // the expression as written
    int line;
};

struct ParseResult {
    FileEntity file;
    std::vector<ClassEntity> classes;
    std::vector<BaseClassEntity> base_classes;
    std::vector<FunctionEntity> functions;
    std::vector<ParamEntity> params;
    std::vector<FieldEntity> fields;
    std::vector<SymbolEntity> symbols;
    std::vector<ScopeEntity> scopes;
    std::vector<BindingEntity> bindings;
    std::vector<RefEntity> refs;
    // Every module spec an import statement of the file names, once each, in
    // source order. Not read off the bindings: one binding can stand for
    // several imports (`import pkg.h, pkg.i` binds only `pkg`).
    std::vector<std::string> import_specs;
};
