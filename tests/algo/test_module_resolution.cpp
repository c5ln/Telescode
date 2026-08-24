#include "algo/Graph.h"
#include "db/db.h"

#include <gtest/gtest.h>
#include <sqlite3.h>

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
    return resolveModule(module, source, ids, inferPackageRoots(ids));
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
    EXPECT_EQ(inferPackageRoots(kPkg), (std::vector<std::string>{""}));
}

TEST(InferPackageRoots, SrcLayoutIsDiscovered)
{
    EXPECT_EQ(inferPackageRoots(kSrcLayout), (std::vector<std::string>{"", "src/"}));
}

TEST(InferPackageRoots, RepoRootAlwaysComesFirst)
{
    // "" must sort first so a root-layout repo resolves exactly as before.
    auto roots = inferPackageRoots(kSrcLayout);
    ASSERT_FALSE(roots.empty());
    EXPECT_EQ(roots.front(), "");
}

TEST(InferPackageRoots, NestedPackagesAreNotRoots)
{
    // src/_pytest/ contains packages, but it is itself a package, so it is not
    // a layout root -- otherwise "mark" alone would resolve.
    auto roots = inferPackageRoots(kSrcLayout);
    EXPECT_EQ(std::count(roots.begin(), roots.end(), "src/_pytest/"), 0);
    EXPECT_EQ(resolveModule("mark", "src/pytest.py", kSrcLayout, roots), "");
}

TEST(InferPackageRoots, DepthCapKeepsFixtureTreesOut)
{
    std::unordered_set<std::string> ids = {
        "src/app/__init__.py",
        "testing/fixtures/deep/nested/proj/__init__.py",
    };
    // "testing/fixtures/deep/nested/" is 4 deep and must not become a root.
    EXPECT_EQ(inferPackageRoots(ids, /*max_depth=*/1),
              (std::vector<std::string>{"", "src/"}));
}

TEST(ResolveModule, SrcLayoutAbsoluteImports)
{
    auto roots = inferPackageRoots(kSrcLayout);
    EXPECT_EQ(resolveModule("_pytest.logging", "testing/test_logging.py", kSrcLayout, roots),
              "src/_pytest/logging.py");
    EXPECT_EQ(resolveModule("_pytest.config", "src/pytest.py", kSrcLayout, roots),
              "src/_pytest/config/__init__.py");
    EXPECT_EQ(resolveModule("pytest", "testing/test_logging.py", kSrcLayout, roots),
              "src/pytest.py");
    EXPECT_EQ(resolveModule("_pytest.mark.structures", "src/pytest.py", kSrcLayout, roots),
              "src/_pytest/mark/structures.py");
}

TEST(ResolveModule, SrcLayoutRelativeImports)
{
    auto roots = inferPackageRoots(kSrcLayout);
    // Relative imports anchor on the source file, so src/ never enters into it.
    EXPECT_EQ(resolveModule(".structures", "src/_pytest/mark/__init__.py", kSrcLayout, roots),
              "src/_pytest/mark/structures.py");
    EXPECT_EQ(resolveModule("..logging", "src/_pytest/mark/structures.py", kSrcLayout, roots),
              "src/_pytest/logging.py");
    EXPECT_EQ(resolveModule(".", "src/_pytest/logging.py", kSrcLayout, roots),
              "src/_pytest/__init__.py");
}

TEST(ResolveModule, SrcLayoutStillRejectsExternals)
{
    auto roots = inferPackageRoots(kSrcLayout);
    EXPECT_EQ(resolveModule("os",     "src/pytest.py", kSrcLayout, roots), "");
    EXPECT_EQ(resolveModule("attr",   "src/pytest.py", kSrcLayout, roots), "");
    EXPECT_EQ(resolveModule("numpy",  "src/pytest.py", kSrcLayout, roots), "");
}

// ── Through GraphBuilder ──────────────────────────────────────────────────────
// The unit tests above cannot catch a wiring mistake: resolveModule needs the
// importing file, and build_file_graph has to hand it the right one.

namespace {

void exec_sql(sqlite3* db, const char* sql)
{
    char* err = nullptr;
    ASSERT_EQ(sqlite3_exec(db, sql, nullptr, nullptr, &err), SQLITE_OK) << (err ? err : "");
    sqlite3_free(err);
}

bool has_edge(const Graph& g, const std::string& src, const std::string& tgt)
{
    auto u = g.id_to_node.find(src);
    auto v = g.id_to_node.find(tgt);
    if (u == g.id_to_node.end() || v == g.id_to_node.end()) return false;
    for (NodeId w : g.adj[u->second]) if (w == v->second) return true;
    return false;
}

}  // namespace

TEST(GraphBuilderImports, RelativeImportsBecomeEdges)
{
    sqlite3* db = nullptr;
    ASSERT_EQ(initDb(":memory:", &db), SQLITE_OK);

    exec_sql(db,
        "INSERT INTO file(file_id, file_name, language, raw_loc) VALUES"
        " ('pkg/__init__.py','__init__.py','python',10),"
        " ('pkg/utils.py','utils.py','python',10),"
        " ('pkg/core/__init__.py','__init__.py','python',10),"
        " ('pkg/core/dataset.py','dataset.py','python',10),"
        " ('pkg/core/variable.py','variable.py','python',10),"
        " ('absmod.py','absmod.py','python',10);");

    exec_sql(db,
        "INSERT INTO link(source_id, target_id, link_type) VALUES"
        " ('pkg/__init__.py','.core','IMPORTS'),"
        " ('pkg/core/dataset.py','.variable','IMPORTS'),"
        " ('pkg/core/dataset.py','..utils','IMPORTS'),"
        " ('pkg/core/variable.py','.','IMPORTS'),"
        " ('absmod.py','pkg.core.dataset','IMPORTS'),"
        " ('absmod.py','os','IMPORTS'),"
        " ('pkg/core/dataset.py','...escape','IMPORTS');");

    auto gbr = GraphBuilder::build(db, AlgoConfig{});
    const Graph& g = gbr.file_graph;

    EXPECT_TRUE(has_edge(g, "pkg/__init__.py",      "pkg/core/__init__.py"));
    EXPECT_TRUE(has_edge(g, "pkg/core/dataset.py",  "pkg/core/variable.py"));
    EXPECT_TRUE(has_edge(g, "pkg/core/dataset.py",  "pkg/utils.py"));
    EXPECT_TRUE(has_edge(g, "pkg/core/variable.py", "pkg/core/__init__.py"));
    EXPECT_TRUE(has_edge(g, "absmod.py",            "pkg/core/dataset.py"));

    // `os` is external and `...escape` climbs above the repo root: neither adds
    // a node, so the graph holds exactly the six project files.
    EXPECT_EQ(g.id_to_node.count("os"), 0u);
    EXPECT_EQ(g.size(), 6);

    sqlite3_close(db);
}

TEST(GraphBuilderImports, SrcLayoutImportsBecomeEdges)
{
    sqlite3* db = nullptr;
    ASSERT_EQ(initDb(":memory:", &db), SQLITE_OK);

    exec_sql(db,
        "INSERT INTO file(file_id, file_name, language, raw_loc) VALUES"
        " ('src/pytest.py','pytest.py','python',10),"
        " ('src/_pytest/__init__.py','__init__.py','python',10),"
        " ('src/_pytest/logging.py','logging.py','python',10),"
        " ('testing/test_logging.py','test_logging.py','python',10);");

    exec_sql(db,
        "INSERT INTO link(source_id, target_id, link_type) VALUES"
        " ('testing/test_logging.py','_pytest.logging','IMPORTS'),"
        " ('src/pytest.py','_pytest.logging','IMPORTS'),"
        " ('src/_pytest/logging.py','.','IMPORTS'),"
        " ('testing/test_logging.py','numpy','IMPORTS');");

    auto gbr = GraphBuilder::build(db, AlgoConfig{});
    const Graph& g = gbr.file_graph;

    EXPECT_TRUE(has_edge(g, "testing/test_logging.py", "src/_pytest/logging.py"));
    EXPECT_TRUE(has_edge(g, "src/pytest.py",           "src/_pytest/logging.py"));
    EXPECT_TRUE(has_edge(g, "src/_pytest/logging.py",  "src/_pytest/__init__.py"));
    EXPECT_EQ(g.id_to_node.count("numpy"), 0u);

    sqlite3_close(db);
}
