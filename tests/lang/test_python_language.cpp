#include "lang/python/PythonLanguage.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

// A package tree with a subpackage, mirroring the fixture the bug was found on.
//   pkg/__init__.py
//   pkg/utils.py
//   pkg/core/__init__.py
//   pkg/core/dataset.py
//   pkg/core/variable.py
//   absmod.py           (top-level, flat)
const std::unordered_set<std::string> kPkg = {
    "pkg/__init__.py",
    "pkg/utils.py",
    "pkg/core/__init__.py",
    "pkg/core/dataset.py",
    "pkg/core/variable.py",
    "absmod.py",
};

std::string R(const std::string& module, const std::string& source,
              const std::unordered_set<std::string>& ids = kPkg)
{
    const PythonLanguage python;
    return python.resolveModule(module, source, python.buildIndex(ids));
}

}  // namespace

// ── Relative imports ──────────────────────────────────────────────────────────
// The parser records these verbatim (".core", "..utils", "."), so a plain
// '.' -> '/' substitution produced "/core.py" / "//utils.py" and every relative
// import in the repo was dropped.

TEST(ResolveModule, SingleDotSubmodule)
{
    // from .variable import X   inside pkg/core/dataset.py
    EXPECT_EQ(R(".variable", "pkg/core/dataset.py"), "pkg/core/variable.py");
}

TEST(ResolveModule, SingleDotFromPackageInit)
{
    // from .core import X   inside pkg/__init__.py
    // For an __init__.py the current package is its own directory, not the parent.
    EXPECT_EQ(R(".core", "pkg/__init__.py"), "pkg/core/__init__.py");
    EXPECT_EQ(R(".utils", "pkg/__init__.py"), "pkg/utils.py");
}

TEST(ResolveModule, DoubleDotClimbsOneLevel)
{
    // from ..utils import X   inside pkg/core/dataset.py
    EXPECT_EQ(R("..utils", "pkg/core/dataset.py"), "pkg/utils.py");
}

TEST(ResolveModule, BareDotIsTheCurrentPackageInit)
{
    // from . import X   -- recorded as a lone "."
    EXPECT_EQ(R(".", "pkg/core/dataset.py"), "pkg/core/__init__.py");
    EXPECT_EQ(R(".", "pkg/utils.py"),        "pkg/__init__.py");
    // From the package's own __init__.py, "." is that same __init__.py; a
    // self-edge, which add_edge drops.
    EXPECT_EQ(R(".", "pkg/__init__.py"),     "pkg/__init__.py");
}

TEST(ResolveModule, BareDoubleDotIsTheParentPackageInit)
{
    // from .. import X   inside pkg/core/dataset.py
    EXPECT_EQ(R("..", "pkg/core/dataset.py"), "pkg/__init__.py");
}

TEST(ResolveModule, RelativeTargetResolvingToASubpackage)
{
    // from .core import X  -> the package's __init__.py, not a module file
    EXPECT_EQ(R(".core", "pkg/__init__.py"), "pkg/core/__init__.py");
}

TEST(ResolveModule, DottedRelativeTail)
{
    // from .core.dataset import X   inside pkg/__init__.py
    EXPECT_EQ(R(".core.dataset", "pkg/__init__.py"), "pkg/core/dataset.py");
    // from ..core.variable import X  inside pkg/core/dataset.py
    EXPECT_EQ(R("..core.variable", "pkg/core/dataset.py"), "pkg/core/variable.py");
}

// ── Climbing past the repo root ───────────────────────────────────────────────
// Unresolvable, not an error: the same thing that happens to an external module.

TEST(ResolveModule, ClimbingAboveRepoRootFails)
{
    EXPECT_EQ(R("..utils", "absmod.py"),           "");  // no parent to climb to
    EXPECT_EQ(R("...utils", "pkg/core/dataset.py"), "");  // two levels above pkg/
    EXPECT_EQ(R("..",       "absmod.py"),           "");
}

TEST(ResolveModule, BareDotAtRepoRootFails)
{
    // A top-level module has no package, so `from . import x` has no anchor.
    EXPECT_EQ(R(".", "absmod.py"), "");
}

TEST(ResolveModule, RelativeToAMissingFileFails)
{
    EXPECT_EQ(R(".nosuch", "pkg/core/dataset.py"), "");
}

// ── Absolute imports still work ───────────────────────────────────────────────

TEST(ResolveModule, AbsoluteModuleFile)
{
    EXPECT_EQ(R("pkg.core.dataset", "absmod.py"), "pkg/core/dataset.py");
    EXPECT_EQ(R("pkg.utils",        "absmod.py"), "pkg/utils.py");
}

TEST(ResolveModule, AbsolutePackageInit)
{
    EXPECT_EQ(R("pkg",      "absmod.py"), "pkg/__init__.py");
    EXPECT_EQ(R("pkg.core", "absmod.py"), "pkg/core/__init__.py");
}

TEST(ResolveModule, ExternalModulesStayUnresolved)
{
    EXPECT_EQ(R("os",     "pkg/core/dataset.py"), "");
    EXPECT_EQ(R("sys",    "absmod.py"),           "");
    EXPECT_EQ(R("numpy",  "pkg/__init__.py"),     "");
    EXPECT_EQ(R("os.path", "absmod.py"),          "");
}

TEST(ResolveModule, EmptyModuleIsUnresolved)
{
    EXPECT_EQ(R("", "pkg/core/dataset.py"), "");
}

// ── src-layout (inferPackageRoots) ────────────────────────────────────────────
// pytest et al. keep their package under src/, so `_pytest.logging` is
// src/_pytest/logging.py and a root-anchored lookup finds nothing.

namespace {
const std::unordered_set<std::string> kSrcLayout = {
    "src/pytest.py",
    "src/_pytest/__init__.py",
    "src/_pytest/logging.py",
    "src/_pytest/config/__init__.py",
    "src/_pytest/mark/__init__.py",
    "src/_pytest/mark/structures.py",
    "testing/test_logging.py",
    "setup.py",
};
}  // namespace

TEST(InferPackageRoots, FlatRepoHasOnlyTheRepoRoot)
{
    EXPECT_EQ(PythonLanguage::inferPackageRoots(kPkg), (std::vector<std::string>{""}));
}

TEST(InferPackageRoots, SrcLayoutIsDiscovered)
{
    EXPECT_EQ(PythonLanguage::inferPackageRoots(kSrcLayout), (std::vector<std::string>{"", "src/"}));
}

TEST(InferPackageRoots, RepoRootAlwaysComesFirst)
{
    // "" must sort first so a root-layout repo resolves exactly as before.
    auto roots = PythonLanguage::inferPackageRoots(kSrcLayout);
    ASSERT_FALSE(roots.empty());
    EXPECT_EQ(roots.front(), "");
}

TEST(InferPackageRoots, NestedPackagesAreNotRoots)
{
    // src/_pytest/ contains packages, but it is itself a package, so it is not
    // a layout root -- otherwise "mark" alone would resolve.
    auto roots = PythonLanguage::inferPackageRoots(kSrcLayout);
    EXPECT_EQ(std::count(roots.begin(), roots.end(), "src/_pytest/"), 0);
    EXPECT_EQ(R("mark", "src/pytest.py", kSrcLayout), "");
}

TEST(InferPackageRoots, DepthCapKeepsFixtureTreesOut)
{
    std::unordered_set<std::string> ids = {
        "src/app/__init__.py",
        "testing/fixtures/deep/nested/proj/__init__.py",
    };
    // "testing/fixtures/deep/nested/" is 4 deep and must not become a root.
    EXPECT_EQ(PythonLanguage::inferPackageRoots(ids, /*max_depth=*/1),
              (std::vector<std::string>{"", "src/"}));
}

TEST(ResolveModule, SrcLayoutAbsoluteImports)
{
    EXPECT_EQ(R("_pytest.logging", "testing/test_logging.py", kSrcLayout),
              "src/_pytest/logging.py");
    EXPECT_EQ(R("_pytest.config", "src/pytest.py", kSrcLayout),
              "src/_pytest/config/__init__.py");
    EXPECT_EQ(R("pytest", "testing/test_logging.py", kSrcLayout),
              "src/pytest.py");
    EXPECT_EQ(R("_pytest.mark.structures", "src/pytest.py", kSrcLayout),
              "src/_pytest/mark/structures.py");
}

TEST(ResolveModule, SrcLayoutRelativeImports)
{
    // Relative imports anchor on the source file, so src/ never enters into it.
    EXPECT_EQ(R(".structures", "src/_pytest/mark/__init__.py", kSrcLayout),
              "src/_pytest/mark/structures.py");
    EXPECT_EQ(R("..logging", "src/_pytest/mark/structures.py", kSrcLayout),
              "src/_pytest/logging.py");
    EXPECT_EQ(R(".", "src/_pytest/logging.py", kSrcLayout),
              "src/_pytest/__init__.py");
}

TEST(ResolveModule, SrcLayoutStillRejectsExternals)
{
    EXPECT_EQ(R("os",     "src/pytest.py", kSrcLayout), "");
    EXPECT_EQ(R("attr",   "src/pytest.py", kSrcLayout), "");
    EXPECT_EQ(R("numpy",  "src/pytest.py", kSrcLayout), "");
}

// ── Builtins ──────────────────────────────────────────────────────────────────
// The outermost scope: a bare `list(...)` that no project scope binds is the
// builtin, never some project's `Installer.list`.

TEST(PythonBuiltins, BuiltinFunctionsAndTypes)
{
    const PythonLanguage python;
    EXPECT_TRUE(python.isBuiltin("list"));
    EXPECT_TRUE(python.isBuiltin("len"));
    EXPECT_TRUE(python.isBuiltin("print"));
    EXPECT_TRUE(python.isBuiltin("ValueError"));
    EXPECT_TRUE(python.isBuiltin("__import__"));
}

TEST(PythonBuiltins, ProjectNamesAreNotBuiltins)
{
    const PythonLanguage python;
    EXPECT_FALSE(python.isBuiltin("foo"));
    EXPECT_FALSE(python.isBuiltin("Installer"));
    EXPECT_FALSE(python.isBuiltin("List"));       // case-sensitive
    EXPECT_FALSE(python.isBuiltin(""));
}

TEST(PythonBuiltins, ModuleAttributesAreNotBuiltins)
{
    // `__name__` in user code is the module's own global.
    const PythonLanguage python;
    EXPECT_FALSE(python.isBuiltin("__name__"));
    EXPECT_FALSE(python.isBuiltin("__doc__"));
}

TEST(PythonLanguage, HandlesPythonFilesOnly)
{
    const PythonLanguage python;
    EXPECT_STREQ(python.name(), "python");
    EXPECT_TRUE(python.handlesFile("pkg/core/dataset.py"));
    EXPECT_FALSE(python.handlesFile("README.md"));
    EXPECT_FALSE(python.handlesFile("pkg/core/dataset.pyc"));
}

// ── Scope ladder ──────────────────────────────────────────────────────────────
// Class bodies are not visible from the scopes nested in them: inside a method,
// `list` is the builtin even when the class defines a `list` method.

namespace {

std::vector<std::string> P(const std::vector<LexicalScope>& chain)
{
    const PythonLanguage python;
    return python.parentScopes(chain);
}

}  // namespace

TEST(PythonParentScopes, FileScopeClimbsToBuiltins)
{
    EXPECT_EQ(P({{"m.py", "file"}}),
              (std::vector<std::string>{kBuiltinScope}));
}

TEST(PythonParentScopes, MethodSkipsItsClass)
{
    EXPECT_EQ(P({{"m.py::B::run", "function"}, {"m.py::B", "class"}, {"m.py", "file"}}),
              (std::vector<std::string>{"m.py", kBuiltinScope}));
}

TEST(PythonParentScopes, ClassBodySeesEnclosingScope)
{
    // A class body is searched itself (it is chain[0]) and then its parents.
    EXPECT_EQ(P({{"m.py::B", "class"}, {"m.py", "file"}}),
              (std::vector<std::string>{"m.py", kBuiltinScope}));
    // ...including an enclosing function, whose locals a class body can read.
    EXPECT_EQ(P({{"m.py::Local", "class"}, {"m.py::f", "function"}, {"m.py", "file"}}),
              (std::vector<std::string>{"m.py::f", "m.py", kBuiltinScope}));
}

TEST(PythonParentScopes, NestedFunctionSeesEnclosingFunction)
{
    EXPECT_EQ(P({{"m.py::f::g", "function"}, {"m.py::f", "function"}, {"m.py", "file"}}),
              (std::vector<std::string>{"m.py::f", "m.py", kBuiltinScope}));
}

TEST(PythonParentScopes, EveryEnclosingClassIsSkipped)
{
    // Method of a class nested in a class, and a class nested in a class: the
    // outer class body is invisible to both.
    EXPECT_EQ(P({{"m.py::Inner::m", "function"}, {"m.py::Inner", "class"},
                 {"m.py::Outer", "class"}, {"m.py", "file"}}),
              (std::vector<std::string>{"m.py", kBuiltinScope}));
    EXPECT_EQ(P({{"m.py::Inner", "class"}, {"m.py::Outer", "class"}, {"m.py", "file"}}),
              (std::vector<std::string>{"m.py", kBuiltinScope}));
}
