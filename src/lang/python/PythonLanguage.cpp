#include "PythonLanguage.h"

#include <algorithm>
#include <utility>

namespace {

// Directory part of a file_id, without the trailing slash. "a.py" -> "".
std::string dir_of(const std::string& file_id)
{
    const std::size_t slash = file_id.find_last_of('/');
    return slash == std::string::npos ? std::string() : file_id.substr(0, slash);
}

// Strip `levels` trailing components. Returns false if that would go above the
// repo root -- `from ..x import y` in a top-level module has nothing to climb to.
bool climb(std::string& dir, int levels)
{
    for (int i = 0; i < levels; ++i) {
        if (dir.empty()) return false;
        const std::size_t slash = dir.find_last_of('/');
        dir = (slash == std::string::npos) ? std::string() : dir.substr(0, slash);
    }
    return true;
}

// "pkg/core" + "mod.sub" -> "pkg/core/mod/sub"; an empty tail keeps the base.
std::string join_module(const std::string& base, const std::string& dotted)
{
    std::string tail = dotted;
    for (char& c : tail) if (c == '.') c = '/';
    if (tail.empty()) return base;
    return base.empty() ? tail : base + "/" + tail;
}

// A module path resolves to either the module file or the package __init__.
std::string file_for(const std::string& path,
                     const std::unordered_set<std::string>& file_ids)
{
    if (path.empty()) return {};
    std::string candidate = path + ".py";
    if (file_ids.count(candidate)) return candidate;
    candidate = path + "/__init__.py";
    if (file_ids.count(candidate)) return candidate;
    return {};
}

}  // namespace

bool PythonLanguage::handlesFile(const std::string& file_id) const
{
    static const std::string kExt = ".py";
    return file_id.size() > kExt.size() &&
           file_id.compare(file_id.size() - kExt.size(), kExt.size(), kExt) == 0;
}

ProjectIndex PythonLanguage::buildIndex(std::unordered_set<std::string> file_ids) const
{
    ProjectIndex index;
    index.package_roots = inferPackageRoots(file_ids);
    index.file_ids      = std::move(file_ids);
    return index;
}

std::vector<std::string> PythonLanguage::inferPackageRoots(
    const std::unordered_set<std::string>& file_ids, int max_depth)
{
    static const std::string kInit = "/__init__.py";

    std::unordered_set<std::string> packages;
    for (const std::string& fid : file_ids) {
        if (fid.size() > kInit.size() &&
            fid.compare(fid.size() - kInit.size(), kInit.size(), kInit) == 0)
            packages.insert(fid.substr(0, fid.size() - kInit.size()));
    }

    std::unordered_set<std::string> roots{""};
    for (const std::string& pkg : packages) {
        const std::string parent = dir_of(pkg);
        if (parent.empty()) continue;              // already at the repo root
        if (packages.count(parent)) continue;      // a nested package, not top-level
        int depth = 1;
        for (char c : parent) if (c == '/') ++depth;
        if (depth > max_depth) continue;           // too deep to be a layout root
        roots.insert(parent + "/");
    }

    std::vector<std::string> out(roots.begin(), roots.end());
    // Shortest first so "" wins whenever the repo is root-layout; ties broken
    // lexicographically to keep the graph reproducible across runs.
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
        return a.size() != b.size() ? a.size() < b.size() : a < b;
    });
    return out;
}

std::string PythonLanguage::resolveModule(const std::string& spec,
                                          const std::string& from_file_id,
                                          const ProjectIndex& index) const
{
    const std::unordered_set<std::string>& file_ids = index.file_ids;
    if (spec.empty()) return {};

    if (spec[0] == '.') {
        // Relative: resolve against the importing file's own directory, so the
        // layout roots below play no part -- the anchor is already inside the tree.
        std::size_t dots = 0;
        while (dots < spec.size() && spec[dots] == '.') ++dots;

        std::string base = dir_of(from_file_id);
        // One dot is the current package (the directory the file sits in, which
        // for pkg/__init__.py is pkg itself); each extra dot climbs one level.
        if (!climb(base, static_cast<int>(dots) - 1)) return {};

        const std::string tail = spec.substr(dots);
        if (tail.empty()) {
            // `from . import x` -- the target is the package itself.
            if (base.empty()) return {};
            const std::string init = base + "/__init__.py";
            return file_ids.count(init) ? init : std::string();
        }
        return file_for(join_module(base, tail), file_ids);
    }

    // Absolute: try each layout root, shortest first.
    for (const std::string& root : index.package_roots) {
        std::string hit = file_for(join_module(root.empty() ? std::string()
                                                            : root.substr(0, root.size() - 1),
                                               spec),
                                    file_ids);
        if (!hit.empty()) return hit;
    }
    return {};
}

bool PythonLanguage::isBuiltin(const std::string& name) const
{
    // dir(builtins) on CPython 3.12 (unchanged from 3.11), minus the module's own
    // attributes (__name__, __doc__, __package__, __loader__, __spec__): a bare
    // `__name__` in user code is the importing module's global, not a builtin.
    // __import__, __build_class__ and __debug__ stay -- they are real builtin
    // bindings and `__import__(...)` does appear as a call. exit/quit/help/
    // copyright/credits/license come from `site`, which a normal interpreter
    // always imports, so code calling them is not calling a project symbol.
    static const std::unordered_set<std::string> kBuiltins = {
        // constants
        "False", "None", "True", "Ellipsis", "NotImplemented", "__debug__",
        // functions and types
        "__build_class__", "__import__",
        "abs", "aiter", "all", "anext", "any", "ascii", "bin", "bool",
        "breakpoint", "bytearray", "bytes", "callable", "chr", "classmethod",
        "compile", "complex", "copyright", "credits", "delattr", "dict", "dir",
        "divmod", "enumerate", "eval", "exec", "exit", "filter", "float",
        "format", "frozenset", "getattr", "globals", "hasattr", "hash", "help",
        "hex", "id", "input", "int", "isinstance", "issubclass", "iter", "len",
        "license", "list", "locals", "map", "max", "memoryview", "min", "next",
        "object", "oct", "open", "ord", "pow", "print", "property", "quit",
        "range", "repr", "reversed", "round", "set", "setattr", "slice",
        "sorted", "staticmethod", "str", "sum", "super", "tuple", "type",
        "vars", "zip",
        // exceptions and warnings
        "ArithmeticError", "AssertionError", "AttributeError", "BaseException",
        "BaseExceptionGroup", "BlockingIOError", "BrokenPipeError",
        "BufferError", "BytesWarning", "ChildProcessError",
        "ConnectionAbortedError", "ConnectionError", "ConnectionRefusedError",
        "ConnectionResetError", "DeprecationWarning", "EOFError",
        "EncodingWarning", "EnvironmentError", "Exception", "ExceptionGroup",
        "FileExistsError", "FileNotFoundError", "FloatingPointError",
        "FutureWarning", "GeneratorExit", "IOError", "ImportError",
        "ImportWarning", "IndentationError", "IndexError", "InterruptedError",
        "IsADirectoryError", "KeyError", "KeyboardInterrupt", "LookupError",
        "MemoryError", "ModuleNotFoundError", "NameError", "NotADirectoryError",
        "NotImplementedError", "OSError", "OverflowError",
        "PendingDeprecationWarning", "PermissionError", "ProcessLookupError",
        "RecursionError", "ReferenceError", "ResourceWarning", "RuntimeError",
        "RuntimeWarning", "StopAsyncIteration", "StopIteration", "SyntaxError",
        "SyntaxWarning", "SystemError", "SystemExit", "TabError",
        "TimeoutError", "TypeError", "UnboundLocalError", "UnicodeDecodeError",
        "UnicodeEncodeError", "UnicodeError", "UnicodeTranslateError",
        "UnicodeWarning", "UserWarning", "ValueError", "Warning",
        "ZeroDivisionError",
    };
    return kBuiltins.count(name) != 0;
}

std::vector<std::string> PythonLanguage::parentScopes(
    const std::vector<LexicalScope>& chain) const
{
    std::vector<std::string> out;
    for (size_t i = 1; i < chain.size(); ++i) {
        if (chain[i].kind == "class") continue;
        out.push_back(chain[i].id);
    }
    out.push_back(kBuiltinScope);
    return out;
}
