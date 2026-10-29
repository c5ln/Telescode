#include "PythonParser.h"
#include <tree_sitter/api.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <cstring>
#include <algorithm>
#include <stdexcept>
#include <unordered_map>
#include <set>
#include <tuple>
#include <iostream>
#include <memory>
#include <thread>
#include <atomic>

extern "C" const TSLanguage* tree_sitter_python();

namespace fs = std::filesystem;


// helper 함수들

static std::string nodeText(TSNode node, const std::string& src) {
    uint32_t start = ts_node_start_byte(node);
    uint32_t end   = ts_node_end_byte(node);
    if (start > end || end > src.size()) return {};
    return src.substr(start, end - start);
}

static bool nodeIsNull(TSNode node) {
    return ts_node_is_null(node);
}

// Find the first named child with a given field name
static TSNode childByFieldName(TSNode node, const char* fieldName) {
    return ts_node_child_by_field_name(node, fieldName, (uint32_t)std::strlen(fieldName));
}

// Return the type string of a node
static std::string nodeType(TSNode node) {
    if (nodeIsNull(node)) return "";
    const char* t = ts_node_type(node);
    return t ? t : "";
}

// context passed through recursive traversal
struct TraversalContext {
    const std::string& src;
    const std::string& fileId;
    ParseResult& result;

    // Stack of enclosing entity IDs and their types ("class" / "function")
    struct Frame {
        std::string id;   // class_id or function_id
        std::string kind; // "class" or "function"
        // Names this scope declares `global` / `nonlocal`: assigning them binds
        // in another scope, so they get no binding here.
        std::set<std::string> declared;
    };
    std::vector<Frame> scopeStack;

    // (scope_id, local_name, line) already bound -- binding's PK. A name bound
    // twice on one line (`a = a = 1`) keeps its first row.
    std::set<std::string> bindingKeys;

    int nestingDepth() const {
        return (int)scopeStack.size();
    }

    std::string parentId() const {
        if (scopeStack.empty()) return fileId;
        return scopeStack.back().id;
    }

    bool parentIsClass() const {
        return !scopeStack.empty() && scopeStack.back().kind == "class";
    }
};

// countCyclomaticComplexity 
// Count branch decision points inside one function's body.
// Returns the count to ADD to the base complexity of 1.
// Does NOT descend into nested function_definition or lambda nodes.

static int countCyclomaticComplexity(TSNode node) {
    if (nodeIsNull(node)) return 0;

    std::string type = nodeType(node);

    // Nested functions/lambdas own their own CC; don't bleed into parent.
    if (type == "function_definition" || type == "lambda") return 0;

    int count = 0;

    // Each of these node types represents one additional decision point.
    if (type == "if_statement"          ||  // if ...
        type == "elif_clause"           ||  // elif ...
        type == "for_statement"         ||  // for ...
        type == "while_statement"       ||  // while ...
        type == "case_clause"           ||  // match arm
        type == "except_clause"         ||  // except ...
        type == "conditional_expression"||  // x if cond else y
        type == "boolean_operator") {       // ... and/or ...
        count += 1;
    }

    uint32_t n = ts_node_child_count(node);
    for (uint32_t i = 0; i < n; ++i)
        count += countCyclomaticComplexity(ts_node_child(node, i));

    return count;
}

// computeMaxBlockDepth / computeMaxBlockDepthClauses 
// Mutually recursive: forward-declare the clause helper first.
static void computeMaxBlockDepthClauses(TSNode node, int startDepth, int& maxDepth);

// Track the deepest control-flow nesting inside one function body.
// `depth` is the depth *entering* this node; caller passes 0 for the body root.
// Does NOT descend into nested function_definition or lambda nodes.

static void computeMaxBlockDepth(TSNode node, int depth, int& maxDepth) {
    if (nodeIsNull(node)) return;

    std::string type = nodeType(node);

    // Nested definitions own their own depth — stop here.
    if (type == "function_definition" || type == "lambda") return;

    // These constructs each add one level. elif/else/except/finally/case_clause
    // are children of these nodes and share the parent's incremented depth.
    if (type == "if_statement"    ||
        type == "for_statement"   ||
        type == "while_statement" ||
        type == "try_statement"   ||
        type == "with_statement"  ||
        type == "match_statement") {
        ++depth;
        if (depth > maxDepth) maxDepth = depth;
    }

    // Comprehension clauses are AST siblings but semantically nested.
    // Hand off to the clause helper so each for/if accumulates depth in order.
    if (type == "list_comprehension"       ||
        type == "set_comprehension"        ||
        type == "dictionary_comprehension" ||
        type == "generator_expression") {
        computeMaxBlockDepthClauses(node, depth, maxDepth);
        return;
    }

    uint32_t n = ts_node_child_count(node);
    for (uint32_t i = 0; i < n; ++i)
        computeMaxBlockDepth(ts_node_child(node, i), depth, maxDepth);
}

// Process children of a comprehension node. for_in_clause and if_clause are
// flat siblings in the AST but must be counted as if sequentially nested, so
// clauseDepth accumulates across them. The body expression is not a block and
// does not contribute depth, so it is processed at the pre-clause startDepth.
static void computeMaxBlockDepthClauses(TSNode node, int startDepth, int& maxDepth) {
    int clauseDepth = startDepth;
    uint32_t n = ts_node_child_count(node);
    for (uint32_t i = 0; i < n; ++i) {
        TSNode child = ts_node_child(node, i);
        if (nodeIsNull(child)) continue;
        std::string ctype = nodeType(child);
        if (ctype == "for_in_clause" || ctype == "if_clause") {
            ++clauseDepth;
            if (clauseDepth > maxDepth) maxDepth = clauseDepth;
            // Recurse into clause internals (e.g. nested comprehension in iterable).
            uint32_t m = ts_node_child_count(child);
            for (uint32_t j = 0; j < m; ++j)
                computeMaxBlockDepth(ts_node_child(child, j), clauseDepth, maxDepth);
        } else {
            // Body / key / value expression — not a block, use outer depth.
            computeMaxBlockDepth(child, startDepth, maxDepth);
        }
    }
}

// ── name-resolution extraction helpers ──────────────────────────────────────

static int nodeLine(TSNode node) {
    return (int)ts_node_start_point(node).row;
}

// `foo`, `a.b.c`, `self.run` -> the dotted chain; anything else (a call result,
// a subscript, a parenthesised expression anywhere in the chain) -> false.
// Built from the identifiers rather than the node text so that `a . b` and a
// chain broken over lines still normalise to "a.b".
static bool identifierChain(TSNode node, const std::string& src, std::string& out) {
    std::string type = nodeType(node);
    if (type == "identifier") {
        out = nodeText(node, src);
        return !out.empty();
    }
    if (type == "attribute") {
        TSNode obj  = childByFieldName(node, "object");
        TSNode attr = childByFieldName(node, "attribute");
        if (nodeIsNull(obj) || nodeIsNull(attr)) return false;
        std::string head;
        if (!identifierChain(obj, src, head)) return false;
        out = head + "." + nodeText(attr, src);
        return true;
    }
    return false;
}

static void addRef(ParseResult& result, const std::string& fileId,
                   const std::string& sourceId, const std::string& scopeId,
                   const char* kind, TSNode exprNode, const std::string& src) {
    std::string path;
    if (!identifierChain(exprNode, src, path)) return;
    RefEntity r;
    r.file_id   = fileId;
    r.source_id = sourceId;
    r.scope_id  = scopeId;
    r.kind      = kind;
    r.name_path = path;
    r.raw_text  = nodeText(exprNode, src);
    r.line      = nodeLine(exprNode);
    result.refs.push_back(std::move(r));
}

static void addSymbol(TraversalContext& ctx, const std::string& id,
                      const char* kind, const std::string& name,
                      int startLine, int endLine) {
    SymbolEntity se;
    se.symbol_id  = id;
    se.file_id    = ctx.fileId;
    se.parent_id  = ctx.scopeStack.empty() ? std::string() : ctx.scopeStack.back().id;
    se.kind       = kind;
    se.name       = name;
    se.start_line = startLine;
    se.end_line   = endLine;
    ctx.result.symbols.push_back(std::move(se));
}

static void addScope(TraversalContext& ctx, const std::string& id, const char* kind,
                     int startLine, int endLine) {
    ScopeEntity sc;
    sc.scope_id        = id;
    sc.file_id         = ctx.fileId;
    sc.parent_scope_id = ctx.parentId();  // called before the frame is pushed
    sc.kind            = kind;
    sc.owner_id        = id;
    sc.start_line      = startLine;
    sc.end_line        = endLine;
    ctx.result.scopes.push_back(std::move(sc));
}

// Bind `name` in the current scope.
// Every binding is kept, in source order: `try: from .util import helper /
// except ImportError: helper = None` gives `helper` two rows, and so does
// `import a.b` followed by `import a.c` for `a`. No winner is picked here -- the
// resolver prefers the binding that resolves to a project symbol and marks
// that choice confidence = 'heuristic'. Only a repeat on the same line (same
// PK) is dropped, keeping the first.
static void addBinding(TraversalContext& ctx, const std::string& name,
                       const char* kind, const std::string& targetId, int line,
                       const std::string& moduleSpec = "",
                       const std::string& importedName = "") {
    if (name.empty()) return;
    if (!ctx.scopeStack.empty() && ctx.scopeStack.back().declared.count(name)) return;

    BindingEntity b;
    b.scope_id      = ctx.parentId();
    b.local_name    = name;
    b.kind          = kind;
    b.file_id       = ctx.fileId;
    b.target_id     = targetId;
    b.module_spec   = moduleSpec;
    b.imported_name = importedName;
    b.line          = line;

    std::string key = b.scope_id + '\n' + name + '\n' + std::to_string(line);
    if (ctx.bindingKeys.insert(std::move(key)).second)
        ctx.result.bindings.push_back(std::move(b));
}

// Bind every name an assignment target introduces: `x`, `a, (b, *c)`, `[d, e]`.
// Attribute and subscript targets (`self.x`, `d[k]`) bind nothing. At module
// level each name is also a `variable` symbol, which the binding points at; in
// a function or class body it is a plain local with no symbol row.
static void bindTargets(TSNode node, TraversalContext& ctx) {
    if (nodeIsNull(node)) return;
    std::string type = nodeType(node);
    if (type == "identifier") {
        std::string name = nodeText(node, ctx.src);
        int line = nodeLine(node);
        if (ctx.scopeStack.empty()) {
            std::string id = ctx.fileId + "::" + name;
            addSymbol(ctx, id, "variable", name, line, line);
            addBinding(ctx, name, "def", id, line);
        } else {
            addBinding(ctx, name, "def", "", line);
        }
        return;
    }
    if (type == "pattern_list" || type == "tuple_pattern" || type == "list_pattern" ||
        type == "tuple" || type == "list" || type == "expression_list" ||
        type == "parenthesized_expression" || type == "list_splat_pattern" ||
        type == "list_splat" || type == "as_pattern_target") {
        uint32_t n = ts_node_child_count(node);
        for (uint32_t i = 0; i < n; ++i) {
            TSNode c = ts_node_child(node, i);
            if (ts_node_is_named(c)) bindTargets(c, ctx);
        }
    }
}

// Names a function or class body declares global/nonlocal. Python applies the
// declaration to the whole block wherever it appears, so it is collected
// before the body is walked. Nested definitions have their own blocks.
static void collectDeclared(TSNode node, const std::string& src,
                            std::set<std::string>& out) {
    if (nodeIsNull(node)) return;
    std::string type = nodeType(node);
    if (type == "function_definition" || type == "class_definition" || type == "lambda")
        return;
    if (type == "global_statement" || type == "nonlocal_statement") {
        uint32_t n = ts_node_child_count(node);
        for (uint32_t i = 0; i < n; ++i) {
            TSNode c = ts_node_child(node, i);
            if (nodeType(c) == "identifier") out.insert(nodeText(c, src));
        }
        return;
    }
    uint32_t n = ts_node_child_count(node);
    for (uint32_t i = 0; i < n; ++i)
        collectDeclared(ts_node_child(node, i), src, out);
}

// 전방 선언
static void traverseNode(TSNode node, TraversalContext& ctx);
static void handleClassDef(TSNode node, TraversalContext& ctx);
static void handleFunctionDef(TSNode node, TraversalContext& ctx, bool isAsync);
static void handleImport(TSNode node, TraversalContext& ctx);
static void handleImportFrom(TSNode node, TraversalContext& ctx);
static void handleDecoratedDef(TSNode node, TraversalContext& ctx);
static void collectCalls(TSNode node, const std::string& funcId,
                         const std::string& scopeId, const std::string& fileId,
                         const std::string& src, ParseResult& result);
static void collectFields(TSNode node, const std::string& classId,
                          const std::string& fileId,
                          const std::string& src, ParseResult& result);

// traversal 

static void traverseNode(TSNode node, TraversalContext& ctx) {
    if (nodeIsNull(node)) return;
    if (ts_node_has_error(node) && nodeType(node) == "ERROR") {
        // Skip ERROR nodes entirely to avoid garbage data
        return;
    }

    std::string type = nodeType(node);

    if (type == "class_definition") {
        handleClassDef(node, ctx);
        return; // handleClassDef recurses into children itself
    }
    if (type == "function_definition") {
        // Detect async: check for an "async" keyword child (unnamed node)
        bool isAsync = false;
        uint32_t childCount = ts_node_child_count(node);
        for (uint32_t i = 0; i < childCount; ++i) {
            TSNode c = ts_node_child(node, i);
            if (!nodeIsNull(c) && nodeType(c) == "async") {
                isAsync = true;
                break;
            }
        }
        handleFunctionDef(node, ctx, isAsync);
        return;
    }
    if (type == "import_statement") {
        handleImport(node, ctx);
        // Don't return — imports have no interesting children to descend into
    }
    if (type == "import_from_statement") {
        handleImportFrom(node, ctx);
    }
    if (type == "decorated_definition") {
        handleDecoratedDef(node, ctx);
        return; // decorated_definition recurses internally
    }

    // Name bindings. Comprehension variables (for_in_clause) are local to the
    // comprehension and bind nothing here; a walrus inside one binds in the
    // enclosing scope, which is what visiting named_expression anywhere gives.
    if (type == "lambda") {
        return; // its own scope; holds no definitions or imports
    }
    if (type == "assignment" || type == "augmented_assignment" || type == "for_statement") {
        bindTargets(childByFieldName(node, "left"), ctx);
    } else if (type == "as_pattern") {
        // with ... as x / except E as x / case ... as x
        bindTargets(childByFieldName(node, "alias"), ctx);
    } else if (type == "named_expression") {
        bindTargets(childByFieldName(node, "name"), ctx);
    } else if (type == "type_alias_statement") {
        TSNode left = childByFieldName(node, "left");
        if (!nodeIsNull(left) && ts_node_named_child_count(left) > 0)
            bindTargets(ts_node_named_child(left, 0), ctx);
    }

    // Default: recurse into children
    uint32_t count = ts_node_child_count(node);
    for (uint32_t i = 0; i < count; ++i) {
        traverseNode(ts_node_child(node, i), ctx);
    }
}

//  class definition

static void handleClassDef(TSNode node, TraversalContext& ctx) {
    // name field
    TSNode nameNode = childByFieldName(node, "name");
    if (nodeIsNull(nameNode)) return;

    std::string className = nodeText(nameNode, ctx.src);
    std::string classId   = ctx.fileId + "::" + className;

    int startLine = (int)ts_node_start_point(node).row;
    int endLine   = (int)ts_node_end_point(node).row;

    ClassEntity ce;
    ce.class_id   = classId;
    ce.file_id    = ctx.fileId;
    ce.class_name = className;
    ce.start_line = startLine;
    ce.end_line   = endLine;
    ctx.result.classes.push_back(ce);

    addSymbol(ctx, classId, "class", className, startLine, endLine);
    addBinding(ctx, className, "def", classId, startLine);
    addScope(ctx, classId, "class", startLine, endLine);
    // Bases are evaluated in the scope enclosing the class statement.
    const std::string outerScope = ctx.parentId();

    // base_class entities and INHERITS refs; the resolver writes the links
    TSNode superclassesNode = childByFieldName(node, "superclasses");
    if (!nodeIsNull(superclassesNode)) {
        int ordinal = 0;
        uint32_t argCount = ts_node_child_count(superclassesNode);
        for (uint32_t i = 0; i < argCount; ++i) {
            TSNode arg = ts_node_child(superclassesNode, i);
            if (nodeIsNull(arg)) continue;
            std::string argType = nodeType(arg);
            // Skip punctuation
            if (argType == "," || argType == "(" || argType == ")") continue;

            std::string baseText;
            if (argType == "identifier") {
                baseText = nodeText(arg, ctx.src);
            } else if (argType == "attribute") {
                baseText = nodeText(arg, ctx.src);
            } else if (argType == "subscript") {
                // `Base[T]`: a generic base. Still the class Base, so it gets a
                // ref; no base_class row, as before.
                addRef(ctx.result, ctx.fileId, classId, outerScope, "INHERITS",
                       childByFieldName(arg, "value"), ctx.src);
                continue;
            } else {
                // keyword_argument or other — skip
                continue;
            }
            if (baseText.empty()) continue;

            BaseClassEntity bce;
            bce.class_id        = classId;
            bce.base_class_name = baseText;
            bce.ordinal         = ordinal++;
            ctx.result.base_classes.push_back(bce);

            addRef(ctx.result, ctx.fileId, classId, outerScope, "INHERITS", arg, ctx.src);
        }
    }

    // Recurse into class body with class on scope stack
    TSNode bodyNode = childByFieldName(node, "body");
    ctx.scopeStack.push_back({classId, "class", {}});
    collectDeclared(bodyNode, ctx.src, ctx.scopeStack.back().declared);
    if (!nodeIsNull(bodyNode)) {
        uint32_t count = ts_node_child_count(bodyNode);
        for (uint32_t i = 0; i < count; ++i) {
            traverseNode(ts_node_child(bodyNode, i), ctx);
        }
    }
    ctx.scopeStack.pop_back();
}

// function_definition / async_function_definition 

static void handleFunctionDef(TSNode node, TraversalContext& ctx, bool isAsync) {
    TSNode nameNode = childByFieldName(node, "name");
    if (nodeIsNull(nameNode)) return;

    std::string funcName  = nodeText(nameNode, ctx.src);
    std::string parentId  = ctx.parentId();
    std::string funcId    = parentId + "::" + funcName;

    int startLine = (int)ts_node_start_point(node).row;
    int endLine   = (int)ts_node_end_point(node).row;

    // Compute per-function metrics that require an AST pass over the body.
    TSNode bodyNode = childByFieldName(node, "body");

    int cc = 1 + (!nodeIsNull(bodyNode) ? countCyclomaticComplexity(bodyNode) : 0);

    int maxBlockDepth = 0;
    if (!nodeIsNull(bodyNode))
        computeMaxBlockDepth(bodyNode, 0, maxBlockDepth);

    FunctionEntity fe;
    fe.function_id            = funcId;
    fe.function_name          = funcName;
    fe.nesting_depth          = ctx.nestingDepth();
    fe.is_async               = isAsync ? 1 : 0;
    fe.cyclomatic_complexity  = cc;
    fe.max_block_depth        = maxBlockDepth;
    fe.loc                    = endLine - startLine + 1;
    fe.start_line             = startLine;
    fe.end_line               = endLine;
    if (ctx.parentIsClass())
        fe.class_id = parentId;
    else
        fe.file_id = ctx.fileId;
    ctx.result.functions.push_back(fe);

    const bool isMethod = ctx.parentIsClass();
    addSymbol(ctx, funcId, isMethod ? "method" : "function", funcName, startLine, endLine);
    addBinding(ctx, funcName, "def", funcId, startLine);
    addScope(ctx, funcId, "function", startLine, endLine);

    // Parameters bind in the function's own scope; push it now so they land
    // there. Declarations are collected first so they are known before any
    // binding in the body.
    ctx.scopeStack.push_back({funcId, "function", {}});
    collectDeclared(bodyNode, ctx.src, ctx.scopeStack.back().declared);

    // Parameters
    TSNode paramsNode = childByFieldName(node, "parameters");
    if (!nodeIsNull(paramsNode)) {
        int ordinal = 0;
        uint32_t paramCount = ts_node_child_count(paramsNode);
        for (uint32_t i = 0; i < paramCount; ++i) {
            TSNode param = ts_node_child(paramsNode, i);
            if (nodeIsNull(param)) continue;
            std::string ptype = nodeType(param);

            std::string paramName;
            if (ptype == "identifier") {
                paramName = nodeText(param, ctx.src);
            } else if (ptype == "default_parameter") {
                // name = value  — grab the name field
                TSNode pname = childByFieldName(param, "name");
                if (!nodeIsNull(pname)) paramName = nodeText(pname, ctx.src);
            } else if (ptype == "typed_parameter") {
                // name: type — first named child is usually identifier
                uint32_t tpc = ts_node_child_count(param);
                for (uint32_t j = 0; j < tpc; ++j) {
                    TSNode c = ts_node_child(param, j);
                    if (!nodeIsNull(c) && nodeType(c) == "identifier") {
                        paramName = nodeText(c, ctx.src);
                        break;
                    }
                }
            } else if (ptype == "typed_default_parameter") {
                TSNode pname = childByFieldName(param, "name");
                if (!nodeIsNull(pname)) paramName = nodeText(pname, ctx.src);
            } else if (ptype == "list_splat_pattern" || ptype == "dictionary_splat_pattern") {
                // *args or **kwargs — get inner identifier
                uint32_t sc = ts_node_child_count(param);
                for (uint32_t j = 0; j < sc; ++j) {
                    TSNode c = ts_node_child(param, j);
                    if (!nodeIsNull(c) && nodeType(c) == "identifier") {
                        paramName = nodeText(c, ctx.src);
                        break;
                    }
                }
            }
            // Skip punctuation and empty
            if (paramName.empty() || paramName == "," ||
                paramName == "(" || paramName == ")") continue;

            ParamEntity pe;
            pe.function_id = funcId;
            pe.param_name  = paramName;
            pe.ordinal     = ordinal++;
            ctx.result.params.push_back(pe);

            addBinding(ctx, paramName, "def", "", nodeLine(param));
        }
    }

    // Collect CALLS from the function body (bodyNode already fetched above)
    if (!nodeIsNull(bodyNode)) {
        collectCalls(bodyNode, funcId, funcId, ctx.fileId, ctx.src, ctx.result);
        // Collect self.attr assignments — only meaningful inside methods
        if (isMethod)
            collectFields(bodyNode, parentId, ctx.fileId, ctx.src, ctx.result);
    }

    // Recurse into function body for nested definitions
    if (!nodeIsNull(bodyNode)) {
        uint32_t count = ts_node_child_count(bodyNode);
        for (uint32_t i = 0; i < count; ++i) {
            traverseNode(ts_node_child(bodyNode, i), ctx);
        }
    }
    ctx.scopeStack.pop_back();
}

// import_statement 

static void handleImport(TSNode node, TraversalContext& ctx) {
    // import a, b, c  OR  import a as b
    uint32_t count = ts_node_child_count(node);
    for (uint32_t i = 0; i < count; ++i) {
        TSNode child = ts_node_child(node, i);
        if (nodeIsNull(child)) continue;
        std::string ctype = nodeType(child);
        std::string modName;
        std::string alias;
        if (ctype == "dotted_name") {
            modName = nodeText(child, ctx.src);
        } else if (ctype == "aliased_import") {
            TSNode nameNode = childByFieldName(child, "name");
            if (!nodeIsNull(nameNode)) modName = nodeText(nameNode, ctx.src);
            TSNode aliasNode = childByFieldName(child, "alias");
            if (!nodeIsNull(aliasNode)) alias = nodeText(aliasNode, ctx.src);
        }
        if (!modName.empty()) {
            LinkEntity link;
            link.source_id = ctx.fileId;
            link.target_id = modName;
            link.link_type = "IMPORTS";
            ctx.result.links.push_back(link);

            // `import a.b.c` binds the top-level package `a`; `import a.b as x`
            // binds `x` to a.b itself. module_spec is the full dotted name either way.
            std::string local = alias.empty() ? modName.substr(0, modName.find('.')) : alias;
            addBinding(ctx, local, "import_module", "", nodeLine(child), modName);
        }
    }
}

// import_from_statement 

static void handleImportFrom(TSNode node, TraversalContext& ctx) {
    // from X import Y
    TSNode moduleNode = childByFieldName(node, "module_name");
    if (!nodeIsNull(moduleNode)) {
        std::string modName = nodeText(moduleNode, ctx.src);
        if (!modName.empty()) {
            LinkEntity link;
            link.source_id = ctx.fileId;
            link.target_id = modName;
            link.link_type = "IMPORTS";
            ctx.result.links.push_back(link);

            // module_spec stays verbatim (".", "..pkg") for resolveModule.
            uint32_t count = ts_node_child_count(node);
            for (uint32_t i = 0; i < count; ++i) {
                TSNode child = ts_node_child(node, i);
                std::string ctype = nodeType(child);
                if (ctype == "wildcard_import") {
                    // One per statement, so the line keeps several apart in one scope.
                    int line = nodeLine(child);
                    addBinding(ctx, "*" + std::to_string(line), "import_all", "", line, modName);
                    continue;
                }
                const char* field = ts_node_field_name_for_child(node, i);
                if (!field || std::strcmp(field, "name") != 0) continue;
                std::string name;
                std::string local;
                if (ctype == "dotted_name") {
                    name = local = nodeText(child, ctx.src);
                } else if (ctype == "aliased_import") {
                    TSNode nameNode  = childByFieldName(child, "name");
                    TSNode aliasNode = childByFieldName(child, "alias");
                    if (nodeIsNull(nameNode) || nodeIsNull(aliasNode)) continue;
                    name  = nodeText(nameNode, ctx.src);
                    local = nodeText(aliasNode, ctx.src);
                }
                addBinding(ctx, local, "import_symbol", "", nodeLine(child), modName, name);
            }
        }
    }
}

//  decorated_definition 

static void handleDecoratedDef(TSNode node, TraversalContext& ctx) {
    // Collect the callee chain of each decorator first, for refs
    std::vector<TSNode> decoratorExprs;
    uint32_t count = ts_node_child_count(node);
    for (uint32_t i = 0; i < count; ++i) {
        TSNode child = ts_node_child(node, i);
        if (nodeIsNull(child)) continue;
        std::string ctype = nodeType(child);
        if (ctype == "decorator") {
            // decorator body is usually @name or @name(args)
            // Get the child after "@"
            uint32_t dc = ts_node_child_count(child);
            for (uint32_t j = 0; j < dc; ++j) {
                TSNode dc_node = ts_node_child(child, j);
                if (nodeIsNull(dc_node)) continue;
                std::string dtype = nodeType(dc_node);
                if (dtype == "identifier" || dtype == "dotted_name" || dtype == "attribute") {
                    decoratorExprs.push_back(dc_node);
                    break;
                } else if (dtype == "call") {
                    TSNode callFunc = childByFieldName(dc_node, "function");
                    if (!nodeIsNull(callFunc)) decoratorExprs.push_back(callFunc);
                    break;
                }
            }
        }
    }
    // Decorators are evaluated in the scope enclosing the definition.
    const std::string outerScope = ctx.parentId();

    // Now process the inner definition (function or class) to get its ID
    // We process it normally first, then create DECORATES refs
    // We need to know the entity ID before creating the refs.
    // Strategy: note current sizes, traverse, then check what was added.
    size_t prevFuncSize  = ctx.result.functions.size();
    size_t prevClassSize = ctx.result.classes.size();

    for (uint32_t i = 0; i < count; ++i) {
        TSNode child = ts_node_child(node, i);
        if (nodeIsNull(child)) continue;
        std::string ctype = nodeType(child);
        if (ctype == "function_definition") {
            // Detect async keyword inside the function_definition node
            bool isAsync = false;
            uint32_t fc = ts_node_child_count(child);
            for (uint32_t k = 0; k < fc; ++k) {
                TSNode kc = ts_node_child(child, k);
                if (!nodeIsNull(kc) && nodeType(kc) == "async") { isAsync = true; break; }
            }
            handleFunctionDef(child, ctx, isAsync);
        } else if (ctype == "class_definition") {
            handleClassDef(child, ctx);
        }
        // Skip decorator nodes (already handled above)
    }

    // Determine the decorated entity's ID.
    // Prefer class over function: handleClassDef() may emit methods as side-effects,
    // so when both counts grow we must pick the class, not the first new method.
    std::string decoratedId;
    if (ctx.result.classes.size() > prevClassSize) {
        decoratedId = ctx.result.classes[prevClassSize].class_id;
    } else if (ctx.result.functions.size() > prevFuncSize) {
        decoratedId = ctx.result.functions[prevFuncSize].function_id;
    }

    if (!decoratedId.empty()) {
        // The DECORATES link runs decorator -> decorated, but a ref's source is
        // where the name occurs, so here source_id is the decorated entity and
        // name_path the decorator. The resolver writes the link reversed.
        for (TSNode expr : decoratorExprs)
            addRef(ctx.result, ctx.fileId, decoratedId, outerScope, "DECORATES", expr, ctx.src);
    }
}

//  collectFields
// Recursively traverse a method body looking for `self.attr = ...` assignments.
// Does NOT descend into nested function_definition nodes.

static char fieldAccess(const std::string& name)
{
    const bool dunder = name.size() >= 4 &&
        name[0] == '_' && name[1] == '_' &&
        name[name.size()-2] == '_' && name[name.size()-1] == '_';
    if (dunder) return '\0'; // skip magic attrs
    if (name.size() >= 2 && name[0] == '_' && name[1] == '_') return '-';
    if (!name.empty() && name[0] == '_') return '#';
    return '+';
}

static void collectFields(TSNode node, const std::string& classId,
                          const std::string& fileId,
                          const std::string& src, ParseResult& result)
{
    if (nodeIsNull(node)) return;
    std::string type = nodeType(node);

    if (type == "function_definition") return; // nested func — skip

    if (type == "assignment" || type == "annotated_assignment") {
        TSNode left = childByFieldName(node, "left");
        if (!nodeIsNull(left) && nodeType(left) == "attribute") {
            TSNode obj  = childByFieldName(left, "object");
            TSNode attr = childByFieldName(left, "attribute");
            if (!nodeIsNull(obj) && !nodeIsNull(attr) &&
                nodeType(obj) == "identifier" && nodeText(obj, src) == "self") {
                std::string fname = nodeText(attr, src);
                char access = fieldAccess(fname);
                if (!fname.empty() && access != '\0') {
                    FieldEntity fe;
                    fe.class_id  = classId;
                    fe.field_name = fname;
                    fe.access    = access;
                    result.fields.push_back(std::move(fe));

                    SymbolEntity se;
                    se.symbol_id  = classId + "::" + fname;
                    se.file_id    = fileId;
                    se.parent_id  = classId;
                    se.kind       = "field";
                    se.name       = fname;
                    se.start_line = se.end_line = nodeLine(left);
                    result.symbols.push_back(std::move(se));
                }
            }
        }
    }

    uint32_t count = ts_node_child_count(node);
    for (uint32_t i = 0; i < count; ++i)
        collectFields(ts_node_child(node, i), classId, fileId, src, result);
}

//  collectCalls 
// Recursively traverse bodyNode looking for `call` nodes, but do NOT descend
// into nested function_definition nodes (those calls belong to inner funcs).

// scopeId is the innermost scope the call sits in: funcId itself, or a class
// defined inside the function (whose body is its own scope, though its calls
// still belong to funcId).

static void collectCalls(TSNode node, const std::string& funcId,
                         const std::string& scopeId, const std::string& fileId,
                         const std::string& src, ParseResult& result) {
    if (nodeIsNull(node)) return;
    std::string type = nodeType(node);

    // Don't descend into nested function definitions — their calls are theirs
    if (type == "function_definition") return;

    if (type == "class_definition") {
        TSNode nameNode = childByFieldName(node, "name");
        const std::string classScope = nodeIsNull(nameNode)
            ? scopeId : fileId + "::" + nodeText(nameNode, src);
        uint32_t count = ts_node_child_count(node);
        for (uint32_t i = 0; i < count; ++i) {
            const char* field = ts_node_field_name_for_child(node, i);
            const bool inBody = field && std::strcmp(field, "body") == 0;
            collectCalls(ts_node_child(node, i), funcId, inBody ? classScope : scopeId,
                         fileId, src, result);
        }
        return;
    }

    if (type == "call") {
        // Only identifier chains become refs; `super().x`, `d[k].f` and the
        // like have no name to look up. The resolver writes the CALLS links.
        TSNode funcNode = childByFieldName(node, "function");
        if (!nodeIsNull(funcNode))
            addRef(result, fileId, funcId, scopeId, "CALLS", funcNode, src);
        // Still recurse into arguments (there may be nested calls)
        TSNode argsNode = childByFieldName(node, "arguments");
        if (!nodeIsNull(argsNode)) {
            collectCalls(argsNode, funcId, scopeId, fileId, src, result);
        }
        return;
    }

    uint32_t count = ts_node_child_count(node);
    for (uint32_t i = 0; i < count; ++i) {
        collectCalls(ts_node_child(node, i), funcId, scopeId, fileId, src, result);
    }
}

// ── per-file finishing for name resolution ───────────────────────────────────

// "pkg/a/b.py" -> "pkg.a.b", "pkg/__init__.py" -> "pkg", "__init__.py" -> "".
// Informational only: it ignores package roots (src/ layouts), which the parser
// cannot see from one file; resolution goes through ILanguage::resolveModule.
static std::string moduleNameOf(const std::string& fileId) {
    std::string s = fileId;
    if (s.size() > 3 && s.compare(s.size() - 3, 3, ".py") == 0) s.erase(s.size() - 3);
    if (s == "__init__") return "";
    const std::string initSuffix = "/__init__";
    if (s.size() > initSuffix.size() &&
        s.compare(s.size() - initSuffix.size(), initSuffix.size(), initSuffix) == 0)
        s.erase(s.size() - initSuffix.size());
    std::replace(s.begin(), s.end(), '/', '.');
    return s;
}

// True if the module assigns `__all__` exactly once at top level, to a list or
// tuple of plain string literals; `out` then holds the names. Anything else
// (no __all__, `__all__ += ...`, a computed value) returns false, and every
// symbol stays exported.
static bool literalDunderAll(TSNode root, const std::string& src,
                             std::set<std::string>& out) {
    int assignments = 0;
    bool literal = false;
    uint32_t n = ts_node_child_count(root);
    for (uint32_t i = 0; i < n; ++i) {
        TSNode stmt = ts_node_child(root, i);
        if (nodeType(stmt) != "expression_statement" || ts_node_named_child_count(stmt) == 0)
            continue;
        TSNode expr = ts_node_named_child(stmt, 0);
        std::string etype = nodeType(expr);
        if (etype != "assignment" && etype != "augmented_assignment") continue;
        TSNode left = childByFieldName(expr, "left");
        if (nodeType(left) != "identifier" || nodeText(left, src) != "__all__") continue;

        ++assignments;
        literal = false;
        TSNode right = childByFieldName(expr, "right");
        std::string rtype = nodeType(right);
        if (etype != "assignment" || (rtype != "list" && rtype != "tuple")) continue;

        literal = true;
        out.clear();
        uint32_t m = ts_node_named_child_count(right);
        for (uint32_t j = 0; j < m && literal; ++j) {
            TSNode str = ts_node_named_child(right, j);
            if (nodeType(str) != "string") { literal = false; break; }
            // string_start, string_content, string_end -- nothing else (no f-string
            // interpolation, no implicit concatenation).
            std::string content;
            uint32_t k = ts_node_named_child_count(str);
            for (uint32_t c = 0; c < k; ++c) {
                TSNode part = ts_node_named_child(str, c);
                std::string ptype = nodeType(part);
                if (ptype == "string_content") content = nodeText(part, src);
                else if (ptype != "string_start" && ptype != "string_end") literal = false;
            }
            if (literal) out.insert(content);
        }
    }
    return assignments == 1 && literal;
}

// Give every symbol_id one row. Ids repeat when a name is defined twice (an
// if/else pair of defs, a property getter and setter, `def f` followed by
// `f = wrap(f)`, `self.x` assigned in several methods). A class/function/method
// definition wins over a field or variable of the same id, and among
// definitions the last one wins, as it does in the class and function tables
// (INSERT OR REPLACE). Same for scopes. Then mark which module-level symbols and
// bindings `from m import *` takes: those __all__ lists when there is a literal
// one (an imported name it leaves out is not re-exported either), else those
// whose name does not start with an underscore.
static void finalizeSymbols(ParseResult& result, const std::set<std::string>* exports) {
    auto isDef = [](const std::string& kind) {
        return kind == "class" || kind == "function" || kind == "method";
    };

    std::unordered_map<std::string, size_t> index;
    std::vector<SymbolEntity> unique;
    unique.reserve(result.symbols.size());
    for (auto& sym : result.symbols) {
        auto it = index.find(sym.symbol_id);
        if (it == index.end()) {
            index.emplace(sym.symbol_id, unique.size());
            unique.push_back(std::move(sym));
        } else if (isDef(sym.kind)) {
            unique[it->second] = std::move(sym);
        }
    }
    auto exported = [exports](const std::string& name) {
        return exports ? exports->count(name) != 0 : name.empty() || name[0] != '_';
    };
    for (auto& sym : unique)
        if (sym.parent_id.empty() && !exported(sym.name)) sym.is_exported = 0;
    for (auto& b : result.bindings)
        if (b.scope_id == result.file.file_id && b.kind != "import_all" && !exported(b.local_name))
            b.is_exported = 0;
    result.symbols = std::move(unique);

    std::unordered_map<std::string, size_t> scopeIndex;
    std::vector<ScopeEntity> scopes;
    scopes.reserve(result.scopes.size());
    for (auto& sc : result.scopes) {
        auto it = scopeIndex.find(sc.scope_id);
        if (it == scopeIndex.end()) {
            scopeIndex.emplace(sc.scope_id, scopes.size());
            scopes.push_back(std::move(sc));
        } else {
            scopes[it->second] = std::move(sc);
        }
    }
    result.scopes = std::move(scopes);
}

// ─── PythonParser ─────────────────────────────────────────────────────────────

struct TsParserDeleter {
    void operator()(TSParser* p) const { ts_parser_delete(p); }
};

using TsParserPtr = std::unique_ptr<TSParser, TsParserDeleter>;

static TsParserPtr makeTsParser() {
    TsParserPtr p(ts_parser_new());
    ts_parser_set_language(p.get(), tree_sitter_python());
    return p;
}

ParseResult PythonParser::parseFile(const std::string& filePath,
                                    const std::string& repoRoot) {
    ParseResult result;

    // Read file contents
    std::ifstream ifs(filePath);
    if (!ifs.is_open()) return result;
    std::ostringstream oss;
    oss << ifs.rdbuf();
    std::string src = oss.str();

    // Compute file metadata
    fs::path absPath   = fs::absolute(filePath);
    fs::path rootPath  = fs::absolute(repoRoot);
    std::string fileId;
    try {
        fileId = fs::relative(absPath, rootPath).string();
    } catch (...) {
        fileId = absPath.string();
    }
    // Normalize path separators to forward slash
    std::replace(fileId.begin(), fileId.end(), '\\', '/');

    int loc = (int)std::count(src.begin(), src.end(), '\n');
    // If file doesn't end with newline, add 1 for the last line
    if (!src.empty() && src.back() != '\n') ++loc;

    // Logical LOC: exclude blank lines and comment-only lines.
    int logical_loc = 0;
    {
        std::istringstream lss(src);
        std::string ln;
        while (std::getline(lss, ln)) {
            size_t first = ln.find_first_not_of(" \t\r");
            if (first == std::string::npos) continue; // blank
            if (ln[first] == '#') continue;            // comment-only
            ++logical_loc;
        }
    }

    // Generated/vendored detection: flag rather than exclude so the file remains
    // visible; downstream ranking can filter on is_generated = 0.
    int is_generated = 0;

    // Path-based: split fileId by '/' and compare each directory segment in full.
    // Filename (last component) is skipped — only directory segments are checked.
    {
        static const char* kSegments[] = {
            "vendor", "_vendor", "third_party", "node_modules",
            "venv", ".venv", "site-packages", "__pycache__",
            "dist", "build", "out", nullptr
        };
        std::string remaining = fileId;
        size_t pos;
        while ((pos = remaining.find('/')) != std::string::npos && !is_generated) {
            std::string seg = remaining.substr(0, pos);
            remaining = remaining.substr(pos + 1);
            for (int si = 0; kSegments[si] && !is_generated; ++si) {
                if (seg == kSegments[si]) is_generated = 1;
            }
        }
    }

    // Content-based: generation-marker comments within the first 5 lines.
    if (!is_generated) {
        std::istringstream gss(src);
        std::string gline;
        int checked = 0;
        while (std::getline(gss, gline) && checked < 5 && !is_generated) {
            size_t first = gline.find_first_not_of(" \t\r");
            if (first != std::string::npos && gline[first] == '#') {
                std::string lower = gline.substr(first);
                for (char& c : lower) c = (char)std::tolower((unsigned char)c);
                if (lower.find("generated")   != std::string::npos ||
                    lower.find("do not edit") != std::string::npos ||
                    lower.find("autogenerated") != std::string::npos)
                    is_generated = 1;
            }
            ++checked;
        }
    }

    result.file.file_id     = fileId;
    result.file.file_name   = absPath.filename().string();
    result.file.language    = "python";
    result.file.module_name = moduleNameOf(fileId);
    result.file.raw_loc     = loc;
    result.file.logical_loc = logical_loc;
    result.file.is_generated = is_generated;

    // 파서는 호출마다 로컬로 생성 생성 비용은 파싱 대비 무시 가능(~0.0003ms)
    TsParserPtr parser = makeTsParser();
    //old_tree에 nullptr을 넘기므로 증분 파싱이 아니라 새 파싱
    TSTree* tree = ts_parser_parse_string(parser.get(), nullptr,
                                          src.c_str(), (uint32_t)src.size());
    if (!tree) return result;

    TSNode root = ts_tree_root_node(tree);

    // 노드의 텍스를 잘라낼 원본 소스(src)
    // 추출한 entity에 태깅할 fileId
    // 결과를 누적할 result
    // 현재 스코프(class 안인지, function 안인지)를 추적할 상태(scopeStack) 
    TraversalContext ctx{src, fileId, result, {}};

    // The file scope; its id is the file_id.
    {
        ScopeEntity sc;
        sc.scope_id   = fileId;
        sc.file_id    = fileId;
        sc.kind       = "file";
        sc.owner_id   = fileId;
        sc.start_line = 0;
        sc.end_line   = loc > 0 ? loc - 1 : 0;
        result.scopes.push_back(std::move(sc));
    }

    uint32_t count = ts_node_child_count(root);
    for (uint32_t i = 0; i < count; ++i) {
        traverseNode(ts_node_child(root, i), ctx);
    }

    std::set<std::string> exports;
    const bool hasAll = literalDunderAll(root, src, exports);

    ts_tree_delete(tree);

    finalizeSymbols(result, hasAll ? &exports : nullptr);

    // Roll up per-function metrics to file level
    {
        int maxCC = 0, maxBD = 0, maxLOC = 0;
        double sumCC = 0.0, sumBD = 0.0, sumLOC = 0.0;
        for (const auto& fn : result.functions) {
            if (fn.cyclomatic_complexity > maxCC)  maxCC  = fn.cyclomatic_complexity;
            if (fn.max_block_depth       > maxBD)  maxBD  = fn.max_block_depth;
            if (fn.loc                   > maxLOC) maxLOC = fn.loc;
            sumCC  += fn.cyclomatic_complexity;
            sumBD  += fn.max_block_depth;
            sumLOC += fn.loc;
        }
        double n = (double)result.functions.size();
        result.file.max_cyclomatic_complexity = maxCC;
        result.file.avg_cyclomatic_complexity = n > 0 ? sumCC  / n : 0.0;
        result.file.max_block_depth           = maxBD;
        result.file.avg_block_depth           = n > 0 ? sumBD  / n : 0.0;
        result.file.max_function_loc          = maxLOC;
        result.file.avg_function_loc          = n > 0 ? sumLOC / n : 0.0;
    }

    // Deduplicate links to satisfy PRIMARY KEY (source_id, target_id, link_type)
    // The parser only writes IMPORTS links, and the same module can be
    // imported at top level and again inside a function.
    {
        std::set<std::tuple<std::string, std::string, std::string>> seen;
        std::vector<LinkEntity> unique;
        unique.reserve(result.links.size());
        for (auto& link : result.links) {
            if (seen.emplace(link.source_id, link.target_id, link.link_type).second) {
                unique.push_back(std::move(link));
            }
        }
        result.links = std::move(unique);
    }

    return result;
}

std::vector<ParseResult> PythonParser::parseDirectory(
    const std::string& dirPath,
    const std::string& allowedRoot)
{
    std::vector<ParseResult> results;
    fs::path root = fs::absolute(dirPath);

    // Whitelist boundary check: dirPath must be within allowedRoot.
    if (!allowedRoot.empty()) {
        fs::path boundary = fs::absolute(allowedRoot).lexically_normal();
        fs::path normRoot  = root.lexically_normal();
        // lexically_relative returns a path from boundary to normRoot.
        // If the first component is "..", normRoot is outside boundary.
        fs::path rel = normRoot.lexically_relative(boundary);
        if (rel.empty() || *rel.begin() == "..") {
            std::cerr << "[PythonParser] Rejected: '" << normRoot.string()
                      << "' is outside the allowed root '" << boundary.string() << "'\n";
            return results;
        }
    }

    //  파싱 대상 파일 목록 수집
    std::vector<std::string> files;

    std::error_code ec;
    fs::recursive_directory_iterator it(root,
        fs::directory_options::skip_permission_denied, ec);
    fs::recursive_directory_iterator end;

    for (; !ec && it != end; it.increment(ec)) {
        if (ec) { ec.clear(); break; }

        const auto& entry = *it;

        if (entry.is_symlink()) continue;
        if (entry.is_directory()) continue;
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".py") continue;

        // Whitelist file-level guard: skip files outside the allowed root.
        if (!allowedRoot.empty()) {
            fs::path boundary = fs::absolute(allowedRoot).lexically_normal();
            fs::path filePath = fs::absolute(entry.path()).lexically_normal();
            fs::path rel = filePath.lexically_relative(boundary);
            if (rel.empty() || *rel.begin() == "..") continue; // .. 나오면 boundary 밖이라는 뜻
        }

        files.push_back(entry.path().string());
    }

    // 병렬 파싱 
    // parseFile은 멤버 상태를 쓰지 않아 스레드 안전. 각 워커가 아토믹 카운터로
    // 다음 파일 인덱스를 가져가 results[i]에 기록하므로 락 없이 순서가 결정적이다.
    results.resize(files.size());
    {
        const std::string rootStr = root.string();
        unsigned hw = std::thread::hardware_concurrency();
        if (hw == 0) hw = 1;

        // 적정한 thread 수 판단. thread 16개 가능한데, 파일 3개면 3개만 만들면 된다.
        unsigned numThreads = std::min<unsigned>(hw, (unsigned)files.size()); 

        // atomic을 이용해서, 여러 thread가 동시에 접근해도 충돌 X. 
        std::atomic<size_t> next{0}; 
        auto worker = [&]() {
            size_t i;

            //fetch_add는 etch_add(1)은 두 동작을 원자적으로 수행. 
            // 1. 현재 next 값을 반환. 
            // 2. next를 1 증가.
            while ((i = next.fetch_add(1)) < files.size()) { 
                try {
                    results[i] = parseFile(files[i], rootStr);
                } catch (...) {
                    // 해당 파일만 빈 결과로 남기고 계속 진행.
                    // (std::thread에서 예외가 새면 프로세스가 종료됨)
                }
            }
        };

        std::vector<std::thread> pool;
        pool.reserve(numThreads);
        for (unsigned t = 0; t < numThreads; ++t)
            pool.emplace_back(worker);
        for (auto& th : pool)
            th.join();
    }

    // CALLS / INHERITS / DECORATES links are not resolved here: the parser
    // only sees one file at a time, so Resolver does it on the database once
    // every file is in.

    return results;
}
