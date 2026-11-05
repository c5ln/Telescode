// tests/parser/test_python_extraction.cpp
// What PythonParser extracts for name resolution: symbol, scope, binding and
// ref rows, checked row for row on tests/fixtures/resolution after a real scan
// (PythonParser::parseDirectory + DbInserter::insertAll), so the insert path
// is covered too. Lines are 0-indexed, as everywhere else in the database.
//
// The fixture is the input the resolver is tested on as well; these tests pin
// down what the resolver gets to see, not what it should conclude.

#include "db/DbInserter.h"
#include "db/DbUpdater.h"
#include "db/db.h"
#include "lang/LanguageRegistry.h"
#include "parser/PythonParser.h"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

#ifndef TS_RESOLUTION_FIXTURE
#error "TS_RESOLUTION_FIXTURE must point at tests/fixtures/resolution"
#endif

namespace {

using Rows = std::vector<std::string>;

// A scanned copy of the fixture in a database that deletes itself.
class ScannedDb {
public:
    explicit ScannedDb(const std::string& tag) {
        path_ = (fs::temp_directory_path() / ("ts_parser_test_" + tag + ".db")).string();
        remove();
    }
    ~ScannedDb() {
        if (db_) sqlite3_close(db_);
        remove();
    }

    // Scan the fixture and open the result for queries.
    void scan() {
        PythonParser parser;
        const auto results = parser.parseDirectory(TS_RESOLUTION_FIXTURE, TS_RESOLUTION_FIXTURE);
        // No language plugin: the rows exactly as the parser wrote them.
        ASSERT_EQ(DbInserter::insertAll(path_.c_str(), results, nullptr, LanguageRegistry{}),
                  SQLITE_OK);
        open();
    }

    void open() {
        if (db_) sqlite3_close(db_);
        ASSERT_EQ(sqlite3_open(path_.c_str(), &db_), SQLITE_OK);
    }

    // Each row as its columns joined by '|', NULL spelled out.
    Rows rows(const std::string& sql, const std::string& param = "") const {
        Rows out;
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
            ADD_FAILURE() << "bad query: " << sql << ": " << sqlite3_errmsg(db_);
            return out;
        }
        if (!param.empty()) sqlite3_bind_text(stmt, 1, param.c_str(), -1, SQLITE_TRANSIENT);
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

    int count(const std::string& sql, const std::string& param = "") const {
        const Rows r = rows(sql, param);
        return r.empty() ? -1 : std::stoi(r[0]);
    }

    const std::string& path() const { return path_; }

private:
    void remove() {
        for (const char* suffix : {"", "-wal", "-shm"}) {
            std::error_code ec;
            fs::remove(path_ + suffix, ec);
        }
    }

    std::string path_;
    sqlite3* db_ = nullptr;
};

const char* kSymbols =
    "SELECT symbol_id, parent_id, kind, name, is_exported, start_line, end_line"
    "  FROM symbol WHERE file_id = ? ORDER BY symbol_id;";
const char* kScopes =
    "SELECT scope_id, parent_scope_id, kind, start_line, end_line"
    "  FROM scope WHERE file_id = ? ORDER BY scope_id;";
const char* kBindings =
    "SELECT scope_id, local_name, kind, target_id, module_spec, imported_name, line"
    "  FROM binding WHERE file_id = ? ORDER BY scope_id, local_name, line;";
const char* kRefs =
    "SELECT kind, source_id, scope_id, name_path, raw_text, line"
    "  FROM ref WHERE file_id = ? ORDER BY line, kind, name_path;";

class Extraction : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        db_ = new ScannedDb("extraction");
        db_->scan();
    }
    static void TearDownTestSuite() {
        delete db_;
        db_ = nullptr;
    }
    static ScannedDb* db_;
};

ScannedDb* Extraction::db_ = nullptr;

}  // namespace

// ── file ──────────────────────────────────────────────────────────────────────

TEST_F(Extraction, FileLanguageAndModuleName)
{
    EXPECT_EQ(db_->rows("SELECT file_id, language, module_name FROM file ORDER BY file_id;"),
              (Rows{
                  "resapp/__init__.py|python|resapp",
                  "resapp/allmod.py|python|resapp.allmod",
                  "resapp/allstar.py|python|resapp.allstar",
                  "resapp/cases.py|python|resapp.cases",
                  "resapp/compat.py|python|resapp.compat",
                  "resapp/ctors.py|python|resapp.ctors",
                  "resapp/ctoruse.py|python|resapp.ctoruse",
                  "resapp/cycles.py|python|resapp.cycles",
                  "resapp/fallback.py|python|resapp.fallback",
                  "resapp/missing.py|python|resapp.missing",
                  "resapp/models.py|python|resapp.models",
                  "resapp/mro.py|python|resapp.mro",
                  "resapp/pkg2/__init__.py|python|resapp.pkg2",
                  "resapp/pkg2/mod.py|python|resapp.pkg2.mod",
                  "resapp/star.py|python|resapp.star",
                  "resapp/sub/__init__.py|python|resapp.sub",
                  "resapp/sub/deco.py|python|resapp.sub.deco",
                  "resapp/sub/helpers.py|python|resapp.sub.helpers",
                  "resapp/sub/views.py|python|resapp.sub.views",
                  "resapp/under.py|python|resapp.under",
                  "resapp/underexplicit.py|python|resapp.underexplicit",
                  "resapp/understar.py|python|resapp.understar",
                  "resapp/util.py|python|resapp.util",
              }));
}

TEST_F(Extraction, SchemaVersion)
{
    EXPECT_EQ(db_->count("PRAGMA user_version;"), kDbVersion);
}

// ── resapp/__init__.py: re-exports ────────────────────────────────────────────

TEST_F(Extraction, PackageInitReExports)
{
    const std::string f = "resapp/__init__.py";
    // `__all__` lists PublicB and helper, so `__all__` itself is not exported.
    EXPECT_EQ(db_->rows(kSymbols, f), (Rows{
        "resapp/__init__.py::__all__|NULL|variable|__all__|0|4|4",
    }));
    EXPECT_EQ(db_->rows(kScopes, f), (Rows{
        "resapp/__init__.py|NULL|file|0|4",
    }));
    // `from .models import B as PublicB`: bound under the alias, the original
    // name kept in imported_name, the relative spec kept verbatim.
    EXPECT_EQ(db_->rows(kBindings, f), (Rows{
        "resapp/__init__.py|PublicB|import_symbol|NULL|.models|B|1",
        "resapp/__init__.py|__all__|def|resapp/__init__.py::__all__|NULL|NULL|4",
        "resapp/__init__.py|helper|import_symbol|NULL|.util|helper|2",
    }));
    EXPECT_EQ(db_->rows(kRefs, f), Rows{});
}

// ── resapp/models.py: class B with a method named `list` ──────────────────────

TEST_F(Extraction, ModelsSymbolsAndScopes)
{
    const std::string f = "resapp/models.py";
    EXPECT_EQ(db_->rows(kSymbols, f), (Rows{
        "resapp/models.py::B|NULL|class|B|1|4|9",
        "resapp/models.py::B::list|resapp/models.py::B|method|list|1|5|6",
        "resapp/models.py::B::run|resapp/models.py::B|method|run|1|8|9",
        "resapp/models.py::E|NULL|class|E|1|12|13",
        "resapp/models.py::first|NULL|function|first|1|20|21",
        "resapp/models.py::make|NULL|function|make|1|16|17",
    }));
    EXPECT_EQ(db_->rows(kScopes, f), (Rows{
        "resapp/models.py|NULL|file|0|21",
        "resapp/models.py::B|resapp/models.py|class|4|9",
        "resapp/models.py::B::list|resapp/models.py::B|function|5|6",
        "resapp/models.py::B::run|resapp/models.py::B|function|8|9",
        "resapp/models.py::E|resapp/models.py|class|12|13",
        "resapp/models.py::first|resapp/models.py|function|20|21",
        "resapp/models.py::make|resapp/models.py|function|16|17",
    }));
}

TEST_F(Extraction, ModelsBindings)
{
    // The method `list` binds in the class scope only; the module scope has no
    // `list`, so a bare `list(...)` elsewhere in the file is still the builtin.
    // `import os.path` binds the top-level package `os`.
    EXPECT_EQ(db_->rows(kBindings, "resapp/models.py"), (Rows{
        "resapp/models.py|B|def|resapp/models.py::B|NULL|NULL|4",
        "resapp/models.py|E|def|resapp/models.py::E|NULL|NULL|12",
        "resapp/models.py|OD|import_symbol|NULL|collections|OrderedDict|1",
        "resapp/models.py|first|def|resapp/models.py::first|NULL|NULL|20",
        "resapp/models.py|make|def|resapp/models.py::make|NULL|NULL|16",
        "resapp/models.py|os|import_module|NULL|os.path|NULL|0",
        "resapp/models.py::B|list|def|resapp/models.py::B::list|NULL|NULL|5",
        "resapp/models.py::B|run|def|resapp/models.py::B::run|NULL|NULL|8",
        "resapp/models.py::B::list|self|def|NULL|NULL|NULL|5",
        "resapp/models.py::B::run|self|def|NULL|NULL|NULL|8",
    }));
}

TEST_F(Extraction, ModelsRefs)
{
    // `class E(Exception)` -- an external base, looked up from the module scope.
    // `make()[0].run()` in first() has no identifier chain and yields no ref.
    EXPECT_EQ(db_->rows(kRefs, "resapp/models.py"), (Rows{
        "CALLS|resapp/models.py::B::run|resapp/models.py::B::run|self.list|self.list|9",
        "INHERITS|resapp/models.py::E|resapp/models.py|Exception|Exception|12",
        "CALLS|resapp/models.py::make|resapp/models.py::make|B|B|17",
    }));
}

// ── resapp/util.py: builtin shadowing, global, __all__ ────────────────────────

TEST_F(Extraction, UtilSymbols)
{
    // Module-level `list = [...]` is a variable symbol that shadows the builtin
    // for the whole module. `_private` is not in the literal __all__.
    EXPECT_EQ(db_->rows(kSymbols, "resapp/util.py"), (Rows{
        "resapp/util.py::__all__|NULL|variable|__all__|0|23|23",
        "resapp/util.py::_private|NULL|function|_private|0|19|20",
        "resapp/util.py::bump|NULL|function|bump|1|8|11",
        "resapp/util.py::counter|NULL|variable|counter|1|1|1",
        "resapp/util.py::helper|NULL|function|helper|1|4|5",
        "resapp/util.py::list|NULL|variable|list|1|0|0",
        "resapp/util.py::local_len|NULL|function|local_len|1|14|16",
    }));
    EXPECT_EQ(db_->rows(kScopes, "resapp/util.py"), (Rows{
        "resapp/util.py|NULL|file|0|23",
        "resapp/util.py::_private|resapp/util.py|function|19|20",
        "resapp/util.py::bump|resapp/util.py|function|8|11",
        "resapp/util.py::helper|resapp/util.py|function|4|5",
        "resapp/util.py::local_len|resapp/util.py|function|14|16",
    }));
}

TEST_F(Extraction, UtilBindings)
{
    // bump() declares `global counter`, so its assignment binds nothing in
    // bump's scope -- counter there is the module's. local_len() rebinds `len`
    // locally (target NULL: a local has no symbol row), shadowing the builtin
    // in that function only; the lambda's parameter `s` binds nothing.
    EXPECT_EQ(db_->rows(kBindings, "resapp/util.py"), (Rows{
        "resapp/util.py|__all__|def|resapp/util.py::__all__|NULL|NULL|23",
        "resapp/util.py|_private|def|resapp/util.py::_private|NULL|NULL|19",
        "resapp/util.py|bump|def|resapp/util.py::bump|NULL|NULL|8",
        "resapp/util.py|counter|def|resapp/util.py::counter|NULL|NULL|1",
        "resapp/util.py|helper|def|resapp/util.py::helper|NULL|NULL|4",
        "resapp/util.py|list|def|resapp/util.py::list|NULL|NULL|0",
        "resapp/util.py|local_len|def|resapp/util.py::local_len|NULL|NULL|14",
        "resapp/util.py::helper|x|def|NULL|NULL|NULL|4",
        "resapp/util.py::local_len|items|def|NULL|NULL|NULL|14",
        "resapp/util.py::local_len|len|def|NULL|NULL|NULL|15",
    }));
    EXPECT_EQ(db_->count("SELECT COUNT(*) FROM binding WHERE scope_id = ?;",
                         "resapp/util.py::bump"), 0);
}

TEST_F(Extraction, UtilRefs)
{
    // Both are `len`; only the scope tells the builtin (helper) from the local
    // lambda (local_len).
    EXPECT_EQ(db_->rows(kRefs, "resapp/util.py"), (Rows{
        "CALLS|resapp/util.py::helper|resapp/util.py::helper|len|len|5",
        "CALLS|resapp/util.py::local_len|resapp/util.py::local_len|len|len|16",
    }));
}

// ── resapp/sub/helpers.py: a second, unrelated class B ────────────────────────

TEST_F(Extraction, HelpersSymbolsAndScopes)
{
    const std::string f = "resapp/sub/helpers.py";
    EXPECT_EQ(db_->rows(kSymbols, f), (Rows{
        "resapp/sub/helpers.py::B|NULL|class|B|1|3|11",
        "resapp/sub/helpers.py::B::build|resapp/sub/helpers.py::B|method|build|1|10|11",
        "resapp/sub/helpers.py::B::list|resapp/sub/helpers.py::B|method|list|1|6|7",
        "resapp/sub/helpers.py::cached|NULL|function|cached|1|15|16",
    }));
    EXPECT_EQ(db_->rows(kScopes, f), (Rows{
        "resapp/sub/helpers.py|NULL|file|0|16",
        "resapp/sub/helpers.py::B|resapp/sub/helpers.py|class|3|11",
        "resapp/sub/helpers.py::B::build|resapp/sub/helpers.py::B|function|10|11",
        "resapp/sub/helpers.py::B::list|resapp/sub/helpers.py::B|function|6|7",
        "resapp/sub/helpers.py::cached|resapp/sub/helpers.py|function|15|16",
    }));
    EXPECT_EQ(db_->rows(kBindings, f), (Rows{
        "resapp/sub/helpers.py|B|def|resapp/sub/helpers.py::B|NULL|NULL|3",
        "resapp/sub/helpers.py|cached|def|resapp/sub/helpers.py::cached|NULL|NULL|15",
        "resapp/sub/helpers.py|functools|import_module|NULL|functools|NULL|0",
        "resapp/sub/helpers.py::B|build|def|resapp/sub/helpers.py::B::build|NULL|NULL|10",
        "resapp/sub/helpers.py::B|list|def|resapp/sub/helpers.py::B::list|NULL|NULL|6",
        "resapp/sub/helpers.py::B::list|self|def|NULL|NULL|NULL|6",
    }));
}

TEST_F(Extraction, HelpersRefs)
{
    // `list(range(3))` inside the method B.list: scope is the method, whose
    // parent class scope binds `list` -- the resolver must skip that class.
    // Decorators are looked up in the scope around the decorated definition:
    // the class body for @staticmethod, the module for @functools.lru_cache.
    // `B().list()` in cached() is a call on a call result: no ref.
    EXPECT_EQ(db_->rows(kRefs, "resapp/sub/helpers.py"), (Rows{
        "CALLS|resapp/sub/helpers.py::B::list|resapp/sub/helpers.py::B::list|list|list|7",
        "CALLS|resapp/sub/helpers.py::B::list|resapp/sub/helpers.py::B::list|range|range|7",
        "DECORATES|resapp/sub/helpers.py::B::build|resapp/sub/helpers.py::B|staticmethod|staticmethod|9",
        "CALLS|resapp/sub/helpers.py::B::build|resapp/sub/helpers.py::B::build|B|B|11",
        "DECORATES|resapp/sub/helpers.py::cached|resapp/sub/helpers.py|functools.lru_cache|functools.lru_cache|14",
    }));
}

// ── resapp/sub/views.py: every import form ────────────────────────────────────

TEST_F(Extraction, ViewsSymbolsAndScopes)
{
    const std::string f = "resapp/sub/views.py";
    EXPECT_EQ(db_->rows(kSymbols, f), (Rows{
        "resapp/sub/views.py::View|NULL|class|View|1|21|27",
        "resapp/sub/views.py::View::get|resapp/sub/views.py::View|method|get|1|22|24",
        "resapp/sub/views.py::View::render|resapp/sub/views.py::View|method|render|1|26|27",
        "resapp/sub/views.py::index|NULL|function|index|1|16|18",
        "resapp/sub/views.py::make_public|NULL|function|make_public|1|30|31",
        "resapp/sub/views.py::route|NULL|function|route|1|9|12",
        "resapp/sub/views.py::route::wrap|resapp/sub/views.py::route|function|wrap|1|10|11",
    }));
    EXPECT_EQ(db_->rows(kScopes, f), (Rows{
        "resapp/sub/views.py|NULL|file|0|31",
        "resapp/sub/views.py::View|resapp/sub/views.py|class|21|27",
        "resapp/sub/views.py::View::get|resapp/sub/views.py::View|function|22|24",
        "resapp/sub/views.py::View::render|resapp/sub/views.py::View|function|26|27",
        "resapp/sub/views.py::index|resapp/sub/views.py|function|16|18",
        "resapp/sub/views.py::make_public|resapp/sub/views.py|function|30|31",
        "resapp/sub/views.py::route|resapp/sub/views.py|function|9|12",
        "resapp/sub/views.py::route::wrap|resapp/sub/views.py::route|function|10|11",
    }));
}

TEST_F(Extraction, ViewsBindings)
{
    //   from .. import util            -> util,    import_symbol("..", "util")
    //   from ..models import B as C    -> C,       import_symbol("..models", "B")
    //   from . import helpers          -> helpers, import_symbol(".", "helpers")
    //   from ..util import *           -> "*3",    import_all("..util")
    //   import resapp.sub.helpers      -> resapp,  import_module("resapp.sub.helpers")
    //   import resapp.models as m      -> m,       import_module("resapp.models")
    //   from resapp import PublicB     -> PublicB, import_symbol("resapp", "PublicB")
    // and `from json import dumps` inside index() binds in index's scope.
    EXPECT_EQ(db_->rows(kBindings, "resapp/sub/views.py"), (Rows{
        "resapp/sub/views.py|*3|import_all|NULL|..util|NULL|3",
        "resapp/sub/views.py|C|import_symbol|NULL|..models|B|1",
        "resapp/sub/views.py|PublicB|import_symbol|NULL|resapp|PublicB|6",
        "resapp/sub/views.py|View|def|resapp/sub/views.py::View|NULL|NULL|21",
        "resapp/sub/views.py|helpers|import_symbol|NULL|.|helpers|2",
        "resapp/sub/views.py|index|def|resapp/sub/views.py::index|NULL|NULL|16",
        "resapp/sub/views.py|m|import_module|NULL|resapp.models|NULL|5",
        "resapp/sub/views.py|make_public|def|resapp/sub/views.py::make_public|NULL|NULL|30",
        "resapp/sub/views.py|resapp|import_module|NULL|resapp.sub.helpers|NULL|4",
        "resapp/sub/views.py|route|def|resapp/sub/views.py::route|NULL|NULL|9",
        "resapp/sub/views.py|util|import_symbol|NULL|..|util|0",
        "resapp/sub/views.py::View|get|def|resapp/sub/views.py::View::get|NULL|NULL|22",
        "resapp/sub/views.py::View|render|def|resapp/sub/views.py::View::render|NULL|NULL|26",
        "resapp/sub/views.py::View::get|self|def|NULL|NULL|NULL|22",
        "resapp/sub/views.py::View::render|self|def|NULL|NULL|NULL|26",
        "resapp/sub/views.py::index|dumps|import_symbol|NULL|json|dumps|17",
        "resapp/sub/views.py::route|path|def|NULL|NULL|NULL|9",
        "resapp/sub/views.py::route|wrap|def|resapp/sub/views.py::route::wrap|NULL|NULL|10",
        "resapp/sub/views.py::route::wrap|fn|def|NULL|NULL|NULL|10",
    }));
}

TEST_F(Extraction, ViewsRefs)
{
    // Bases resolve from the module scope; `metaclass=type` is not a base.
    // `super().get()` yields no ref. DECORATES refs start at the decorated
    // definition and name the decorator (the link runs the other way).
    EXPECT_EQ(db_->rows(kRefs, "resapp/sub/views.py"), (Rows{
        "DECORATES|resapp/sub/views.py::index|resapp/sub/views.py|route|route|15",
        "CALLS|resapp/sub/views.py::index|resapp/sub/views.py::index|dumps|dumps|18",
        "CALLS|resapp/sub/views.py::index|resapp/sub/views.py::index|helper|helper|18",
        "INHERITS|resapp/sub/views.py::View|resapp/sub/views.py|C|C|21",
        "INHERITS|resapp/sub/views.py::View|resapp/sub/views.py|m.E|m.E|21",
        "CALLS|resapp/sub/views.py::View::get|resapp/sub/views.py::View::get|util.helper|util.helper|24",
        "CALLS|resapp/sub/views.py::View::render|resapp/sub/views.py::View::render|resapp.sub.helpers.cached|resapp.sub.helpers.cached|27",
        "CALLS|resapp/sub/views.py::make_public|resapp/sub/views.py::make_public|PublicB|PublicB|31",
    }));
}

// ── resapp/compat.py: a name bound more than once in one scope ───────────────

TEST_F(Extraction, CompatKeepsEveryBinding)
{
    const std::string f = "resapp/compat.py";
    EXPECT_EQ(db_->rows(kSymbols, f), (Rows{
        "resapp/compat.py::a|NULL|variable|a|1|9|9",
        "resapp/compat.py::helper|NULL|variable|helper|1|4|4",
        "resapp/compat.py::use|NULL|function|use|1|12|13",
    }));
    EXPECT_EQ(db_->rows(kScopes, f), (Rows{
        "resapp/compat.py|NULL|file|0|13",
        "resapp/compat.py::use|resapp/compat.py|function|12|13",
    }));
    // No winner is picked:
    //   try: from .util import helper / except ImportError: helper = None
    //     -> both rows; the resolver prefers the one reaching a project symbol.
    //   import xml.dom / import xml.sax -> `xml` twice, one per module_spec.
    //   a = a = 1 -> the same name twice on one line is one row.
    EXPECT_EQ(db_->rows(kBindings, f), (Rows{
        "resapp/compat.py|a|def|resapp/compat.py::a|NULL|NULL|9",
        "resapp/compat.py|helper|import_symbol|NULL|.util|helper|2",
        "resapp/compat.py|helper|def|resapp/compat.py::helper|NULL|NULL|4",
        "resapp/compat.py|use|def|resapp/compat.py::use|NULL|NULL|12",
        "resapp/compat.py|xml|import_module|NULL|xml.dom|NULL|6",
        "resapp/compat.py|xml|import_module|NULL|xml.sax|NULL|7",
    }));
    EXPECT_EQ(db_->rows(kRefs, f), (Rows{
        "CALLS|resapp/compat.py::use|resapp/compat.py::use|helper|helper|13",
    }));
}

// ── __all__ and module-level bindings ─────────────────────────────────────────

TEST_F(Extraction, AllMarksModuleLevelBindings)
{
    // allmod.py imports helper and bump but lists only helper in __all__, so a
    // wildcard import of it must not see bump. Bindings below module level and
    // modules without a literal __all__ keep the default.
    const char* kExported =
        "SELECT local_name, is_exported FROM binding"
        " WHERE file_id = ?1 AND scope_id = ?1 ORDER BY local_name;";
    EXPECT_EQ(db_->rows(kExported, "resapp/allmod.py"),
              (Rows{"__all__|0", "bump|0", "helper|1"}));
    EXPECT_EQ(db_->rows(kExported, "resapp/__init__.py"),
              (Rows{"PublicB|1", "__all__|0", "helper|1"}));
    EXPECT_EQ(db_->count("SELECT COUNT(*) FROM binding"
                         " WHERE file_id = 'resapp/compat.py' AND is_exported = 0;"), 0);
    EXPECT_EQ(db_->count("SELECT COUNT(*) FROM binding"
                         " WHERE scope_id <> file_id AND is_exported = 0;"), 0);
}

TEST_F(Extraction, WithoutAllUnderscoreNamesAreNotExported)
{
    // under.py has no __all__: `import *` skips names starting with `_`, the
    // module's symbols and its imports alike.
    EXPECT_EQ(db_->rows("SELECT local_name, is_exported FROM binding"
                        " WHERE file_id = ?1 AND scope_id = ?1 ORDER BY local_name;", "resapp/under.py"),
              (Rows{"_os|0", "_priv|0", "pub|1"}));
    EXPECT_EQ(db_->rows("SELECT name, is_exported FROM symbol WHERE file_id = ? ORDER BY name;",
                        "resapp/under.py"),
              (Rows{"_priv|0", "pub|1"}));
}

// ── resapp/cases.py: a generic base ───────────────────────────────────────────

TEST_F(Extraction, SubscriptedBaseIsARef)
{
    // `class Typed(Child[int])`: the ref names the subscripted value, Child.
    // There is no base_class row for it, as before refs existed.
    EXPECT_EQ(db_->rows("SELECT kind, source_id, scope_id, name_path, raw_text, line FROM ref"
                        " WHERE source_id = 'resapp/cases.py::Typed';"),
              (Rows{"INHERITS|resapp/cases.py::Typed|resapp/cases.py|Child|Child|63"}));
    EXPECT_EQ(db_->count("SELECT COUNT(*) FROM base_class WHERE class_id = 'resapp/cases.py::Typed';"), 0);
}

// ── whole-scan invariants ─────────────────────────────────────────────────────

TEST_F(Extraction, EmptyPackageInitHasOnlyItsFileScope)
{
    const std::string f = "resapp/sub/__init__.py";
    EXPECT_EQ(db_->rows(kScopes, f), (Rows{"resapp/sub/__init__.py|NULL|file|0|0"}));
    EXPECT_EQ(db_->rows(kSymbols, f), Rows{});
    EXPECT_EQ(db_->rows(kBindings, f), Rows{});
}

TEST_F(Extraction, RefsAreIdentifierChainsAndPending)
{
    // The parser writes refs, bindings and module specs only; every link --
    // IMPORTS too -- follows from the resolver's answers, and this scan has no
    // language to resolve with.
    EXPECT_EQ(db_->count("SELECT COUNT(*) FROM link;"), 0);
    EXPECT_EQ(db_->count("SELECT COUNT(*) FROM ref"
                         " WHERE name_path GLOB '*[^A-Za-z0-9_.]*';"), 0);
    EXPECT_EQ(db_->count("SELECT COUNT(*) FROM ref"
                         " WHERE status <> 'pending' OR resolved_id IS NOT NULL OR emits_link <> 0;"), 0);
}

TEST_F(Extraction, EveryClassAndFunctionHasASymbol)
{
    EXPECT_EQ(db_->count("SELECT COUNT(*) FROM class"
                         " WHERE class_id NOT IN (SELECT symbol_id FROM symbol WHERE kind = 'class');"), 0);
    EXPECT_EQ(db_->count("SELECT COUNT(*) FROM function"
                         " WHERE function_id NOT IN (SELECT symbol_id FROM symbol"
                         "                           WHERE kind IN ('function', 'method'));"), 0);
}

TEST_F(Extraction, ImportSpecsArePending)
{
    // One row per module spec the file's imports name, unanswered.
    EXPECT_EQ(db_->rows("SELECT module_spec, resolved_file_id, status FROM import_resolution"
                        " WHERE file_id = ? ORDER BY 1;", "resapp/compat.py"),
              (Rows{".util|NULL|pending", "xml.dom|NULL|pending", "xml.sax|NULL|pending"}));
    EXPECT_EQ(db_->count("SELECT COUNT(*) FROM import_resolution WHERE status <> 'pending';"), 0);
}

// ── DbUpdater ─────────────────────────────────────────────────────────────────
// The per-file delete reaches the new tables through ON DELETE CASCADE on
// file_id, and updateFile writes them back.

TEST(ExtractionUpdate, DeleteAndUpdateFileRoundTrip)
{
    ScannedDb db("update");
    db.scan();
    const std::string f = "resapp/util.py";
    const char* kCounts =
        "SELECT (SELECT COUNT(*) FROM symbol  WHERE file_id = ?1)"
        "  || ',' || (SELECT COUNT(*) FROM scope   WHERE file_id = ?1)"
        "  || ',' || (SELECT COUNT(*) FROM binding WHERE file_id = ?1)"
        "  || ',' || (SELECT COUNT(*) FROM ref     WHERE file_id = ?1);";
    const Rows before = db.rows(kCounts, f);
    ASSERT_EQ(before, (Rows{"7,5,10,2"}));

    ASSERT_EQ(DbUpdater::deleteFile(db.path().c_str(), f), SQLITE_OK);
    db.open();
    EXPECT_EQ(db.rows(kCounts, f), (Rows{"0,0,0,0"}));
    // Other files keep theirs.
    EXPECT_EQ(db.count("SELECT COUNT(*) FROM symbol WHERE file_id = 'resapp/models.py';"), 6);

    PythonParser parser;
    const ParseResult r = parser.parseFile(
        (fs::path(TS_RESOLUTION_FIXTURE) / f).string(), TS_RESOLUTION_FIXTURE);
    ASSERT_EQ(DbUpdater::updateFile(db.path().c_str(), r), SQLITE_OK);
    // updateFile on a file that is already there replaces rather than duplicates.
    ASSERT_EQ(DbUpdater::updateFile(db.path().c_str(), r), SQLITE_OK);
    db.open();
    EXPECT_EQ(db.rows(kCounts, f), before);
}

// ── global / nonlocal imports ─────────────────────────────────────────────────

TEST(ExtractionDeclared, ImportOfADeclaredNameBindsWhereTheNameLives)
{
    // An import is an assignment: under `global y` it binds the module's y,
    // under `nonlocal z` the enclosing function's z. Its module spec has to
    // reach a binding somewhere, or import_resolution -- and the IMPORTS link --
    // never sees it. A plain assignment to a declared name still binds nothing.
    const fs::path dir = fs::temp_directory_path() / "ts_parser_test_declared";
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::ofstream(dir / "t.py") <<
        "y = None\n"
        "def hook():\n"
        "    global y\n"
        "    from .mod import x as y\n"
        "    y = 2\n"
        "def outer():\n"
        "    z = 0\n"
        "    def inner():\n"
        "        nonlocal z\n"
        "        import pkg.mod as z\n"
        "    class K:\n"
        "        global w\n"
        "        import os as w\n";
    PythonParser parser;
    const ParseResult r = parser.parseFile((dir / "t.py").string(), dir.string());
    fs::remove_all(dir);

    Rows bindings;
    for (const BindingEntity& b : r.bindings)
        bindings.push_back(b.scope_id + "|" + b.local_name + "|" + b.kind + "|" + b.module_spec + "|" +
                           std::to_string(b.line));
    std::sort(bindings.begin(), bindings.end());
    EXPECT_EQ(bindings, (Rows{
        "t.py::outer|K|def||10",
        "t.py::outer|inner|def||7",
        "t.py::outer|z|def||6",
        "t.py::outer|z|import_module|pkg.mod|9",   // nonlocal: outer's z
        "t.py|hook|def||1",
        "t.py|outer|def||5",
        "t.py|w|import_module|os|12",              // global in a class body
        "t.py|y|def||0",
        "t.py|y|import_symbol|.mod|3",             // global; `y = 2` binds nothing
    }));
}

namespace {

// t.py with the given text, parsed on its own.
ParseResult parseSource(const std::string& tag, const std::string& text)
{
    const fs::path dir = fs::temp_directory_path() / ("ts_parser_test_" + tag);
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::ofstream(dir / "t.py") << text;
    PythonParser parser;
    ParseResult r = parser.parseFile((dir / "t.py").string(), dir.string());
    fs::remove_all(dir);
    return r;
}

// The import bindings of r, as scope|name|module_spec|line.
Rows importBindings(const ParseResult& r)
{
    Rows out;
    for (const BindingEntity& b : r.bindings)
        if (!b.module_spec.empty())
            out.push_back(b.scope_id + "|" + b.local_name + "|" + b.module_spec + "|" +
                          std::to_string(b.line));
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace

TEST(ExtractionDeclared, NonlocalChainBindsInTheFunctionThatOwnsTheName)
{
    // c's nonlocal x is b's, and b's is nonlocal too: the name is a's.
    const ParseResult r = parseSource("declared_chain",
        "def a():\n"
        "    x = 0\n"
        "    def b():\n"
        "        nonlocal x\n"
        "        def c():\n"
        "            nonlocal x\n"
        "            import pkg.mod as x\n");
    EXPECT_EQ(importBindings(r), (Rows{"t.py::a|x|pkg.mod|6"}));
}

TEST(ExtractionDeclared, NonlocalInAMethodSkipsTheClassBody)
{
    // A class body is not an enclosing scope: m's nonlocal v is f's.
    const ParseResult r = parseSource("declared_method",
        "def f():\n"
        "    v = 0\n"
        "    class K:\n"
        "        v = 1\n"
        "        def m(self):\n"
        "            nonlocal v\n"
        "            import os as v\n");
    EXPECT_EQ(importBindings(r), (Rows{"t.py::f|v|os|6"}));
}

// ── Import specs ──────────────────────────────────────────────────────────────

TEST(ExtractionImports, EverySpecOfEveryImportIsListed)
{
    // Two imports that bind `pkg` on one line keep one binding, but each
    // names its own module; a spec imported twice is listed once.
    const ParseResult r = parseSource("import_specs",
        "import pkg.h, pkg.i\n"
        "import pkg.j; import pkg.k\n"
        "from . import x\n"
        "from .y import *\n"
        "def f():\n"
        "    import pkg.h\n");
    EXPECT_EQ(r.import_specs, (Rows{"pkg.h", "pkg.i", "pkg.j", "pkg.k", ".", ".y"}));
    EXPECT_EQ(importBindings(r), (Rows{
        "t.py::f|pkg|pkg.h|5",
        "t.py|*3|.y|3",
        "t.py|pkg|pkg.h|0",
        "t.py|pkg|pkg.j|1",
        "t.py|x|.|2",
    }));
}

TEST(ExtractionWalk, VirtualEnvironmentsAndCachesAreNotParsed)
{
    // A repository's own code is parsed; an installed environment inside it is not.
    // An environment is recognised by its pyvenv.cfg marker, whatever its name.
    const fs::path dir = fs::temp_directory_path() / "ts_parser_test_walk";
    fs::remove_all(dir);
    for (const char* sub : {"pkg", ".venv/lib/python3.11/site-packages/requests", "venv/lib",
                            "env311/lib", "pkg/__pycache__", "node_modules/x", "Lib/venv"})
        fs::create_directories(dir / sub);
    std::ofstream(dir / "pkg/app.py") << "x = 1\n";
    std::ofstream(dir / "pkg/vendor_like.py") << "y = 2\n";
    std::ofstream(dir / ".venv/lib/python3.11/site-packages/requests/api.py") << "z = 3\n";
    std::ofstream(dir / ".venv/pyvenv.cfg") << "home = /usr/bin\n";
    std::ofstream(dir / "venv/pyvenv.cfg") << "home = /usr/bin\n";
    std::ofstream(dir / "venv/lib/a.py") << "a = 1\n";
    // A package named venv (CPython's Lib/venv) is repository source, not an environment.
    std::ofstream(dir / "Lib/venv/__init__.py") << "e = 1\n";
    std::ofstream(dir / "env311/pyvenv.cfg") << "home = /usr/bin\n";
    std::ofstream(dir / "env311/lib/b.py") << "b = 1\n";
    std::ofstream(dir / "pkg/__pycache__/c.py") << "c = 1\n";
    std::ofstream(dir / "node_modules/x/d.py") << "d = 1\n";

    PythonParser parser;
    const auto results = parser.parseDirectory(dir.string(), dir.string());
    fs::remove_all(dir);

    std::vector<std::string> ids;
    for (const auto& r : results) ids.push_back(r.file.file_id);
    std::sort(ids.begin(), ids.end());
    EXPECT_EQ(ids, (std::vector<std::string>{"Lib/venv/__init__.py", "pkg/app.py", "pkg/vendor_like.py"}));
}
