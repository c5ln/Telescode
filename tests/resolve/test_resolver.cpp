// tests/resolve/test_resolver.cpp
// What Resolver concludes from tests/fixtures/resolution: import_resolution,
// the resolved_id/status/confidence of each interesting ref, and the CALLS /
// INHERITS / DECORATES links written from them. The fixture is scanned for real
// (PythonParser::parseDirectory + DbInserter::insertAll, which resolves), the
// same path `scan` takes. Lines are 0-indexed.
//
// tests/parser/test_python_extraction.cpp pins down the rows the resolver
// reads; the comments here only say why each answer is the right one.

#include "db/DbInserter.h"
#include "db/DbUpdater.h"
#include "db/db.h"
#include "lang/python/PythonLanguage.h"
#include "parser/PythonParser.h"
#include "resolve/Resolver.h"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <algorithm>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
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
        close();
        ASSERT_EQ(DbInserter::insertAll(path_.c_str(), results, &stats_), SQLITE_OK);
        open();
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

// Resolver::resolveAll on an open database, in a transaction of its own.
int resolveAgain(sqlite3* db, const LanguageRegistry& languages = LanguageRegistry::builtin())
{
    int rc = sqlite3_exec(db, "BEGIN;", nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK) return rc;
    rc = Resolver::resolveAll(db, nullptr, languages);
    sqlite3_exec(db, rc == SQLITE_OK ? "COMMIT;" : "ROLLBACK;", nullptr, nullptr, nullptr);
    return rc;
}

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
    "CALLS|resapp/mro.py::Both::use|resapp/mro.py::Left::get_axis_num",
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
    "INHERITS|resapp/mro.py::Both|resapp/mro.py::Left",
    "INHERITS|resapp/mro.py::Both|resapp/mro.py::Right",
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
    // One row per distinct (file, module_spec) an import names. Relative specs
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

TEST_F(Resolution, LeftBaseWinsForAMemberBothDefine)
{
    // class Both(Left, Right), both bases define get_axis_num: the base
    // search goes left to right, so it is Left's (xarray's
    // Variable(NamedArray, AbstractArray) has this shape).
    EXPECT_EQ(db_->ref("resapp/mro.py", 15, "self.get_axis_num"),
              "resapp/mro.py::Left::get_axis_num|resolved|heuristic");
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

TEST_F(Resolution, InheritsFixpointSettles)
{
    // The fixture needs at least the first round, and settles well within the cap.
    EXPECT_GE(db_->stats().inheritsRounds, 1);
    EXPECT_FALSE(db_->stats().inheritsCapHit);
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

TEST_F(Resolution, ImportLinksAreResolvedFiles)
{
    // views.py imports through eight specs; `json` is external, and `..` and
    // `resapp`, `..models` and `resapp.models` each name one file: five links.
    EXPECT_EQ(db_->rows("SELECT target_id FROM link WHERE link_type = 'IMPORTS'"
                        "   AND source_id = 'resapp/sub/views.py' ORDER BY target_id;"),
              (Rows{"resapp/__init__.py", "resapp/models.py", "resapp/sub/__init__.py",
                    "resapp/sub/helpers.py", "resapp/util.py"}));
}

TEST_F(Resolution, LinkRowsAreDistinct)
{
    // One ref per link carries emits_link, so the view needs no DISTINCT.
    EXPECT_EQ(db_->count("SELECT COUNT(*) FROM link;"),
              db_->count("SELECT COUNT(*) FROM (SELECT DISTINCT * FROM link);"));
    // Some symbol in the fixture uses the same target on more than one line:
    // more refs than links.
    EXPECT_GT(db_->count("SELECT COUNT(*) FROM ref WHERE status = 'resolved'"
                         "   AND kind IN ('CALLS', 'INHERITS', 'DECORATES');"),
              db_->count("SELECT COUNT(*) FROM link WHERE link_type <> 'IMPORTS';"));
    EXPECT_EQ(db_->stats().links.at("CALLS") + db_->stats().links.at("INHERITS") +
              db_->stats().links.at("DECORATES"),
              static_cast<int>(kFixtureLinks.size()));
}

TEST_F(Resolution, EachLinkIsMarkedOnItsFirstRef)
{
    // Of the refs behind one link, the lowest ref_id carries the mark: the
    // order the old table's rows were inserted in, which Graph relies on.
    EXPECT_EQ(db_->count("SELECT COUNT(*) FROM ref r WHERE r.emits_link = 1 AND EXISTS ("
                         "  SELECT 1 FROM ref o WHERE o.ref_id < r.ref_id AND o.status = 'resolved'"
                         "     AND o.source_id = r.source_id AND o.resolved_id = r.resolved_id"
                         "     AND o.kind = r.kind);"), 0);
    // And the fixture has links with more than one ref, so the rule is tested.
    EXPECT_GT(db_->count("SELECT COUNT(*) FROM (SELECT 1 FROM ref WHERE status = 'resolved'"
                         "   AND kind IN ('CALLS', 'INHERITS', 'DECORATES')"
                         " GROUP BY source_id, resolved_id, kind HAVING COUNT(*) > 1 AND MAX(emits_link) = 1);"), 0);
}

TEST(ResolverRerun, ReplacesStaleLinksAndIsIdempotent)
{
    ResolvedDb db("rerun");
    db.scan();
    // Every ref marked as a link, as no resolver run would leave them.
    ASSERT_EQ(sqlite3_exec(db.db(), "UPDATE ref SET emits_link = 1;", nullptr, nullptr, nullptr),
              SQLITE_OK);
    ASSERT_NE(db.links(), kFixtureLinks);
    ASSERT_EQ(resolveAgain(db.db()), SQLITE_OK);
    EXPECT_EQ(db.links(), kFixtureLinks);
    ASSERT_EQ(resolveAgain(db.db()), SQLITE_OK);
    EXPECT_EQ(db.links(), kFixtureLinks);
}

// ── After DbUpdater ───────────────────────────────────────────────────────────
// Every update operation re-resolves the refs the change can affect, so links
// into and out of the changed file are right afterwards -- including links from
// files that were not re-parsed. test_incremental.cpp holds each update against
// a fresh scan.

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

TEST(ResolverAfterUpdate, UnreadableFileScansAndUpdatesLater)
{
    // A repo whose only file cannot be read scans to a file row with no scope;
    // updating it once the file can be read gives its links.
    FixtureCopy tree("unreadable", {{"a.py", "def f():\n    return 0\n\n\ndef g():\n    return f()\n"}});
    fs::permissions(tree.file("a.py"), fs::perms::none);
    if (std::ifstream(tree.file("a.py")).is_open()) {
        fs::permissions(tree.file("a.py"), fs::perms::owner_read | fs::perms::owner_write);
        GTEST_SKIP() << "running with permission to read any file";
    }
    ResolvedDb db("unreadable");
    db.scan(tree.root());   // asserts the scan succeeds
    db.close();

    fs::permissions(tree.file("a.py"), fs::perms::owner_read | fs::perms::owner_write);
    PythonParser parser;
    ASSERT_EQ(DbUpdater::updateFile(db.path().c_str(), parser.parseFile(tree.file("a.py"), tree.root())),
              SQLITE_OK);
    db.open();
    EXPECT_EQ(db.links(), (Rows{"CALLS|a.py::g|a.py::f"}));
}

TEST(ResolverLanguages, FilesWithoutAPluginGoBackToPending)
{
    // Resolved once with Python, then with no language at all: nothing the
    // first run decided may linger as a link.
    ResolvedDb db("noplugin");
    db.scan();
    ASSERT_FALSE(db.links().empty());
    const LanguageRegistry none;
    ASSERT_EQ(resolveAgain(db.db(), none), SQLITE_OK);
    EXPECT_EQ(db.count("SELECT COUNT(*) FROM ref WHERE status <> 'pending' OR resolved_id IS NOT NULL"
                       "    OR confidence IS NOT NULL OR resolver IS NOT NULL OR emits_link <> 0;"), 0);
    EXPECT_EQ(db.count("SELECT COUNT(*) FROM import_resolution"
                       " WHERE status <> 'pending' OR resolved_file_id IS NOT NULL;"), 0);
    EXPECT_EQ(db.rows("SELECT * FROM link;"), Rows{});
}

// ── Format version ────────────────────────────────────────────────────────────
// A database of another version (user_version) is never read or changed: scan
// replaces it, everything else refuses it. There is no migration.

namespace {

std::string fileBytes(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Every DbUpdater operation, on a database it must refuse with `refusal`.
void expectUpdatesRefused(const FixtureCopy& tree, const std::string& path,
                          int refusal = kWrongDbVersion)
{
    PythonParser parser;
    const ParseResult util = parser.parseFile(tree.file("resapp/util.py"), tree.root());
    const char* p = path.c_str();
    EXPECT_EQ(DbUpdater::updateFile(p, util), refusal);
    EXPECT_EQ(DbUpdater::updateFiles(p, {util}), refusal);
    EXPECT_EQ(DbUpdater::deleteFile(p, "resapp/util.py"), refusal);
    EXPECT_EQ(DbUpdater::deleteFiles(p, {"resapp/util.py"}), refusal);
    EXPECT_EQ(DbUpdater::renameFile(p, "resapp/util.py", util), refusal);
    std::vector<std::string> importers{"stale"};
    EXPECT_EQ(DbUpdater::findImportingFiles(p, "resapp/models.py", importers), refusal);
    EXPECT_TRUE(importers.empty());
}

}  // namespace

TEST(DbVersion, NewDatabaseHasTheCurrentSchema)
{
    ResolvedDb db("version_new");
    db.open();
    EXPECT_EQ(db.count("PRAGMA user_version;"), kDbVersion);
    EXPECT_EQ(db.rows("SELECT type FROM sqlite_master WHERE name = 'link';"), (Rows{"view"}));
    bool empty = true;
    EXPECT_EQ(checkDbVersion(db.path().c_str(), &empty), SQLITE_OK);
    EXPECT_FALSE(empty);
}

TEST(DbVersion, OtherVersionIsRefusedAndLeftAlone)
{
    FixtureCopy tree("version_other");
    ResolvedDb db("version_other");
    db.scan(tree.root());
    ASSERT_EQ(sqlite3_exec(db.db(), ("PRAGMA user_version = " + std::to_string(kDbVersion - 1) + ";").c_str(),
                           nullptr, nullptr, nullptr), SQLITE_OK);
    db.close();
    const std::string before = fileBytes(db.path());
    ASSERT_FALSE(before.empty());

    sqlite3* conn = nullptr;
    EXPECT_EQ(initDb(db.path().c_str(), &conn), kWrongDbVersion);
    EXPECT_EQ(conn, nullptr);
    expectUpdatesRefused(tree, db.path());
    EXPECT_EQ(fileBytes(db.path()), before);
}

TEST(DbVersion, FileThatIsNotADatabaseIsRefused)
{
    FixtureCopy tree("version_garbage");
    ResolvedDb db("version_garbage");
    std::ofstream(db.path(), std::ios::binary) << std::string(4096, 'x');
    const std::string before = fileBytes(db.path());

    sqlite3* conn = nullptr;
    EXPECT_EQ(initDb(db.path().c_str(), &conn), kNotADatabase);
    expectUpdatesRefused(tree, db.path(), kNotADatabase);
    EXPECT_EQ(fileBytes(db.path()), before);
}

TEST(DbVersion, ScanReplacesAnotherVersion)
{
    FixtureCopy tree("version_rescan");
    ResolvedDb db("version_rescan");
    db.scan(tree.root());
    ASSERT_EQ(sqlite3_exec(db.db(),
        ("CREATE TABLE leftover(x); PRAGMA user_version = " + std::to_string(kDbVersion + 1) + ";").c_str(),
        nullptr, nullptr, nullptr), SQLITE_OK);
    db.close();

    db.scan(tree.root());
    EXPECT_EQ(db.count("PRAGMA user_version;"), kDbVersion);
    EXPECT_EQ(db.count("SELECT COUNT(*) FROM sqlite_master WHERE name = 'leftover';"), 0);
    EXPECT_EQ(db.links(), kFixtureLinks);
}

TEST(DbVersion, ScanLeavesAFileThatIsNotADatabaseAlone)
{
    // Only an SQLite database is ever deleted: anything else at the path may
    // be a file the path was mistyped for.
    FixtureCopy tree("version_rescan_garbage");
    PythonParser parser;
    const auto results = parser.parseDirectory(tree.root(), tree.root());
    for (const std::string& text : {std::string(4096, 'x'),
                                    std::string("import os\n\n\ndef main():\n    return os.getcwd()\n"),
                                    std::string("SQLite")}) {
        ResolvedDb db("version_rescan_garbage");
        std::ofstream(db.path(), std::ios::binary) << text;
        EXPECT_EQ(DbInserter::insertAll(db.path().c_str(), results), kNotADatabase) << text;
        EXPECT_EQ(fileBytes(db.path()), text);
        EXPECT_FALSE(fs::exists(db.path() + "-wal"));
    }
}

namespace {

// An SQLite file at path with the given SQL run in it, closed cleanly.
void makeSqliteFile(const std::string& path, const std::string& sql)
{
    sqlite3* raw = nullptr;
    ASSERT_EQ(sqlite3_open(path.c_str(), &raw), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(raw, sql.c_str(), nullptr, nullptr, nullptr), SQLITE_OK);
    sqlite3_close(raw);
}

}  // namespace

TEST(DbVersion, ScanLeavesSomeoneElsesSqliteFileAlone)
{
    // SQLite, but not Telescode's: no `file` and `link`. Whatever its version.
    FixtureCopy tree("version_foreign");
    PythonParser parser;
    const auto results = parser.parseDirectory(tree.root(), tree.root());
    for (int version : {0, kDbVersion + 1}) {
        ResolvedDb db("version_foreign");
        makeSqliteFile(db.path(),
            "CREATE TABLE important(x); INSERT INTO important VALUES('precious');"
            "CREATE TABLE file(y); PRAGMA user_version = " + std::to_string(version) + ";");
        const std::string before = fileBytes(db.path());
        EXPECT_EQ(DbInserter::insertAll(db.path().c_str(), results), kNotADatabase) << version;
        EXPECT_EQ(fileBytes(db.path()), before) << version;
        EXPECT_FALSE(fs::exists(db.path() + "-wal")) << version;
    }
}

TEST(DbVersion, ScanReplacesATelescodeDatabaseFromBeforeTheVersion)
{
    // What 5e1638d wrote: file and link tables, user_version 0.
    FixtureCopy tree("version_unversioned");
    ResolvedDb db("version_unversioned");
    makeSqliteFile(db.path(),
        "CREATE TABLE file(file_id TEXT PRIMARY KEY, loc INTEGER);"
        "CREATE TABLE link(source_id TEXT, target_id TEXT, link_type TEXT);"
        "INSERT INTO link VALUES('a.py', 'b', 'IMPORTS');");
    expectUpdatesRefused(tree, db.path());

    db.scan(tree.root());
    EXPECT_EQ(db.count("PRAGMA user_version;"), kDbVersion);
    EXPECT_EQ(db.links(), kFixtureLinks);
}

TEST(DbVersion, ScanFillsAnEmptyFile)
{
    FixtureCopy tree("version_empty_file");
    ResolvedDb db("version_empty_file");
    std::ofstream(db.path(), std::ios::binary).close();
    ASSERT_TRUE(fs::exists(db.path()));

    db.scan(tree.root());
    EXPECT_EQ(db.count("PRAGMA user_version;"), kDbVersion);
    EXPECT_EQ(db.links(), kFixtureLinks);
}

TEST(DbVersion, ScanOfACurrentDatabaseKeepsWhatIsNotParserOutput)
{
    // Same version: rescanned in place, so the stored algo config stays.
    FixtureCopy tree("version_keep");
    ResolvedDb db("version_keep");
    db.scan(tree.root());
    ASSERT_EQ(sqlite3_exec(db.db(),
        "INSERT INTO reading_sequence_config(config_key, config_value) VALUES('alpha', '0.25');",
        nullptr, nullptr, nullptr), SQLITE_OK);
    db.scan(tree.root());
    EXPECT_EQ(db.rows("SELECT config_value FROM reading_sequence_config WHERE config_key = 'alpha';"),
              (Rows{"0.25"}));
    EXPECT_EQ(db.links(), kFixtureLinks);
}

// ── A scan that does not finish ───────────────────────────────────────────────
// Parse rows, resolution and (for a new file) the schema and its version are
// one transaction: a scan that fails or is killed part way leaves the
// database it started from, or an empty file -- never files with no links.

namespace {

// Python, except that the process dies the moment the resolver asks it
// anything about modules -- after every parse row is in, before any answer is.
class DyingPython : public PythonLanguage {
public:
    std::string resolveModule(const std::string&, const std::string&,
                              const ProjectIndex&) const override {
        std::raise(SIGKILL);
        return "";
    }
};

const LanguageRegistry& dyingRegistry()
{
    static const LanguageRegistry reg = [] {
        LanguageRegistry r;
        r.add(std::make_unique<DyingPython>());
        return r;
    }();
    return reg;
}

}  // namespace

TEST(ScanAtomic, FailedResolveLeavesThePreviousDatabase)
{
    FixtureCopy tree("atomic_fail");
    ResolvedDb db("atomic_fail");
    db.scan(tree.root());
    const Rows files = db.rows("SELECT file_id FROM file ORDER BY 1;");
    // Any write of a ref's answer now fails, as a full disk would: a scan
    // inserts each ref with its answer (Resolver::resolveScan).
    ASSERT_EQ(sqlite3_exec(db.db(),
        "CREATE TRIGGER no_answers BEFORE INSERT ON ref BEGIN SELECT RAISE(ABORT, 'no'); END;"
        "CREATE TRIGGER no_updates BEFORE UPDATE ON ref BEGIN SELECT RAISE(ABORT, 'no'); END;",
        nullptr, nullptr, nullptr), SQLITE_OK);
    db.close();

    fs::remove(tree.file("resapp/util.py"));
    PythonParser parser;
    EXPECT_NE(DbInserter::insertAll(db.path().c_str(), parser.parseDirectory(tree.root(), tree.root())),
              SQLITE_OK);

    db.open();
    EXPECT_EQ(db.rows("SELECT file_id FROM file ORDER BY 1;"), files);
    EXPECT_EQ(db.links(), kFixtureLinks);
    EXPECT_EQ(db.count("SELECT COUNT(*) FROM ref WHERE status = 'pending';"), 0);
}

#if GTEST_HAS_DEATH_TEST && !defined(_WIN32)

TEST(ScanAtomic, KilledFirstScanLeavesAnEmptyFile)
{
    FixtureCopy tree("atomic_kill_new");
    ResolvedDb db("atomic_kill_new");
    PythonParser parser;
    const auto results = parser.parseDirectory(tree.root(), tree.root());
    EXPECT_EXIT(DbInserter::insertAll(db.path().c_str(), results, nullptr, dyingRegistry()),
                ::testing::KilledBySignal(SIGKILL), "");

    // No version, no tables: nothing for `algo` to take for a scanned project.
    bool empty = false;
    EXPECT_EQ(checkDbVersion(db.path().c_str(), &empty), SQLITE_OK);
    EXPECT_TRUE(empty);
}

TEST(ScanAtomic, KilledRescanLeavesThePreviousDatabase)
{
    FixtureCopy tree("atomic_kill_rescan");
    ResolvedDb db("atomic_kill_rescan");
    db.scan(tree.root());
    const Rows files = db.rows("SELECT file_id FROM file ORDER BY 1;");
    db.close();

    fs::remove(tree.file("resapp/util.py"));
    PythonParser parser;
    const auto results = parser.parseDirectory(tree.root(), tree.root());
    EXPECT_EXIT(DbInserter::insertAll(db.path().c_str(), results, nullptr, dyingRegistry()),
                ::testing::KilledBySignal(SIGKILL), "");

    db.open();
    EXPECT_EQ(db.count("PRAGMA user_version;"), kDbVersion);
    EXPECT_EQ(db.rows("SELECT file_id FROM file ORDER BY 1;"), files);
    EXPECT_EQ(db.links(), kFixtureLinks);
    EXPECT_EQ(db.count("SELECT COUNT(*) FROM ref WHERE status = 'pending';"), 0);
}

#endif

// ── update dangling ───────────────────────────────────────────────────────────

TEST(ResolverDangling, ImportersByEveryKindOfSpec)
{
    FixtureCopy tree("dangling", {
        {"pkg/__init__.py", "from . import mod\n"},
        {"pkg/mod.py", "def f():\n    return 0\n"},
        {"pkg/rel.py", "from .mod import f\n"},
        {"pkg/up/__init__.py", ""},
        {"pkg/up/deep.py", "from ..mod import f\n"},
        {"absolute.py", "import pkg.mod\n"},
        {"aliased.py", "import pkg.mod as m\n"},
        {"fromsym.py", "from pkg.mod import f\n"},
        {"frompkg.py", "from pkg import mod\n\n\ndef p():\n    return mod.f()\n"},
        {"twice.py", "import pkg.mod\nfrom pkg.mod import f\n"},
        {"other.py", "import os\n"},
    });
    ResolvedDb db("dangling");
    db.scan(tree.root());
    db.close();

    std::vector<std::string> importers;
    ASSERT_EQ(DbUpdater::findImportingFiles(db.path().c_str(), "pkg/mod.py", importers), SQLITE_OK);
    // `from pkg import mod` names the package, whose __init__ is what
    // import_resolution has; that the name is the submodule the resolver
    // finds through the binding. So does the package's own `from . import mod`.
    EXPECT_EQ(importers, (std::vector<std::string>{
        "absolute.py", "aliased.py", "frompkg.py", "fromsym.py", "pkg/__init__.py", "pkg/rel.py",
        "pkg/up/deep.py", "twice.py"}));

    // A spec names one file: `import pkg.mod` is pkg/mod.py's, not the
    // package's. `from . import mod` in the package itself imports
    // pkg/__init__.py from pkg/__init__.py, which does not dangle when it goes.
    ASSERT_EQ(DbUpdater::findImportingFiles(db.path().c_str(), "pkg/__init__.py", importers), SQLITE_OK);
    EXPECT_EQ(importers, (std::vector<std::string>{"frompkg.py"}));

    ASSERT_EQ(DbUpdater::findImportingFiles(db.path().c_str(), "other.py", importers), SQLITE_OK);
    EXPECT_TRUE(importers.empty());
}

TEST(ResolverDangling, ImportsOfGlobalNamesCount)
{
    // An import under `global` binds the module's name: its spec is resolved
    // like any other, so the file imports pkg/mod.py and dangles without it.
    FixtureCopy tree("dangling_global", {
        {"pkg/__init__.py", ""},
        {"pkg/mod.py", "def x(): pass\n"},
        {"pkg/user.py", "y = None\ndef hook():\n    global y\n    from .mod import x as y\n"},
        {"pkg/user2.py", "def hook():\n    global mod\n    import pkg.mod as mod\n"},
    });
    ResolvedDb db("dangling_global");
    db.scan(tree.root());
    EXPECT_EQ(db.rows("SELECT source_id || ' -> ' || target_id FROM link"
                      " WHERE link_type = 'IMPORTS' ORDER BY 1;"),
              (Rows{"pkg/user.py -> pkg/mod.py", "pkg/user2.py -> pkg/mod.py"}));
    db.close();

    std::vector<std::string> importers;
    ASSERT_EQ(DbUpdater::findImportingFiles(db.path().c_str(), "pkg/mod.py", importers), SQLITE_OK);
    EXPECT_EQ(importers, (std::vector<std::string>{"pkg/user.py", "pkg/user2.py"}));
}

TEST(ResolverDangling, EveryImportOnALineCounts)
{
    // Both statements bind `pkg` once on their line, but import two modules
    // each: every spec gets its import_resolution row, so its IMPORTS link.
    FixtureCopy tree("dangling_sameline", {
        {"pkg/__init__.py", ""},
        {"pkg/h.py", ""},
        {"pkg/i.py", ""},
        {"pkg/j.py", ""},
        {"pkg/k.py", ""},
        {"main.py", "import pkg.h, pkg.i\n"},
        {"main2.py", "import pkg.j; import pkg.k\n"},
    });
    ResolvedDb db("dangling_sameline");
    db.scan(tree.root());
    EXPECT_EQ(db.rows("SELECT source_id || ' -> ' || target_id FROM link"
                      " WHERE link_type = 'IMPORTS' ORDER BY 1;"),
              (Rows{"main.py -> pkg/h.py", "main.py -> pkg/i.py",
                    "main2.py -> pkg/j.py", "main2.py -> pkg/k.py"}));
    db.close();

    std::vector<std::string> importers;
    for (const auto& [file, importer] : std::vector<std::pair<std::string, std::string>>{
             {"pkg/h.py", "main.py"}, {"pkg/i.py", "main.py"},
             {"pkg/j.py", "main2.py"}, {"pkg/k.py", "main2.py"}}) {
        ASSERT_EQ(DbUpdater::findImportingFiles(db.path().c_str(), file, importers), SQLITE_OK);
        EXPECT_EQ(importers, (std::vector<std::string>{importer})) << file;
    }
}
