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

    // The immediate parent for a new function: its parent_id and parent_type
    std::string parentId() const {
        if (scopeStack.empty()) return fileId;
        return scopeStack.back().id;
    }

    std::string parentType() const {
        if (scopeStack.empty()) return "file";
        return (scopeStack.back().kind == "class") ? "class" : "file";
    }
};

// ─── forward declarations ────────────────────────────────────────────────────
static void traverseNode(TSNode node, TraversalContext& ctx);
static void handleClassDef(TSNode node, TraversalContext& ctx);
static void handleFunctionDef(TSNode node, TraversalContext& ctx, bool isAsync);
static void handleImport(TSNode node, TraversalContext& ctx);
static void handleImportFrom(TSNode node, TraversalContext& ctx);
static void handleDecoratedDef(TSNode node, TraversalContext& ctx);
static void collectCalls(TSNode node, const std::string& funcId,
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
    std::string parentType= ctx.parentType();
    std::string funcId    = parentId + "::" + funcName;

    int startLine = (int)ts_node_start_point(node).row;
    int endLine   = (int)ts_node_end_point(node).row;

    FunctionEntity fe;
    fe.function_id    = funcId;
    fe.parent_id      = parentId;
    fe.function_name  = funcName;
    fe.parent_type    = parentType;
    fe.nesting_depth  = ctx.nestingDepth();
    fe.is_async       = isAsync ? 1 : 0;
    fe.start_line     = startLine;
    fe.end_line       = endLine;
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

    // Collect CALLS from the function body
    TSNode bodyNode = childByFieldName(node, "body");
    if (!nodeIsNull(bodyNode)) {
        collectCalls(bodyNode, funcId, ctx.src, ctx.result);
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

    // Determine the decorated entity's ID
    std::string decoratedId;
    if (ctx.result.functions.size() > prevFuncSize) {
        decoratedId = ctx.result.functions[prevFuncSize].function_id;
    } else if (ctx.result.classes.size() > prevClassSize) {
        decoratedId = ctx.result.classes[prevClassSize].class_id;
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

    result.file.file_id  = fileId;
    result.file.file_name= absPath.filename().string();
    result.file.path     = absPath.string();
    result.file.language = "python";
    result.file.loc      = loc;

    // Parse with tree-sitter
    TSParser* parser = static_cast<TSParser*>(m_parser);
    TSTree* tree = ts_parser_parse_string(parser, nullptr,
                                          src.c_str(), (uint32_t)src.size());
    if (!tree) return result;

    TSNode root = ts_tree_root_node(tree);

    TraversalContext ctx{src, fileId, result, {}};
    uint32_t count = ts_node_child_count(root);
    for (uint32_t i = 0; i < count; ++i) {
        traverseNode(ts_node_child(root, i), ctx);
    }

    ts_tree_delete(tree);

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

        if (entry.is_directory()) continue;
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".py") continue;

        // Whitelist file-level guard: skip files outside the allowed root.
        if (!allowedRoot.empty()) {
            fs::path boundary = fs::absolute(allowedRoot).lexically_normal();
            fs::path filePath = fs::absolute(entry.path()).lexically_normal();
            fs::path rel = filePath.lexically_relative(boundary);
            if (rel.empty() || *rel.begin() == "..") continue;
        }

        ParseResult pr = parseFile(entry.path().string(), root.string());
        results.push_back(std::move(pr));
    }
    return results;
}
