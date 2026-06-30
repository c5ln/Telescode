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

extern "C" const TSLanguage* tree_sitter_python();

namespace fs = std::filesystem;

// ─── helpers ──────────────────────────────────────────────────────────────────

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

// ─── context passed through recursive traversal ──────────────────────────────

struct TraversalContext {
    const std::string& src;
    const std::string& fileId;
    ParseResult& result;

    // Stack of enclosing entity IDs and their types ("class" / "function")
    struct Frame {
        std::string id;   // class_id or function_id
        std::string kind; // "class" or "function"
    };
    std::vector<Frame> scopeStack;

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

// ─── countCyclomaticComplexity ────────────────────────────────────────────────
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

// ─── computeMaxBlockDepth / computeMaxBlockDepthClauses ──────────────────────
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

// ─── forward declarations ────────────────────────────────────────────────────
static void traverseNode(TSNode node, TraversalContext& ctx);
static void handleClassDef(TSNode node, TraversalContext& ctx);
static void handleFunctionDef(TSNode node, TraversalContext& ctx, bool isAsync);
static void handleImport(TSNode node, TraversalContext& ctx);
static void handleImportFrom(TSNode node, TraversalContext& ctx);
static void handleDecoratedDef(TSNode node, TraversalContext& ctx);
static void collectCalls(TSNode node, const std::string& funcId,
                         const std::string& src, ParseResult& result);
static void collectFields(TSNode node, const std::string& classId,
                          const std::string& src, ParseResult& result);

// ─── traversal ───────────────────────────────────────────────────────────────

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

    // Default: recurse into children
    uint32_t count = ts_node_child_count(node);
    for (uint32_t i = 0; i < count; ++i) {
        traverseNode(ts_node_child(node, i), ctx);
    }
}

// ─── class_definition ────────────────────────────────────────────────────────

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

    // INHERITS link + base_class entities
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

            LinkEntity link;
            link.source_id  = classId;
            link.target_id  = baseText;
            link.link_type  = "INHERITS";
            ctx.result.links.push_back(link);
        }
    }

    // Recurse into class body with class on scope stack
    ctx.scopeStack.push_back({classId, "class"});
    TSNode bodyNode = childByFieldName(node, "body");
    if (!nodeIsNull(bodyNode)) {
        uint32_t count = ts_node_child_count(bodyNode);
        for (uint32_t i = 0; i < count; ++i) {
            traverseNode(ts_node_child(bodyNode, i), ctx);
        }
    }
    ctx.scopeStack.pop_back();
}

// ─── function_definition / async_function_definition ────────────────────────

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
        }
    }

    // Collect CALLS from the function body (bodyNode already fetched above)
    if (!nodeIsNull(bodyNode)) {
        collectCalls(bodyNode, funcId, ctx.src, ctx.result);
        // Collect self.attr assignments — only meaningful inside methods
        if (ctx.parentIsClass())
            collectFields(bodyNode, ctx.parentId(), ctx.src, ctx.result);
    }

    // Recurse into function body for nested definitions
    ctx.scopeStack.push_back({funcId, "function"});
    if (!nodeIsNull(bodyNode)) {
        uint32_t count = ts_node_child_count(bodyNode);
        for (uint32_t i = 0; i < count; ++i) {
            traverseNode(ts_node_child(bodyNode, i), ctx);
        }
    }
    ctx.scopeStack.pop_back();
}

// ─── import_statement ────────────────────────────────────────────────────────

static void handleImport(TSNode node, TraversalContext& ctx) {
    // import a, b, c  OR  import a as b
    uint32_t count = ts_node_child_count(node);
    for (uint32_t i = 0; i < count; ++i) {
        TSNode child = ts_node_child(node, i);
        if (nodeIsNull(child)) continue;
        std::string ctype = nodeType(child);
        std::string modName;
        if (ctype == "dotted_name") {
            modName = nodeText(child, ctx.src);
        } else if (ctype == "aliased_import") {
            TSNode nameNode = childByFieldName(child, "name");
            if (!nodeIsNull(nameNode)) modName = nodeText(nameNode, ctx.src);
        }
        if (!modName.empty()) {
            LinkEntity link;
            link.source_id = ctx.fileId;
            link.target_id = modName;
            link.link_type = "IMPORTS";
            ctx.result.links.push_back(link);
        }
    }
}

// ─── import_from_statement ───────────────────────────────────────────────────

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
        }
    }
}

// ─── decorated_definition ────────────────────────────────────────────────────

static void handleDecoratedDef(TSNode node, TraversalContext& ctx) {
    // Collect decorator names first
    std::vector<std::string> decoratorNames;
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
                if (dtype == "identifier" || dtype == "dotted_name") {
                    decoratorNames.push_back(nodeText(dc_node, ctx.src));
                    break;
                } else if (dtype == "call") {
                    TSNode callFunc = childByFieldName(dc_node, "function");
                    if (!nodeIsNull(callFunc)) {
                        decoratorNames.push_back(nodeText(callFunc, ctx.src));
                    }
                    break;
                }
            }
        }
    }

    // Now process the inner definition (function or class) to get its ID
    // We process it normally first, then create DECORATES links
    // We need to know the entity ID before creating the links.
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
        for (const auto& decName : decoratorNames) {
            LinkEntity link;
            link.source_id = decName;
            link.target_id = decoratedId;
            link.link_type = "DECORATES";
            ctx.result.links.push_back(link);
        }
    }
}

// ─── collectFields ───────────────────────────────────────────────────────────
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
                }
            }
        }
    }

    uint32_t count = ts_node_child_count(node);
    for (uint32_t i = 0; i < count; ++i)
        collectFields(ts_node_child(node, i), classId, src, result);
}

// ─── collectCalls ────────────────────────────────────────────────────────────
// Recursively traverse bodyNode looking for `call` nodes, but do NOT descend
// into nested function_definition nodes (those calls belong to inner funcs).

static void collectCalls(TSNode node, const std::string& funcId,
                         const std::string& src, ParseResult& result) {
    if (nodeIsNull(node)) return;
    std::string type = nodeType(node);

    // Don't descend into nested function definitions — their calls are theirs
    if (type == "function_definition") return;

    if (type == "call") {
        TSNode funcNode = childByFieldName(node, "function");
        if (!nodeIsNull(funcNode)) {
            std::string fnType = nodeType(funcNode);
            std::string calleeName;
            if (fnType == "identifier") {
                calleeName = nodeText(funcNode, src);
            } else if (fnType == "attribute") {
                calleeName = nodeText(funcNode, src);
            }
            if (!calleeName.empty()) {
                LinkEntity link;
                link.source_id = funcId;
                link.target_id = calleeName;
                link.link_type = "CALLS";
                result.links.push_back(link);
            }
        }
        // Still recurse into arguments (there may be nested calls)
        TSNode argsNode = childByFieldName(node, "arguments");
        if (!nodeIsNull(argsNode)) {
            collectCalls(argsNode, funcId, src, result);
        }
        return;
    }

    uint32_t count = ts_node_child_count(node);
    for (uint32_t i = 0; i < count; ++i) {
        collectCalls(ts_node_child(node, i), funcId, src, result);
    }
}

// ─── resolveCallTargets ──────────────────────────────────────────────────────
// Within-file pass: resolve raw callee names in CALLS links to function_ids.
// Handles: plain identifiers, self.method and cls.method patterns.

static void resolveCallTargets(ParseResult& result) {
    // Build lookup: function_name -> list of function_ids
    std::unordered_map<std::string, std::vector<std::string>> nameToIds;
    for (const auto& fn : result.functions) {
        nameToIds[fn.function_name].push_back(fn.function_id);
    }

    std::unordered_map<std::string, std::string> funcToParent;
    std::unordered_map<std::string, bool> funcIsMethod;
    for (const auto& fn : result.functions) {
        bool isMethod = !fn.class_id.empty();
        funcToParent[fn.function_id] = isMethod ? fn.class_id : fn.file_id;
        funcIsMethod[fn.function_id] = isMethod;
    }

    for (auto& link : result.links) {
        if (link.link_type != "CALLS") continue;

        std::string raw = link.target_id;
        bool resolved = false;

        // Case 1: "self.method_name" or "cls.method_name" — resolve within caller's class
        bool isSelf = (raw.rfind("self.", 0) == 0 && raw.size() > 5);
        bool isCls  = (raw.rfind("cls.", 0) == 0 && raw.size() > 4);
        if (isSelf || isCls) {
            std::string methodName = isSelf ? raw.substr(5) : raw.substr(4);
            auto pit = funcToParent.find(link.source_id);
            if (pit != funcToParent.end() && funcIsMethod[link.source_id]) {
                std::string expected = pit->second + "::" + methodName;
                auto it = nameToIds.find(methodName);
                if (it != nameToIds.end()) {
                    for (const auto& fid : it->second) {
                        if (fid == expected) {
                            link.target_id = expected;
                            resolved = true;
                            break;
                        }
                    }
                }
            }
            // Fallback: treat as plain name
            if (!resolved) raw = methodName;
        }

        if (resolved) continue;

        // Case 2: attribute call other than self/cls (e.g. "obj.method") — skip
        if (raw.find('.') != std::string::npos) continue;

        // Case 3: plain identifier
        {
            auto it = nameToIds.find(raw);
            if (it != nameToIds.end()) {
                if (it->second.size() == 1) {
                    link.target_id = it->second[0];
                } else {
                    // Multiple matches: prefer same-parent scope
                    auto pit = funcToParent.find(link.source_id);
                    std::string callerParent = (pit != funcToParent.end()) ? pit->second : "";
                    std::string best;
                    for (const auto& fid : it->second) {
                        auto fpit = funcToParent.find(fid);
                        if (fpit != funcToParent.end() && fpit->second == callerParent) {
                            best = fid;
                            break;
                        }
                    }
                    link.target_id = best.empty() ? it->second[0] : best;
                }
            }
        }
    }
}

// ─── resolveCrossFileCallTargets ─────────────────────────────────────────────
// Cross-file pass: after all files are parsed, resolve remaining raw CALLS
// targets using the global function_name -> function_id map.

static void resolveCrossFileCallTargets(
    std::vector<ParseResult>& results,
    const std::unordered_map<std::string, std::vector<std::string>>& globalNameToIds)
{
    for (auto& pr : results) {
        for (auto& link : pr.links) {
            if (link.link_type != "CALLS") continue;
            // Already resolved (full path contains "::")
            if (link.target_id.find("::") != std::string::npos) continue;
            // Attribute call — skip
            if (link.target_id.find('.') != std::string::npos) continue;

            auto it = globalNameToIds.find(link.target_id);
            if (it != globalNameToIds.end() && it->second.size() == 1) {
                link.target_id = it->second[0];
            }
            // Ambiguous (multiple files define same name) — leave as raw
        }
    }
}

// ─── PythonParser ─────────────────────────────────────────────────────────────

PythonParser::PythonParser() {
    TSParser* parser = ts_parser_new();
    ts_parser_set_language(parser, tree_sitter_python());
    m_parser = parser;
}

PythonParser::~PythonParser() {
    if (m_parser) {
        ts_parser_delete(static_cast<TSParser*>(m_parser));
    }
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

    // Path-based: known vendor/env directory components in the relative file id.
    {
        static const char* kSegments[] = {
            "vendor", "_vendor", "third_party", "node_modules",
            "venv", ".venv", "site-packages", "__pycache__", nullptr
        };
        for (int si = 0; kSegments[si] && !is_generated; ++si) {
            std::string seg = kSegments[si];
            if (fileId.find("/" + seg + "/") != std::string::npos ||
                fileId.compare(0, seg.size() + 1, seg + "/") == 0)
                is_generated = 1;
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
    result.file.raw_loc     = loc;
    result.file.logical_loc = logical_loc;
    result.file.is_generated = is_generated;

    // Parse with tree-sitter
    TSParser* parser = static_cast<TSParser*>(m_parser);
    //old_tree에 nullptr을 넘기므로 증분 파싱이 아니라 새 파싱
    TSTree* tree = ts_parser_parse_string(parser, nullptr,
                                          src.c_str(), (uint32_t)src.size()); 
    if (!tree) return result;

    TSNode root = ts_tree_root_node(tree);

    // 노드의 텍스를 잘라낼 원본 소스(src)
    // 추출한 entity에 태깅할 fileId
    // 결과를 누적할 result
    // 현재 스코프(class 안인지, function 안인지)를 추적할 상태(scopeStack) 
    TraversalContext ctx{src, fileId, result, {}};
    uint32_t count = ts_node_child_count(root);
    for (uint32_t i = 0; i < count; ++i) {
        traverseNode(ts_node_child(root, i), ctx);
    }

    ts_tree_delete(tree);

    resolveCallTargets(result);

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
    // Same (source, target, type) can be generated multiple times, e.g.:
    //   - CALLS: same callee invoked N times in one function body
    //   - IMPORTS: same module imported at top-level and inside a function
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

        ParseResult pr = parseFile(entry.path().string(), root.string());
        results.push_back(std::move(pr));
    }

    // ── Phase B: cross-file CALLS resolve ────────────────────────────────────
    {
        std::unordered_map<std::string, std::vector<std::string>> globalNameToIds;
        for (const auto& pr : results) {
            for (const auto& fn : pr.functions) {
                globalNameToIds[fn.function_name].push_back(fn.function_id);
            }
        }
        resolveCrossFileCallTargets(results, globalNameToIds);
    }

    return results;
}
