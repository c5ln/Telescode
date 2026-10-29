// tests/resolve/test_resolver.cpp
// What Resolver concludes from tests/fixtures/resolution: import_resolution,
// the resolved_id/status/confidence of each interesting ref, and the CALLS /
// INHERITS / DECORATES links written from them. The fixture is scanned for real
// (PythonParser::parseDirectory + DbInserter::insertAll + Resolver::run), the
// same path `scan` takes. Lines are 0-indexed.
//
// tests/parser/test_python_extraction.cpp pins down the rows the resolver
// reads; the comments here only say why each answer is the right one.

#include "db/DbInserter.h"
#include "db/DbUpdater.h"
#include "db/db.h"
#include "parser/PythonParser.h"
#include "resolve/Resolver.h"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;

#ifndef TS_RESOLUTION_FIXTURE
#error "TS_RESOLUTION_FIXTURE must point at tests/fixtures/resolution"
#endif

namespace {

using Rows = std::vector<std::string>;

// A scanned and resolved copy of a fixture tree, in a database that deletes
// itself.
class ResolvedDb {
public:
    explicit ResolvedDb(const std::string& tag) {
        path_ = (fs::temp_directory_path() / ("ts_resolver_test_" + tag + ".db")).string();
        remove();
    }
    ~ResolvedDb() {
        close();
        remove();
    }

    void scan(const std::string& root = TS_RESOLUTION_FIXTURE) {
        PythonParser parser;
        const auto results = parser.parseDirectory(root, root);
        ASSERT_EQ(DbInserter::insertAll(path_.c_str(), results), SQLITE_OK);
        open();
        ASSERT_EQ(Resolver::run(db_, &stats_), SQLITE_OK);
    }

    void open() {
        close();
        ASSERT_EQ(initDb(path_.c_str(), &db_), SQLITE_OK);
    }

    void close() {
        if (db_) sqlite3_close(db_);
        db_ = nullptr;
    }

    // Each row as its columns joined by '|', NULL spelled out. Every `params`
    // entry is bound in order.
    Rows rows(const std::string& sql, const std::vector<std::string>& params = {}) const {
        Rows out;
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
            ADD_FAILURE() << "bad query: " << sql << ": " << sqlite3_errmsg(db_);
            return out;
        }
        for (size_t i = 0; i < params.size(); ++i)
            sqlite3_bind_text(stmt, (int)i + 1, params[i].c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            std::string row;
            for (int i = 0; i < sqlite3_column_count(stmt); ++i) {
                if (i) row += '|';
                const unsigned char* t = sqlite3_column_text(stmt, i);
                row += t ? reinterpret_cast<const char*>(t) : "NULL";
            }
            out.push_back(row);
        }
        sqlite3_finalize(stmt);
        return out;
    }

    int count(const std::string& sql) const {
        const Rows r = rows(sql);
        return r.empty() ? -1 : std::stoi(r[0]);
    }

    // resolved_id|status|confidence of the ref naming `path` on `line` of `file`.
    std::string ref(const std::string& file, int line, const std::string& path) const {
        const Rows r = rows("SELECT resolved_id, status, confidence FROM ref"
                            " WHERE file_id = ? AND line = ? AND name_path = ?;",
                            {file, std::to_string(line), path});
        if (r.size() != 1) return "<" + std::to_string(r.size()) + " refs>";
        return r[0];
    }

    // Every CALLS / INHERITS / DECORATES link, as type|source|target.
    Rows links() const {
        return rows("SELECT link_type, source_id, target_id FROM link"
                    " WHERE link_type <> 'IMPORTS' ORDER BY link_type, source_id, target_id;");
    }

    sqlite3* db() const { return db_; }
    const std::string& path() const { return path_; }
    const Resolver::Stats& stats() const { return stats_; }

private:
    void remove() {
        for (const char* suffix : {"", "-wal", "-shm"}) {
            std::error_code ec;
            fs::remove(path_ + suffix, ec);
        }
    }

    std::string path_;
    sqlite3* db_ = nullptr;
    Resolver::Stats stats_;
};

// Every link the fixture should produce, and no other.
const Rows kFixtureLinks = {
    "CALLS|resapp/allstar.py::use|resapp/util.py::helper",
    "CALLS|resapp/cases.py::Child::__init__|resapp/cases.py::Child::own",
    "CALLS|resapp/cases.py::Child::__init__|resapp/sub/helpers.py::B::list",
    "CALLS|resapp/cases.py::Child::own|resapp/cases.py::len",
    "CALLS|resapp/cases.py::Child::own|resapp/util.py::helper",
    "CALLS|resapp/cases.py::Closure::outer::inner|resapp/cases.py::Closure::target",
    "CALLS|resapp/cases.py::Grand::go|resapp/sub/helpers.py::B::build",
    "CALLS|resapp/cases.py::Typed::go|resapp/cases.py::Child::own",
    "CALLS|resapp/cases.py::install|resapp/cases.py::hook",
    "CALLS|resapp/cases.py::local_import|resapp/util.py::bump",
    "CALLS|resapp/cases.py::tagged|resapp/cases.py::Child::__init__",
    "CALLS|resapp/cases.py::walk|resapp/cases.py::Child::own",
    "CALLS|resapp/cases.py::walk|resapp/cases.py::Inner::deep",
    "CALLS|resapp/cases.py::walk|resapp/sub/helpers.py::cached",
    "CALLS|resapp/compat.py::use|resapp/util.py::helper",
    "CALLS|resapp/ctors.py::make|resapp/ctors.py::Own::__init__",
    "CALLS|resapp/ctors.py::make|resapp/ctors.py::Own::bm",
    "CALLS|resapp/ctoruse.py::use|resapp/ctors.py::Own::__init__",
    "CALLS|resapp/ctoruse.py::use|resapp/ctors.py::Own::bm",
    "CALLS|resapp/cycles.py::B::again|resapp/models.py::B::list",
    "CALLS|resapp/fallback.py::call|resapp/sub/helpers.py::cached",
    "CALLS|resapp/models.py::B::run|resapp/models.py::B::list",
    "CALLS|resapp/pkg2/__init__.py::run|resapp/pkg2/mod.py::f",
    "CALLS|resapp/sub/views.py::View::get|resapp/util.py::helper",
    "CALLS|resapp/sub/views.py::View::render|resapp/sub/helpers.py::cached",
    "CALLS|resapp/sub/views.py::index|resapp/util.py::helper",
    "CALLS|resapp/underexplicit.py::use|resapp/under.py::_priv",
    "CALLS|resapp/understar.py::use|resapp/under.py::pub",
    "DECORATES|resapp/sub/deco.py::tag|resapp/cases.py::tagged",
    "DECORATES|resapp/sub/views.py::route|resapp/sub/views.py::index",
    "INHERITS|resapp/cases.py::Child|resapp/sub/helpers.py::B",
    "INHERITS|resapp/cases.py::Grand|resapp/cases.py::Child",
    "INHERITS|resapp/cases.py::Plain|resapp/sub/helpers.py::B",
    "INHERITS|resapp/cases.py::Typed|resapp/cases.py::Child",
    "INHERITS|resapp/ctors.py::Derived|resapp/ctors.py::Own",
    "INHERITS|resapp/cycles.py::B|resapp/models.py::B",
    "INHERITS|resapp/cycles.py::P|resapp/cycles.py::Q",
    "INHERITS|resapp/cycles.py::Q|resapp/cycles.py::P",
    "INHERITS|resapp/sub/views.py::View|resapp/models.py::B",
    "INHERITS|resapp/sub/views.py::View|resapp/models.py::E",
};

class Resolution : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        db_ = new ResolvedDb("fixture");
        db_->scan();
    }
    static void TearDownTestSuite() {
        delete db_;
        db_ = nullptr;
    }
    static ResolvedDb* db_;
};

ResolvedDb* Resolution::db_ = nullptr;

}  // namespace

// ── import_resolution ─────────────────────────────────────────────────────────

TEST_F(Resolution, ImportResolution)
{
    // One row per distinct (file, module_spec) a binding names. Relative specs
    // anchor on the importing file; `.` and `..` are the package inits.
    EXPECT_EQ(db_->rows("SELECT file_id, module_spec, resolved_file_id, status"
                        "  FROM import_resolution ORDER BY file_id, module_spec;"),
              (Rows{
                  "resapp/__init__.py|.models|resapp/models.py|resolved",
                  "resapp/__init__.py|.util|resapp/util.py|resolved",
                  "resapp/allmod.py|.util|resapp/util.py|resolved",
                  "resapp/allstar.py|.allmod|resapp/allmod.py|resolved",
                  "resapp/cases.py|.sub|resapp/sub/__init__.py|resolved",
                  "resapp/cases.py|.sub.helpers|resapp/sub/helpers.py|resolved",
                  "resapp/cases.py|.util|resapp/util.py|resolved",
                  "resapp/cases.py|resapp|resapp/__init__.py|resolved",
                  "resapp/compat.py|.util|resapp/util.py|resolved",
                  "resapp/compat.py|xml.dom|NULL|external",
                  "resapp/compat.py|xml.sax|NULL|external",
                  "resapp/ctors.py|zarr.storage|NULL|external",
                  "resapp/ctoruse.py|.|resapp/__init__.py|resolved",
                  "resapp/cycles.py|.models|resapp/models.py|resolved",
                  "resapp/fallback.py|.sub.helpers|resapp/sub/helpers.py|resolved",
                  "resapp/fallback.py|.util|resapp/util.py|resolved",
                  "resapp/fallback.py|zarr.storage|NULL|external",
                  "resapp/missing.py|numpy|NULL|external",
                  "resapp/missing.py|resapp.gone|NULL|unresolved",
                  "resapp/models.py|collections|NULL|external",
                  "resapp/models.py|os.path|NULL|external",
                  "resapp/pkg2/__init__.py|.|resapp/pkg2/__init__.py|resolved",
                  "resapp/star.py|os.path|NULL|external",
                  "resapp/sub/helpers.py|functools|NULL|external",
                  "resapp/sub/views.py|.|resapp/sub/__init__.py|resolved",
                  "resapp/sub/views.py|..|resapp/__init__.py|resolved",
                  "resapp/sub/views.py|..models|resapp/models.py|resolved",
                  "resapp/sub/views.py|..util|resapp/util.py|resolved",
                  "resapp/sub/views.py|json|NULL|external",
                  "resapp/sub/views.py|resapp|resapp/__init__.py|resolved",
                  "resapp/sub/views.py|resapp.models|resapp/models.py|resolved",
                  "resapp/sub/views.py|resapp.sub.helpers|resapp/sub/helpers.py|resolved",
                  "resapp/under.py|os|NULL|external",
                  "resapp/underexplicit.py|.under|resapp/under.py|resolved",
                  "resapp/understar.py|.under|resapp/under.py|resolved",
              }));
}

// ── Imports ───────────────────────────────────────────────────────────────────

TEST_F(Resolution, AliasImports)
{
    // from ..models import B as C / import resapp.models as m
    EXPECT_EQ(db_->ref("resapp/sub/views.py", 21, "C"),   "resapp/models.py::B|resolved|exact");
    EXPECT_EQ(db_->ref("resapp/sub/views.py", 21, "m.E"), "resapp/models.py::E|resolved|exact");
}

TEST_F(Resolution, RelativeImports)
{
    // from .. import util -> util.helper: `util` is a submodule of the
    // package `..`, whose __init__ binds no `util`.
    EXPECT_EQ(db_->ref("resapp/sub/views.py", 24, "util.helper"),
              "resapp/util.py::helper|resolved|exact");
    // from .sub import helpers, likewise one level down.
    EXPECT_EQ(db_->ref("resapp/cases.py", 58, "helpers.cached"),
              "resapp/sub/helpers.py::cached|resolved|exact");
    EXPECT_EQ(db_->ref("resapp/cases.py", 29, "helpers.B"),
              "resapp/sub/helpers.py::B|resolved|exact");
}

TEST_F(Resolution, ReExportThroughPackageInit)
{
    // from resapp import PublicB: resapp/__init__.py re-exports models.B under
    // that name. A class with no __init__: resolved to the class, but no link.
    EXPECT_EQ(db_->ref("resapp/sub/views.py", 31, "PublicB"), "resapp/models.py::B|resolved|exact");
    // from resapp import helper: re-exported from util.
    EXPECT_EQ(db_->ref("resapp/cases.py", 21, "helper"), "resapp/util.py::helper|resolved|exact");
}

TEST_F(Resolution, WildcardImport)
{
    // from ..util import * supplies `helper` to views.index.
    EXPECT_EQ(db_->ref("resapp/sub/views.py", 18, "helper"), "resapp/util.py::helper|resolved|exact");
    // from os.path import * might supply `join`: external, not unresolved.
    EXPECT_EQ(db_->ref("resapp/star.py", 4, "join"), "NULL|external|exact");
}

TEST_F(Resolution, FunctionLocalImports)
{
    EXPECT_EQ(db_->ref("resapp/sub/views.py", 18, "dumps"), "NULL|external|exact");
    EXPECT_EQ(db_->ref("resapp/cases.py", 47, "bump"), "resapp/util.py::bump|resolved|exact");
}

TEST_F(Resolution, WildcardImportHonoursAll)
{
    // allmod.py imports helper and bump; its __all__ lists only helper.
    EXPECT_EQ(db_->ref("resapp/allstar.py", 4, "helper"), "resapp/util.py::helper|resolved|exact");
    EXPECT_EQ(db_->ref("resapp/allstar.py", 5, "bump"), "NULL|unresolved|NULL");
}

TEST_F(Resolution, SubmoduleImportedInsideItsPackageInit)
{
    // pkg2/__init__.py: `from . import mod` names the package itself, which
    // binds `mod` only through this very import -- so it is the submodule.
    EXPECT_EQ(db_->ref("resapp/pkg2/__init__.py", 4, "mod.f"), "resapp/pkg2/mod.py::f|resolved|exact");
}

TEST_F(Resolution, MissingProjectModuleIsUnresolvedNotExternal)
{
    // missing.py: resapp.gone would be a module of the project package resapp
    // but does not exist; numpy is outside the project.
    EXPECT_EQ(db_->rows("SELECT module_spec, status FROM import_resolution"
                        " WHERE file_id = 'resapp/missing.py' ORDER BY module_spec;"),
              (Rows{"numpy|external", "resapp.gone|unresolved"}));
    EXPECT_EQ(db_->ref("resapp/missing.py", 7, "numpy.zeros"), "NULL|external|exact");
    EXPECT_EQ(db_->ref("resapp/missing.py", 8, "resapp.gone.f"), "NULL|unresolved|NULL");
    EXPECT_EQ(db_->ref("resapp/missing.py", 9, "thing"), "NULL|unresolved|NULL");
}

TEST_F(Resolution, WildcardImportSkipsUnderscoreNamesWithoutAll)
{
    // under.py has no __all__; understar.py imports it with `*`.
    EXPECT_EQ(db_->ref("resapp/understar.py", 4, "pub"), "resapp/under.py::pub|resolved|exact");
    EXPECT_EQ(db_->ref("resapp/understar.py", 5, "_priv"), "NULL|unresolved|NULL");
    EXPECT_EQ(db_->ref("resapp/understar.py", 6, "_os.getcwd"), "NULL|unresolved|NULL");
    // Asked for by name, an underscore name is imported as usual.
    EXPECT_EQ(db_->ref("resapp/underexplicit.py", 4, "_priv"), "resapp/under.py::_priv|resolved|exact");
}

TEST_F(Resolution, ImportOfADottedModule)
{
    // import resapp.sub.helpers binds `resapp`; the rest walks submodules.
    EXPECT_EQ(db_->ref("resapp/sub/views.py", 27, "resapp.sub.helpers.cached"),
              "resapp/sub/helpers.py::cached|resolved|exact");
}

TEST_F(Resolution, FallbackImportPrefersTheProjectFunction)
{
    // try: from .util import helper / except ImportError: helper = None -- the
    // import reaches a function, the assignment only a variable. A choice
    // between two answers, so heuristic.
    EXPECT_EQ(db_->ref("resapp/compat.py", 13, "helper"), "resapp/util.py::helper|resolved|heuristic");
}

TEST_F(Resolution, FallbackBetweenTwoProjectFunctions)
{
    // try: from .util import helper as impl / except: from .sub.helpers import
    // cached as impl -- equal rank, so the last in source order.
    EXPECT_EQ(db_->ref("resapp/fallback.py", 13, "impl"),
              "resapp/sub/helpers.py::cached|resolved|heuristic");
}

TEST_F(Resolution, FallbackVariableLosesToAnExternalImport)
{
    // try: from zarr.storage import WrapperStore / except ImportError:
    // WrapperStore = object -- the base is zarr's class, not the variable.
    EXPECT_EQ(db_->ref("resapp/fallback.py", 16, "WrapperStore"), "NULL|external|heuristic");
    EXPECT_EQ(db_->count("SELECT COUNT(*) FROM link WHERE link_type = 'INHERITS'"
                         "   AND source_id = 'resapp/fallback.py::Store';"), 0);
}

// ── Builtins and shadowing ────────────────────────────────────────────────────

TEST_F(Resolution, BuiltinsAndShadowing)
{
    EXPECT_EQ(db_->ref("resapp/util.py", 5, "len"), "NULL|builtin|exact");
    // `len = lambda ...` in local_len: a local with no symbol stops the search.
    EXPECT_EQ(db_->ref("resapp/util.py", 16, "len"), "NULL|unresolved|NULL");
    // `def len` at module level in cases.py shadows the builtin there.
    EXPECT_EQ(db_->ref("resapp/cases.py", 21, "len"), "resapp/cases.py::len|resolved|exact");
}

TEST_F(Resolution, MethodNamedListIsNotTheBareName)
{
    // `list(range(3))` inside helpers.B.list: the class body is not on the
    // method's scope ladder, so `list` is the builtin, not the method.
    EXPECT_EQ(db_->ref("resapp/sub/helpers.py", 7, "list"), "NULL|builtin|exact");
    // Every CALLS link to a `list` method comes from a `self.list()`.
    EXPECT_EQ(db_->rows("SELECT source_id, target_id FROM link WHERE link_type = 'CALLS'"
                        "   AND target_id LIKE '%::list' ORDER BY source_id;"),
              (Rows{
                  "resapp/cases.py::Child::__init__|resapp/sub/helpers.py::B::list",
                  "resapp/cycles.py::B::again|resapp/models.py::B::list",
                  "resapp/models.py::B::run|resapp/models.py::B::list",
              }));
}

TEST_F(Resolution, GlobalDeclarationLeavesTheModuleBinding)
{
    // install() declares `global hook` and assigns it: no local binding, so
    // `hook()` is the module's function.
    EXPECT_EQ(db_->ref("resapp/cases.py", 42, "hook"), "resapp/cases.py::hook|resolved|exact");
}

TEST_F(Resolution, UnknownNamesAreUnresolved)
{
    EXPECT_EQ(db_->ref("resapp/cases.py", 60, "missing"), "NULL|unresolved|NULL");
    // An attribute of a function: no type inference.
    EXPECT_EQ(db_->ref("resapp/cases.py", 59, "helper.cache_clear"), "NULL|unresolved|NULL");
}

// ── Classes ───────────────────────────────────────────────────────────────────

TEST_F(Resolution, TwoUnrelatedClassesNamedB)
{
    // views.View inherits models.B (as C); cases.Child and Plain inherit
    // helpers.B. Each child links to its own B only.
    EXPECT_EQ(db_->ref("resapp/cases.py", 15, "B"), "resapp/sub/helpers.py::B|resolved|exact");
    EXPECT_EQ(db_->rows("SELECT source_id, target_id FROM link"
                        " WHERE link_type = 'INHERITS' AND target_id LIKE '%::B'"
                        " ORDER BY source_id;"),
              (Rows{
                  "resapp/cases.py::Child|resapp/sub/helpers.py::B",
                  "resapp/cases.py::Plain|resapp/sub/helpers.py::B",
                  "resapp/cycles.py::B|resapp/models.py::B",
                  "resapp/sub/views.py::View|resapp/models.py::B",
              }));
}

TEST_F(Resolution, BuiltinBaseHasNoLink)
{
    EXPECT_EQ(db_->ref("resapp/models.py", 12, "Exception"), "NULL|builtin|exact");
    EXPECT_EQ(db_->count("SELECT COUNT(*) FROM link"
                         " WHERE link_type = 'INHERITS' AND source_id = 'resapp/models.py::E';"), 0);
    // INHERITS targets are class ids, never names.
    EXPECT_EQ(db_->count("SELECT COUNT(*) FROM link l WHERE l.link_type = 'INHERITS'"
                         "   AND l.target_id NOT IN (SELECT class_id FROM class);"), 0);
}

TEST_F(Resolution, GenericBaseIsItsClass)
{
    // class Typed(Child[int]): the subscript is dropped, the base is Child, and
    // methods are found on it.
    EXPECT_EQ(db_->ref("resapp/cases.py", 63, "Child"), "resapp/cases.py::Child|resolved|exact");
    EXPECT_EQ(db_->ref("resapp/cases.py", 65, "self.own"), "resapp/cases.py::Child::own|resolved|heuristic");
}

TEST_F(Resolution, ClassNamedLikeItsImportedBase)
{
    // from .models import B / class B(B): the base is the import, not the
    // class being defined, and its methods are reached from the subclass.
    EXPECT_EQ(db_->ref("resapp/cycles.py", 4, "B"), "resapp/models.py::B|resolved|exact");
    EXPECT_EQ(db_->ref("resapp/cycles.py", 6, "self.list"), "resapp/models.py::B::list|resolved|heuristic");
}

TEST_F(Resolution, InheritanceCycleTerminates)
{
    // class P(Q) / class Q(P): both links are written; a member on neither
    // ends the base search instead of looping.
    EXPECT_EQ(db_->ref("resapp/cycles.py", 9, "Q"), "resapp/cycles.py::Q|resolved|exact");
    EXPECT_EQ(db_->ref("resapp/cycles.py", 14, "P"), "resapp/cycles.py::P|resolved|exact");
    EXPECT_EQ(db_->ref("resapp/cycles.py", 11, "self.missing"), "NULL|unresolved|NULL");
}

TEST_F(Resolution, ClassCallRunsTheConstructor)
{
    // Child defines __init__; models.B and helpers.B do not, and neither does
    // anything they inherit from.
    EXPECT_EQ(db_->ref("resapp/cases.py", 52, "Child"), "resapp/cases.py::Child::__init__|resolved|exact");
    EXPECT_EQ(db_->ref("resapp/models.py", 17, "B"), "resapp/models.py::B|resolved|exact");
    EXPECT_EQ(db_->ref("resapp/sub/helpers.py", 11, "B"), "resapp/sub/helpers.py::B|resolved|exact");
    // CALLS links only reach functions and methods.
    EXPECT_EQ(db_->count("SELECT COUNT(*) FROM link WHERE link_type = 'CALLS'"
                         "   AND target_id NOT IN (SELECT function_id FROM function);"), 0);
}

TEST_F(Resolution, InheritedConstructor)
{
    // ctors.py: Own defines __init__ (exact); Derived(Own) inherits it
    // (heuristic); Bare has none (the class, no link); Ext's only base is
    // external, which may well define one.
    EXPECT_EQ(db_->ref("resapp/ctors.py", 25, "Own"), "resapp/ctors.py::Own::__init__|resolved|exact");
    EXPECT_EQ(db_->ref("resapp/ctors.py", 26, "Derived"),
              "resapp/ctors.py::Own::__init__|resolved|heuristic");
    EXPECT_EQ(db_->ref("resapp/ctors.py", 27, "Bare"), "resapp/ctors.py::Bare|resolved|exact");
    EXPECT_EQ(db_->ref("resapp/ctors.py", 28, "Ext"), "NULL|external|exact");
    // Through a module alias too.
    EXPECT_EQ(db_->ref("resapp/ctoruse.py", 4, "M.Derived"),
              "resapp/ctors.py::Own::__init__|resolved|heuristic");
}

TEST_F(Resolution, ClassAttributeFoundOnABase)
{
    // Derived.bm: Derived defines no bm, its base Own does.
    EXPECT_EQ(db_->ref("resapp/ctors.py", 29, "Derived.bm"), "resapp/ctors.py::Own::bm|resolved|heuristic");
    EXPECT_EQ(db_->ref("resapp/ctoruse.py", 5, "M.Derived.bm"),
              "resapp/ctors.py::Own::bm|resolved|heuristic");
}

TEST_F(Resolution, ClassMembers)
{
    EXPECT_EQ(db_->ref("resapp/cases.py", 57, "Child.own"), "resapp/cases.py::Child::own|resolved|exact");
    // A nested class is reached through its parent.
    EXPECT_EQ(db_->ref("resapp/cases.py", 56, "Outer.Inner.deep"),
              "resapp/cases.py::Inner::deep|resolved|exact");
}

TEST_F(Resolution, ReceiverMethods)
{
    // On the method's own class: exact.
    EXPECT_EQ(db_->ref("resapp/models.py", 9, "self.list"), "resapp/models.py::B::list|resolved|exact");
    EXPECT_EQ(db_->ref("resapp/cases.py", 18, "self.own"), "resapp/cases.py::Child::own|resolved|exact");
    // Found on a resolved base, one and two levels up: heuristic.
    EXPECT_EQ(db_->ref("resapp/cases.py", 17, "self.list"),
              "resapp/sub/helpers.py::B::list|resolved|heuristic");
    EXPECT_EQ(db_->ref("resapp/cases.py", 26, "self.build"),
              "resapp/sub/helpers.py::B::build|resolved|heuristic");
    // A closure in a method sees the method's `self`.
    EXPECT_EQ(db_->ref("resapp/cases.py", 71, "self.target"),
              "resapp/cases.py::Closure::target|resolved|exact");
    // `self` as a plain function's parameter is just a local.
    EXPECT_EQ(db_->ref("resapp/cases.py", 79, "self.own"), "NULL|unresolved|NULL");
}

// ── Decorators ────────────────────────────────────────────────────────────────

TEST_F(Resolution, Decorators)
{
    EXPECT_EQ(db_->ref("resapp/sub/views.py", 15, "route"), "resapp/sub/views.py::route|resolved|exact");
    EXPECT_EQ(db_->ref("resapp/cases.py", 50, "deco.tag"), "resapp/sub/deco.py::tag|resolved|exact");
    EXPECT_EQ(db_->ref("resapp/sub/helpers.py", 9, "staticmethod"), "NULL|builtin|exact");
    EXPECT_EQ(db_->ref("resapp/sub/helpers.py", 14, "functools.lru_cache"), "NULL|external|exact");
    // The link keeps its old direction: decorator -> decorated.
    EXPECT_EQ(db_->rows("SELECT source_id, target_id FROM link WHERE link_type = 'DECORATES'"
                        " ORDER BY source_id;"),
              (Rows{
                  "resapp/sub/deco.py::tag|resapp/cases.py::tagged",
                  "resapp/sub/views.py::route|resapp/sub/views.py::index",
              }));
}

// ── Whole scan ────────────────────────────────────────────────────────────────

TEST_F(Resolution, EveryLink)
{
    EXPECT_EQ(db_->links(), kFixtureLinks);
}

TEST_F(Resolution, EveryRefIsDecided)
{
    EXPECT_EQ(db_->count("SELECT COUNT(*) FROM ref WHERE status = 'pending';"), 0);
    EXPECT_EQ(db_->rows("SELECT DISTINCT resolver FROM ref;"), (Rows{Resolver::kName}));
    // resolved_id is set exactly when the status says resolved.
    EXPECT_EQ(db_->count("SELECT COUNT(*) FROM ref"
                         " WHERE (resolved_id IS NULL) <> (status <> 'resolved');"), 0);
    EXPECT_EQ(db_->stats().status.at("resolved") + db_->stats().status.at("builtin") +
              db_->stats().status.at("external") + db_->stats().status.at("unresolved"),
              db_->count("SELECT COUNT(*) FROM ref;"));
}

TEST_F(Resolution, ImportLinksAreStillTheParsers)
{
    // Raw module names, as before; the resolver leaves them alone.
    EXPECT_EQ(db_->rows("SELECT target_id FROM link WHERE link_type = 'IMPORTS'"
                        "   AND source_id = 'resapp/sub/views.py' ORDER BY target_id;"),
              (Rows{".", "..", "..models", "..util", "json", "resapp", "resapp.models",
                    "resapp.sub.helpers"}));
}

TEST(ResolverRerun, ReplacesStaleLinksAndIsIdempotent)
{
    ResolvedDb db("rerun");
    db.scan();
    // A link no ref supports, as an older scan reusing the file might leave.
    ASSERT_EQ(sqlite3_exec(db.db(),
        "INSERT INTO link VALUES('resapp/util.py::helper', 'resapp/models.py::B::list', 'CALLS');",
        nullptr, nullptr, nullptr), SQLITE_OK);
    ASSERT_EQ(Resolver::run(db.db()), SQLITE_OK);
    EXPECT_EQ(db.links(), kFixtureLinks);
}

// ── After DbUpdater ───────────────────────────────────────────────────────────
// Every update operation re-resolves the whole database, so links into and out
// of the changed file are right afterwards -- including links from files that
// were not re-parsed.

namespace {

// A private copy of the fixture tree, or a tree of the given files, that the
// test may edit.
class FixtureCopy {
public:
    explicit FixtureCopy(const std::string& tag)
        : root_((fs::temp_directory_path() / ("ts_resolver_fixture_" + tag)).string()) {
        fs::remove_all(root_);
        fs::copy(TS_RESOLUTION_FIXTURE, root_, fs::copy_options::recursive);
    }
    FixtureCopy(const std::string& tag, const std::map<std::string, std::string>& files)
        : root_((fs::temp_directory_path() / ("ts_resolver_fixture_" + tag)).string()) {
        fs::remove_all(root_);
        for (const auto& [fileId, text] : files) {
            fs::create_directories(fs::path(file(fileId)).parent_path());
            write(fileId, text);
        }
    }
    ~FixtureCopy() {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    const std::string& root() const { return root_; }
    std::string file(const std::string& fileId) const { return (fs::path(root_) / fileId).string(); }

    void write(const std::string& fileId, const std::string& text) const {
        std::ofstream(file(fileId), std::ios::trunc) << text;
    }

    // Every .py file, as a file_id.
    std::vector<std::string> fileIds() const {
        std::vector<std::string> out;
        for (const auto& e : fs::recursive_directory_iterator(root_))
            if (e.path().extension() == ".py")
                out.push_back(fs::relative(e.path(), root_).generic_string());
        std::sort(out.begin(), out.end());
        return out;
    }

private:
    std::string root_;
};

Rows without(Rows rows, const std::vector<std::string>& gone) {
    for (const std::string& g : gone) {
        auto it = std::find(rows.begin(), rows.end(), g);
        EXPECT_NE(it, rows.end()) << g;
        if (it != rows.end()) rows.erase(it);
    }
    return rows;
}

}  // namespace

TEST(ResolverAfterUpdate, UnchangedFileKeepsItsIncomingLinks)
{
    // deleteFileEntities drops every link into util.py; re-parsing util.py
    // alone could not bring back the ones from views, compat and cases.
    FixtureCopy tree("unchanged");
    ResolvedDb db("unchanged");
    db.scan(tree.root());
    db.close();

    PythonParser parser;
    ASSERT_EQ(DbUpdater::updateFile(db.path().c_str(),
                                    parser.parseFile(tree.file("resapp/util.py"), tree.root())),
              SQLITE_OK);
    db.open();
    EXPECT_EQ(db.links(), kFixtureLinks);
    EXPECT_EQ(db.count("SELECT COUNT(*) FROM ref WHERE status = 'pending';"), 0);
}

TEST(ResolverAfterUpdate, RenamedMethodDropsTheCallersLink)
{
    // helpers.B.list becomes B.items: Child.__init__'s `self.list()` in another,
    // un-reparsed file no longer finds it.
    FixtureCopy tree("renamed");
    ResolvedDb db("renamed");
    db.scan(tree.root());
    db.close();

    tree.write("resapp/sub/helpers.py",
               "import functools\n"
               "\n"
               "\n"
               "class B:\n"
               "    \"\"\"Unrelated to resapp.models.B.\"\"\"\n"
               "\n"
               "    def items(self):\n"
               "        return list(range(3))\n"
               "\n"
               "    @staticmethod\n"
               "    def build():\n"
               "        return B()\n"
               "\n"
               "\n"
               "@functools.lru_cache(maxsize=None)\n"
               "def cached():\n"
               "    return B().items()\n");
    PythonParser parser;
    ASSERT_EQ(DbUpdater::updateFile(db.path().c_str(),
                                    parser.parseFile(tree.file("resapp/sub/helpers.py"), tree.root())),
              SQLITE_OK);
    db.open();
    EXPECT_EQ(db.ref("resapp/cases.py", 17, "self.list"), "NULL|unresolved|NULL");
    EXPECT_EQ(db.links(),
              without(kFixtureLinks,
                      {"CALLS|resapp/cases.py::Child::__init__|resapp/sub/helpers.py::B::list"}));
}

TEST(ResolverAfterUpdate, DeletedModuleBreaksItsImporters)
{
    FixtureCopy tree("deleted");
    ResolvedDb db("deleted");
    db.scan(tree.root());
    db.close();

    ASSERT_EQ(DbUpdater::deleteFile(db.path().c_str(), "resapp/models.py"), SQLITE_OK);
    db.open();
    // A relative import with nothing behind it is broken, not external.
    EXPECT_EQ(db.rows("SELECT resolved_file_id, status FROM import_resolution"
                      " WHERE file_id = 'resapp/sub/views.py' AND module_spec = '..models';"),
              (Rows{"NULL|unresolved"}));
    EXPECT_EQ(db.ref("resapp/sub/views.py", 21, "C"), "NULL|unresolved|NULL");
    // `resapp.models` is absolute, but resapp is a project package: a module of
    // it that is gone is broken too, not external.
    EXPECT_EQ(db.rows("SELECT resolved_file_id, status FROM import_resolution"
                      " WHERE file_id = 'resapp/sub/views.py' AND module_spec = 'resapp.models';"),
              (Rows{"NULL|unresolved"}));
    EXPECT_EQ(db.ref("resapp/sub/views.py", 21, "m.E"), "NULL|unresolved|NULL");
    EXPECT_EQ(db.links(),
              without(kFixtureLinks, {
                  "CALLS|resapp/cycles.py::B::again|resapp/models.py::B::list",
                  "CALLS|resapp/models.py::B::run|resapp/models.py::B::list",
                  "INHERITS|resapp/cycles.py::B|resapp/models.py::B",
                  "INHERITS|resapp/sub/views.py::View|resapp/models.py::B",
                  "INHERITS|resapp/sub/views.py::View|resapp/models.py::E",
              }));
}

// ── Re-export depth and order independence ────────────────────────────────────

namespace {

// five1 .. five5 pass `deep` on to five6, which defines it: five hops from
// five1. six1 .. six6 likewise, six hops. loopa and loopb re-export each other.
std::map<std::string, std::string> reexportTree()
{
    std::map<std::string, std::string> files{{"pkg/__init__.py", ""}};
    for (int i = 1; i <= 5; ++i)
        files["pkg/five" + std::to_string(i) + ".py"] =
            "from .five" + std::to_string(i + 1) + " import deep\n";
    files["pkg/five6.py"] = "def deep():\n    return 0\n";
    for (int i = 1; i <= 6; ++i)
        files["pkg/six" + std::to_string(i) + ".py"] =
            "from .six" + std::to_string(i + 1) + " import deep\n";
    files["pkg/six7.py"] = "def deep():\n    return 0\n";
    files["pkg/loopa.py"] = "from .loopb import x\n";
    files["pkg/loopb.py"] = "from .loopa import x\n";
    // aaa needs all five hops, zzz four of the same chain; mid five hops of the
    // six chain, whose full six use.py needs. Whichever is resolved first must
    // not change another's answer.
    files["pkg/aaa.py"] = "from pkg.five1 import deep\n\ndef a():\n    return deep()\n";
    files["pkg/zzz.py"] = "from pkg.five2 import deep\n\ndef z():\n    return deep()\n";
    files["pkg/mid.py"] = "from pkg.six2 import deep\n\ndef m():\n    return deep()\n";
    // Y's base Derived.Inner exists only through Derived's own base, so it
    // must not matter whether Derived's INHERITS ref comes before Y's.
    files["pkg/lib.py"] =
        "class Base:\n"
        "    class Inner:\n"
        "        def hi(self):\n"
        "            return 0\n"
        "\n"
        "\n"
        "class Derived(Base):\n"
        "    pass\n";
    files["pkg/nested.py"] =
        "from .lib import Derived\n"
        "\n"
        "\n"
        "class Y(Derived.Inner):\n"
        "    def go(self):\n"
        "        return self.hi()\n";
    files["pkg/use.py"] =
        "from .six1 import deep as six\n"
        "from .loopa import x\n"
        "\n"
        "def u():\n"
        "    six()\n"
        "    return x()\n";
    return files;
}

// Re-parsing a file unchanged moves its refs to the end of the ref table. Do
// that for every file in turn; the links must never change.
void expectOrderIndependent(const FixtureCopy& tree, ResolvedDb& db)
{
    const Rows links = db.links();
    db.close();
    PythonParser parser;
    for (const std::string& fileId : tree.fileIds()) {
        ASSERT_EQ(DbUpdater::updateFile(db.path().c_str(),
                                        parser.parseFile(tree.file(fileId), tree.root())),
                  SQLITE_OK);
        db.open();
        EXPECT_EQ(db.links(), links) << "after updating " << fileId;
        db.close();
    }
    db.open();
}

}  // namespace

TEST(ResolverReexports, FiveHopsResolveSixDoNot)
{
    FixtureCopy tree("reexport", reexportTree());
    ResolvedDb db("reexport");
    db.scan(tree.root());
    EXPECT_EQ(db.ref("pkg/aaa.py", 3, "deep"), "pkg/five6.py::deep|resolved|exact");
    EXPECT_EQ(db.ref("pkg/zzz.py", 3, "deep"), "pkg/five6.py::deep|resolved|exact");
    EXPECT_EQ(db.ref("pkg/mid.py", 3, "deep"), "pkg/six7.py::deep|resolved|exact");
    EXPECT_EQ(db.ref("pkg/use.py", 4, "six"), "NULL|unresolved|NULL");
}

TEST(ResolverReexports, CycleIsUnresolved)
{
    FixtureCopy tree("cycle", reexportTree());
    ResolvedDb db("cycle");
    db.scan(tree.root());
    EXPECT_EQ(db.ref("pkg/use.py", 5, "x"), "NULL|unresolved|NULL");
}

TEST(ResolverOrder, ReexportChainsDoNotDependOnRefOrder)
{
    FixtureCopy tree("order_chain", reexportTree());
    ResolvedDb db("order_chain");
    db.scan(tree.root());
    ASSERT_EQ(db.links(), (Rows{
        "CALLS|pkg/aaa.py::a|pkg/five6.py::deep",
        "CALLS|pkg/mid.py::m|pkg/six7.py::deep",
        "CALLS|pkg/nested.py::Y::go|pkg/lib.py::Inner::hi",
        "CALLS|pkg/zzz.py::z|pkg/five6.py::deep",
        "INHERITS|pkg/lib.py::Derived|pkg/lib.py::Base",
        "INHERITS|pkg/nested.py::Y|pkg/lib.py::Inner",
    }));
    expectOrderIndependent(tree, db);
}

TEST(ResolverOrder, FixtureDoesNotDependOnRefOrder)
{
    FixtureCopy tree("order_fixture");
    ResolvedDb db("order_fixture");
    db.scan(tree.root());
    ASSERT_EQ(db.links(), kFixtureLinks);
    expectOrderIndependent(tree, db);
}

// ── Databases from before the name-resolution tables ─────────────────────────
// Such a database has files and links but no scopes, bindings or refs. Updating
// it would let the resolver drop nearly every link, so DbUpdater refuses and
// leaves the file as it was; a scan rebuilds it.

namespace {

// Everything an old database holds that an update could change, read over a
// plain connection: ResolvedDb::open goes through initDb, which migrates.
Rows snapshot(const std::string& path)
{
    Rows out;
    sqlite3* db = nullptr;
    EXPECT_EQ(sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr), SQLITE_OK);
    for (const char* sql : {
             "SELECT link_type || '|' || source_id || '|' || target_id FROM link ORDER BY 1;",
             "SELECT 'files ' || COUNT(*) FROM file;",
             "SELECT 'symbols ' || COUNT(*) FROM symbol;",
             "SELECT 'version ' || user_version FROM pragma_user_version;",
             "SELECT 'binding columns ' || COUNT(*) FROM pragma_table_info('binding');"}) {
        sqlite3_stmt* stmt = nullptr;
        EXPECT_EQ(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr), SQLITE_OK) << sql;
        while (sqlite3_step(stmt) == SQLITE_ROW)
            out.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
        sqlite3_finalize(stmt);
    }
    sqlite3_close(db);
    return out;
}

// Turn a scanned database into what an older Telescode left: no scan stamp
// (not even the table), the version-2 tables empty, the version-1 schema
// stamp, and a raw-name link of the kind the parser used to write.
void makeOld(sqlite3* db)
{
    ASSERT_EQ(sqlite3_exec(db,
        "DROP TABLE meta;"
        "DELETE FROM ref; DELETE FROM binding; DELETE FROM scope; DELETE FROM symbol;"
        "DELETE FROM import_resolution;"
        "ALTER TABLE binding DROP COLUMN is_exported;"
        "INSERT INTO link VALUES('resapp/util.py::helper', 'len', 'CALLS');"
        "PRAGMA user_version = 1;",
        nullptr, nullptr, nullptr), SQLITE_OK);
}

}  // namespace

TEST(ResolverOldDatabase, UpdatesAreRefusedAndChangeNothing)
{
    FixtureCopy tree("olddb");
    ResolvedDb db("olddb");
    db.scan(tree.root());
    makeOld(db.db());
    db.close();
    const Rows before = snapshot(db.path());
    ASSERT_NE(std::find(before.begin(), before.end(), "version 1"), before.end());

    PythonParser parser;
    const ParseResult util = parser.parseFile(tree.file("resapp/util.py"), tree.root());
    const char* path = db.path().c_str();
    EXPECT_EQ(DbUpdater::updateFile(path, util), Resolver::kNeedsRescan);
    EXPECT_EQ(DbUpdater::updateFiles(path, {util}), Resolver::kNeedsRescan);
    EXPECT_EQ(DbUpdater::deleteFile(path, "resapp/util.py"), Resolver::kNeedsRescan);
    EXPECT_EQ(DbUpdater::renameFile(path, "resapp/util.py", util), Resolver::kNeedsRescan);
    EXPECT_EQ(snapshot(db.path()), before);
}

TEST(ResolverOldDatabase, ScanOverItRebuildsEverything)
{
    FixtureCopy tree("olddb_scan");
    ResolvedDb db("olddb_scan");
    db.scan(tree.root());
    makeOld(db.db());
    EXPECT_TRUE(Resolver::needsRescan(db.db()));
    EXPECT_EQ(Resolver::run(db.db()), Resolver::kNeedsRescan);
    db.close();

    db.scan(tree.root());
    EXPECT_FALSE(Resolver::needsRescan(db.db()));
    EXPECT_EQ(db.links(), kFixtureLinks);
}

TEST(ResolverOldDatabase, StageTwoDatabaseWithoutStampIsRefused)
{
    // A Stage-2 scan has every name-resolution table filled, but not the refs
    // and export flags the resolver now relies on -- and no stamp.
    FixtureCopy tree("stage2db");
    ResolvedDb db("stage2db");
    db.scan(tree.root());
    ASSERT_EQ(sqlite3_exec(db.db(), "DELETE FROM meta;", nullptr, nullptr, nullptr), SQLITE_OK);
    EXPECT_TRUE(Resolver::needsRescan(db.db()));
    db.close();
    const Rows before = snapshot(db.path());

    PythonParser parser;
    EXPECT_EQ(DbUpdater::updateFile(db.path().c_str(),
                                    parser.parseFile(tree.file("resapp/util.py"), tree.root())),
              Resolver::kNeedsRescan);
    EXPECT_EQ(snapshot(db.path()), before);
}

TEST(ResolverOldDatabase, UnreadableFilesDoNotLookOld)
{
    // A repo whose only file cannot be read scans to a file row with no scope.
    // That is a current database, and updating it later works.
    FixtureCopy tree("unreadable", {{"a.py", "def f():\n    return 0\n\n\ndef g():\n    return f()\n"}});
    fs::permissions(tree.file("a.py"), fs::perms::none);
    if (std::ifstream(tree.file("a.py")).is_open()) {
        fs::permissions(tree.file("a.py"), fs::perms::owner_read | fs::perms::owner_write);
        GTEST_SKIP() << "running with permission to read any file";
    }
    ResolvedDb db("unreadable");
    db.scan(tree.root());   // asserts Resolver::run succeeds
    EXPECT_FALSE(Resolver::needsRescan(db.db()));
    db.close();

    fs::permissions(tree.file("a.py"), fs::perms::owner_read | fs::perms::owner_write);
    PythonParser parser;
    ASSERT_EQ(DbUpdater::updateFile(db.path().c_str(), parser.parseFile(tree.file("a.py"), tree.root())),
              SQLITE_OK);
    db.open();
    EXPECT_EQ(db.links(), (Rows{"CALLS|a.py::g|a.py::f"}));
}
