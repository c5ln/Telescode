// tests/resolve/test_incremental.cpp
// DbUpdater re-resolves only the refs an update can have changed
// (Resolver::resolveChanged). Whatever it skips, the database must come out as
// a fresh scan of the same files makes it: every case here edits a tree,
// applies the edit with a DbUpdater operation, scans the edited tree into a
// second database, and compares the two -- link, ref (all but ref_id),
// import_resolution, binding, symbol, scope, file, fact / fact_edge / ref_dep
// (by key), and after `algo` the reading_sequence scores bit for bit.

#include "algo/AlgoDbWriter.h"
#include "algo/AlgoRunner.h"
#include "db/DbInserter.h"
#include "db/DbUpdater.h"
#include "db/db.h"
#include "parser/PythonParser.h"
#include "resolve/Resolver.h"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;

#ifndef TS_RESOLUTION_FIXTURE
#error "TS_RESOLUTION_FIXTURE must point at tests/fixtures/resolution"
#endif

namespace {

using Rows = std::vector<std::string>;

// A tree of files the test edits: a copy of the fixture, or the given files.
class Tree {
public:
    explicit Tree(const std::string& tag) : root_(path("ts_incr_tree_" + tag)) {
        fs::remove_all(root_);
        fs::copy(TS_RESOLUTION_FIXTURE, root_, fs::copy_options::recursive);
    }
    Tree(const std::string& tag, const std::map<std::string, std::string>& files)
        : root_(path("ts_incr_tree_" + tag)) {
        fs::remove_all(root_);
        for (const auto& [fileId, text] : files) write(fileId, text);
    }
    ~Tree() {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    const std::string& root() const { return root_; }
    std::string file(const std::string& fileId) const { return (fs::path(root_) / fileId).string(); }

    void write(const std::string& fileId, const std::string& text) const {
        fs::create_directories(fs::path(file(fileId)).parent_path());
        std::ofstream(file(fileId), std::ios::trunc) << text;
    }
    std::string read(const std::string& fileId) const {
        std::ifstream in(file(fileId));
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    void remove(const std::string& fileId) const { fs::remove(file(fileId)); }
    void move(const std::string& from, const std::string& to) const {
        fs::create_directories(fs::path(file(to)).parent_path());
        fs::rename(file(from), file(to));
    }

    std::vector<std::string> fileIds() const {
        std::vector<std::string> out;
        for (const auto& e : fs::recursive_directory_iterator(root_))
            if (e.path().extension() == ".py")
                out.push_back(fs::relative(e.path(), root_).generic_string());
        std::sort(out.begin(), out.end());
        return out;
    }

    ParseResult parse(const std::string& fileId) const {
        PythonParser parser;
        return parser.parseFile(file(fileId), root_);
    }

private:
    static std::string path(const std::string& name) {
        return (fs::temp_directory_path() / name).string();
    }
    std::string root_;
};

std::string dbPath(const std::string& tag)
{
    return (fs::temp_directory_path() / ("ts_incr_" + tag + ".db")).string();
}

void removeDb(const std::string& path)
{
    for (const char* suffix : {"", "-wal", "-shm"}) {
        std::error_code ec;
        fs::remove(path + suffix, ec);
    }
}

int scan(const Tree& tree, const std::string& path)
{
    PythonParser parser;
    return DbInserter::insertAll(path.c_str(), parser.parseDirectory(tree.root(), tree.root()));
}

Rows query(sqlite3* db, const std::string& sql)
{
    Rows out;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        ADD_FAILURE() << "bad query: " << sql << ": " << sqlite3_errmsg(db);
        return out;
    }
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

// Everything an update must leave as a scan does, each table's rows sorted.
std::map<std::string, Rows> contents(const std::string& path)
{
    static const std::map<std::string, std::string> kTables = {
        {"link", "SELECT source_id, target_id, link_type FROM link"},
        {"ref", "SELECT file_id, source_id, scope_id, kind, subkind, namespace, name_path,"
                "       raw_text, line, resolved_id, status, confidence, resolver, emits_link"
                "  FROM ref"},
        {"import_resolution", "SELECT * FROM import_resolution"},
        {"binding", "SELECT * FROM binding"},
        {"symbol", "SELECT * FROM symbol"},
        {"scope", "SELECT * FROM scope"},
        {"file", "SELECT * FROM file"},
        // What the answers rest on, by key: ids are numbered afresh by a scan.
        // A fact no ref reads any more must have gone, or every update leaves
        // more behind.
        {"fact", "SELECT replace(key, char(10), ' ') FROM fact"},
        {"fact_edge", "SELECT replace(p.key, char(10), ' '), replace(c.key, char(10), ' ')"
                      "  FROM fact_edge e JOIN fact p ON p.fact_id = e.parent_id"
                      "  JOIN fact c ON c.fact_id = e.child_id"},
        {"ref_dep", "SELECT replace(f.key, char(10), ' '), r.file_id, r.source_id, r.scope_id,"
                    "       r.kind, r.name_path, r.line, r.raw_text"
                    "  FROM ref_dep d JOIN fact f ON f.fact_id = d.fact_id"
                    "  JOIN ref r ON r.ref_id = d.ref_id"},
        // Each file's links in ref_id order. An update renumbers the refs of
        // the files it re-parses and so moves them after every other file's,
        // but keeps their order within the file; the scores only depend on
        // that (Graph.cpp adds a node's out-edges in ref_id order, and the
        // passes sum per source node).
        {"edge order", "SELECT source_id, resolved_id, kind FROM ref"
                       " WHERE emits_link = 1 AND kind IN ('CALLS', 'INHERITS')"
                       " ORDER BY kind, file_id, ref_id"},
    };
    std::map<std::string, Rows> out;
    sqlite3* db = nullptr;
    EXPECT_EQ(initDb(path.c_str(), &db), SQLITE_OK);
    for (const auto& [table, sql] : kTables) {
        Rows rows = query(db, sql);
        if (table != "edge order") std::sort(rows.begin(), rows.end());
        out[table] = std::move(rows);
    }
    sqlite3_close(db);
    return out;
}

// `algo` on a copy of the database (it writes scores into `file` too): every
// reading_sequence row, scores in hex so that equal means equal to the last bit.
Rows readingSequence(const std::string& db)
{
    const std::string path = db + ".algo";
    removeDb(path);
    for (const char* suffix : {"", "-wal"}) {
        std::error_code ec;
        if (fs::exists(db + suffix)) fs::copy_file(db + suffix, path + suffix, ec);
    }
    const AlgoConfig cfg = AlgoDbWriter::loadConfig(path.c_str());
    const AlgoRunResult result = AlgoRunner::run(path.c_str(), cfg);
    removeDb(path);
    Rows out;
    for (const ReadingEntry& e : result.entries) {
        char scores[128];
        std::snprintf(scores, sizeof scores, "%a|%a|%a", e.pagerank_score, e.bc_score,
                      e.combined_score);
        out.push_back(e.entity_id + '|' + e.entity_type + '|' + e.file_id + '|' +
                      std::to_string(e.file_rank) + '|' + std::to_string(e.local_rank) + '|' +
                      scores);
    }
    std::sort(out.begin(), out.end());
    return out;
}

// The updated database against a fresh scan of the tree as it now is.
void expectLikeAScan(const Tree& tree, const std::string& updated, const std::string& what,
                     bool algo = false)
{
    const std::string fresh = updated + ".fresh";
    removeDb(fresh);
    ASSERT_EQ(scan(tree, fresh), SQLITE_OK);
    const auto want = contents(fresh);
    const auto got = contents(updated);
    for (const auto& [table, rows] : want) {
        const Rows& have = got.at(table);
        if (have == rows) continue;
        std::string diff;
        for (const std::string& r : rows)
            if (!std::binary_search(have.begin(), have.end(), r) || table == "edge order")
                diff += "\n  scan only:   " + r;
        for (const std::string& r : have)
            if (!std::binary_search(rows.begin(), rows.end(), r) || table == "edge order")
                diff += "\n  update only: " + r;
        if (diff.size() > 4000) diff.resize(4000);
        ADD_FAILURE() << what << ": " << table << " differs" << diff;
    }
    if (algo && !::testing::Test::HasFailure()) {
        EXPECT_EQ(readingSequence(updated), readingSequence(fresh)) << what << ": reading_sequence";
    }
    removeDb(fresh);
}

// A scanned tree and its database, for a sequence of edits each checked
// against a fresh scan.
class Session {
public:
    Session(const std::string& tag, const std::map<std::string, std::string>& files)
        : tree_(tag, files), db_(dbPath(tag)) { start(); }
    explicit Session(const std::string& tag) : tree_(tag), db_(dbPath(tag)) { start(); }
    ~Session() { removeDb(db_); }

    const Tree& tree() const { return tree_; }
    const std::string& db() const { return db_; }

    // Writes the file and updates it (a new file is added).
    void edit(const std::string& fileId, const std::string& text, bool algo = false) {
        tree_.write(fileId, text);
        ASSERT_EQ(DbUpdater::updateFile(db_.c_str(), tree_.parse(fileId)), SQLITE_OK);
        expectLikeAScan(tree_, db_, "edit " + fileId, algo);
    }
    void editMany(const std::map<std::string, std::string>& files) {
        std::vector<ParseResult> results;
        for (const auto& [fileId, text] : files) tree_.write(fileId, text);
        for (const auto& [fileId, text] : files) results.push_back(tree_.parse(fileId));
        ASSERT_EQ(DbUpdater::updateFiles(db_.c_str(), results), SQLITE_OK);
        expectLikeAScan(tree_, db_, "edit several");
    }
    void remove(const std::string& fileId, bool algo = false) {
        tree_.remove(fileId);
        ASSERT_EQ(DbUpdater::deleteFile(db_.c_str(), fileId), SQLITE_OK);
        expectLikeAScan(tree_, db_, "delete " + fileId, algo);
    }
    void removeMany(const std::vector<std::string>& fileIds) {
        for (const std::string& f : fileIds) tree_.remove(f);
        ASSERT_EQ(DbUpdater::deleteFiles(db_.c_str(), fileIds), SQLITE_OK);
        expectLikeAScan(tree_, db_, "delete several");
    }
    void rename(const std::string& from, const std::string& to, bool algo = false) {
        tree_.move(from, to);
        ASSERT_EQ(DbUpdater::renameFile(db_.c_str(), from, tree_.parse(to)), SQLITE_OK);
        expectLikeAScan(tree_, db_, "rename " + from + " -> " + to, algo);
    }
    // Re-parses a file as it is.
    void touch(const std::string& fileId) {
        ASSERT_EQ(DbUpdater::updateFile(db_.c_str(), tree_.parse(fileId)), SQLITE_OK);
        expectLikeAScan(tree_, db_, "touch " + fileId);
    }

    Rows rows(const std::string& sql) const {
        sqlite3* db = nullptr;
        EXPECT_EQ(initDb(db_.c_str(), &db), SQLITE_OK);
        Rows r = query(db, sql);
        sqlite3_close(db);
        return r;
    }
    // resolved_id|status|confidence of the ref naming `path` in `file`.
    std::string ref(const std::string& file, const std::string& path) const {
        const Rows r = rows("SELECT resolved_id, status, confidence FROM ref WHERE file_id = '" +
                            file + "' AND name_path = '" + path + "';");
        return r.size() == 1 ? r[0] : "<" + std::to_string(r.size()) + " refs>";
    }

private:
    void start() {
        removeDb(db_);
        ASSERT_EQ(scan(tree_, db_), SQLITE_OK);
    }
    Tree tree_;
    std::string db_;
};

}  // namespace

// ── Edits within a file ───────────────────────────────────────────────────────

TEST(Incremental, FunctionBodyEdit)
{
    Session s("body");
    std::string util = s.tree().read("resapp/util.py");
    s.edit("resapp/util.py", util + "\n\ndef extra():\n    return helper(2)\n", true);
    // Lines shift above everything else: ref and binding lines move.
    s.edit("resapp/util.py", "# a comment\n\n" + util, true);
    s.edit("resapp/cases.py", "\n\n" + s.tree().read("resapp/cases.py"), true);
}

TEST(Incremental, AddAndDeleteAFunction)
{
    Session s("addfn", {
        {"pkg/__init__.py", ""},
        {"pkg/lib.py", "def a():\n    return 0\n"},
        {"pkg/use.py", "from .lib import a, b\nfrom . import lib\n\n"
                       "def u():\n    a()\n    b()\n    lib.b()\n    lib.c()\n"},
    });
    EXPECT_EQ(s.ref("pkg/use.py", "b"), "NULL|unresolved|NULL");
    s.edit("pkg/lib.py", "def a():\n    return 0\n\n\ndef b():\n    return a()\n");
    EXPECT_EQ(s.ref("pkg/use.py", "b"), "pkg/lib.py::b|resolved|exact");
    EXPECT_EQ(s.ref("pkg/use.py", "lib.b"), "pkg/lib.py::b|resolved|exact");
    s.edit("pkg/lib.py", "def b():\n    return 0\n\n\ndef c():\n    return b()\n");
    EXPECT_EQ(s.ref("pkg/use.py", "a"), "NULL|unresolved|NULL");
    // b becomes a variable: no CALLS link, the ref still resolves.
    s.edit("pkg/lib.py", "b = 1\n");
    EXPECT_EQ(s.ref("pkg/use.py", "b"), "pkg/lib.py::b|resolved|exact");
}

TEST(Incremental, MethodAddedToABaseClass)
{
    // Child in another file calls self.m() and Child.m(); m appears on, then
    // leaves, the base two levels up. Constructors too.
    Session s("basemethod", {
        {"pkg/__init__.py", ""},
        {"pkg/base.py", "class A:\n    pass\n\n\nclass B(A):\n    pass\n"},
        {"pkg/child.py",
         "from .base import B\n\n\n"
         "class Child(B):\n"
         "    def go(self):\n        self.m()\n        Child.m(self)\n        return Child()\n"},
        {"pkg/other.py", "from .child import Child\n\n\ndef f():\n    return Child().m()\n"},
    });
    EXPECT_EQ(s.ref("pkg/child.py", "self.m"), "NULL|unresolved|NULL");
    s.edit("pkg/base.py",
           "class A:\n    def m(self):\n        return 0\n\n    def __init__(self):\n        pass\n\n\n"
           "class B(A):\n    pass\n", true);
    EXPECT_EQ(s.ref("pkg/child.py", "self.m"), "pkg/base.py::A::m|resolved|heuristic");
    EXPECT_EQ(s.ref("pkg/child.py", "Child"), "pkg/base.py::A::__init__|resolved|heuristic");
    // B overrides it: the nearer one wins.
    s.edit("pkg/base.py",
           "class A:\n    def m(self):\n        return 0\n\n\n"
           "class B(A):\n    def m(self):\n        return 1\n");
    EXPECT_EQ(s.ref("pkg/child.py", "self.m"), "pkg/base.py::B::m|resolved|heuristic");
    // B no longer derives from A, and has no m.
    s.edit("pkg/base.py", "class A:\n    def m(self):\n        return 0\n\n\nclass B:\n    pass\n");
    EXPECT_EQ(s.ref("pkg/child.py", "self.m"), "NULL|unresolved|NULL");
    // An outside base may supply it.
    s.edit("pkg/base.py", "import os\n\n\nclass B(os.PathLike):\n    pass\n");
    EXPECT_EQ(s.ref("pkg/child.py", "self.m"), "NULL|external|exact");
}

TEST(Incremental, BasesThatDependOnBases)
{
    // Y's base Derived.Inner exists only through Derived's base, which lives
    // in a third file: changing that file changes Y's base, and so what
    // Y's methods call, two files away.
    Session s("nestedbase", {
        {"pkg/__init__.py", ""},
        {"pkg/root.py", "class Base:\n    class Inner:\n        def hi(self):\n            return 0\n"},
        {"pkg/lib.py", "from .root import Base\n\n\nclass Derived(Base):\n    pass\n"},
        {"pkg/nested.py", "from .lib import Derived\n\n\nclass Y(Derived.Inner):\n"
                          "    def go(self):\n        return self.hi()\n"},
        {"pkg/far.py", "from .nested import Y\n\n\nclass Z(Y):\n    def run(self):\n"
                       "        return self.hi()\n"},
    });
    EXPECT_EQ(s.ref("pkg/far.py", "self.hi"), "pkg/root.py::Inner::hi|resolved|heuristic");
    s.edit("pkg/root.py", "class Base:\n    class Inner:\n        def ho(self):\n            return 0\n");
    EXPECT_EQ(s.ref("pkg/far.py", "self.hi"), "NULL|unresolved|NULL");
    s.edit("pkg/root.py", "class Base:\n    pass\n");
    EXPECT_EQ(s.ref("pkg/nested.py", "Derived.Inner"), "NULL|unresolved|NULL");
    s.edit("pkg/root.py",
           "class Base:\n    class Inner:\n        def hi(self):\n            return 0\n", true);
    EXPECT_EQ(s.ref("pkg/nested.py", "Derived.Inner"), "pkg/root.py::Inner|resolved|heuristic");
    s.edit("pkg/lib.py", "class Derived:\n    pass\n");
    EXPECT_EQ(s.ref("pkg/far.py", "self.hi"), "NULL|unresolved|NULL");
}

TEST(Incremental, BasesFromTwoFiles)
{
    // C's bases are answered together: when only A's file changes, B's
    // INHERITS ref is not one of the refs that read it, but C's bases still
    // need it.
    Session s("twobases", {
        {"pkg/__init__.py", ""},
        {"pkg/a.py", "class A:\n    def fa(self):\n        return 0\n"},
        {"pkg/b.py", "class B:\n    def fb(self):\n        return 0\n"},
        {"pkg/c.py", "from .a import A\nfrom .b import B\n\n\nclass C(A, B):\n"
                     "    def go(self):\n        self.fa()\n        self.fb()\n"},
        {"pkg/d.py", "from .c import C\n\n\nclass D(C):\n    def go(self):\n        return self.fb()\n"},
    });
    EXPECT_EQ(s.ref("pkg/d.py", "self.fb"), "pkg/b.py::B::fb|resolved|heuristic");
    s.edit("pkg/a.py", "def A():\n    return 0\n");
    EXPECT_EQ(s.ref("pkg/d.py", "self.fb"), "pkg/b.py::B::fb|resolved|heuristic");
    EXPECT_EQ(s.ref("pkg/c.py", "self.fa"), "NULL|unresolved|NULL");
    s.edit("pkg/a.py", "class A:\n    def fb(self):\n        return 1\n");
    EXPECT_EQ(s.ref("pkg/d.py", "self.fb"), "pkg/a.py::A::fb|resolved|heuristic");
}

TEST(Incremental, ABaseThatBecomesAClassInPlace)
{
    // C's INHERITS ref names pkg/a.py::A before and after, but only after is
    // it a class, so a base: C's bases changed though its stored answers did
    // not, and self.fa, which read them, is answered again.
    Session s("kindflip", {
        {"pkg/__init__.py", ""},
        {"pkg/a.py", "def A():\n    return 0\n"},
        {"pkg/c.py", "from .a import A\n\n\nclass C(A):\n"
                     "    def go(self):\n        self.fa()\n"},
    });
    EXPECT_EQ(s.ref("pkg/c.py", "self.fa"), "NULL|unresolved|NULL");
    s.edit("pkg/a.py", "class A:\n    def fa(self):\n        return 1\n");
    EXPECT_EQ(s.ref("pkg/c.py", "self.fa"), "pkg/a.py::A::fa|resolved|heuristic");
    s.edit("pkg/a.py", "def A():\n    return 0\n");
    EXPECT_EQ(s.ref("pkg/c.py", "self.fa"), "NULL|unresolved|NULL");
}

TEST(Incremental, ABaseThatBecomesAFunctionInPlace)
{
    // The reverse: A stops being a base of C, so what C's methods find
    // through A goes; B's member is still found, now past nothing.
    Session s("kindflip_back", {
        {"pkg/__init__.py", ""},
        {"pkg/a.py", "class A:\n    def fa(self):\n        return 1\n"},
        {"pkg/b.py", "class B:\n    def fb(self):\n        return 1\n"},
        {"pkg/c.py", "from .a import A\nfrom .b import B\n\n\nclass C(A, B):\n"
                     "    def go(self):\n        self.fa()\n        self.fb()\n"},
        {"pkg/d.py", "from .c import C\n\n\nclass D(C):\n    def go(self):\n"
                     "        return self.fb()\n"},
    });
    EXPECT_EQ(s.ref("pkg/c.py", "self.fa"), "pkg/a.py::A::fa|resolved|heuristic");
    s.edit("pkg/a.py", "def A():\n    return 0\n");
    EXPECT_EQ(s.ref("pkg/c.py", "self.fa"), "NULL|unresolved|NULL");
    EXPECT_EQ(s.ref("pkg/d.py", "self.fb"), "pkg/b.py::B::fb|resolved|heuristic");
    // An empty class becoming a function changes no member C reads, only
    // C's bases.
    s.edit("pkg/a.py", "class A:\n    pass\n");
    s.edit("pkg/a.py", "def A():\n    return 0\n");
}

// ── Imports and re-exports ────────────────────────────────────────────────────

namespace {

// pkg/r1 .. r5 pass `deep` on from r6, which defines it; use.py imports it
// from r1: five hops.
std::map<std::string, std::string> chainTree()
{
    std::map<std::string, std::string> files{{"pkg/__init__.py", "from .r1 import deep\n"}};
    for (int i = 1; i <= 5; ++i)
        files["pkg/r" + std::to_string(i) + ".py"] =
            "from .r" + std::to_string(i + 1) + " import deep\n";
    files["pkg/r6.py"] = "def deep():\n    return 0\n";
    files["use.py"] = "from pkg.r1 import deep\nimport pkg\n\n\ndef u():\n    deep()\n    pkg.deep()\n";
    return files;
}

}  // namespace

TEST(Incremental, ReexportChains)
{
    Session s("chain", chainTree());
    EXPECT_EQ(s.ref("use.py", "deep"), "pkg/r6.py::deep|resolved|exact");
    // pkg.deep is six hops through __init__: too far.
    EXPECT_EQ(s.ref("use.py", "pkg.deep"), "NULL|unresolved|NULL");
    // The middle of the chain stops passing it on, then starts again.
    s.edit("pkg/r3.py", "deep = None\n");
    EXPECT_EQ(s.ref("use.py", "deep"), "pkg/r3.py::deep|resolved|exact");
    s.edit("pkg/r3.py", "from .r4 import deep\n", true);
    EXPECT_EQ(s.ref("use.py", "deep"), "pkg/r6.py::deep|resolved|exact");
    // A shorter chain brings pkg.deep within reach.
    s.edit("pkg/__init__.py", "from .r2 import deep\n");
    EXPECT_EQ(s.ref("use.py", "pkg.deep"), "pkg/r6.py::deep|resolved|exact");
    // Re-export removed from the package init, then a wildcard in its place.
    s.edit("pkg/__init__.py", "");
    EXPECT_EQ(s.ref("use.py", "pkg.deep"), "NULL|unresolved|NULL");
    s.edit("pkg/__init__.py", "from .r5 import *\n", true);
    EXPECT_EQ(s.ref("use.py", "pkg.deep"), "pkg/r6.py::deep|resolved|exact");
    // The definition goes: every hop's answer changes.
    s.edit("pkg/r6.py", "");
    s.edit("pkg/r6.py", "def deep():\n    return 1\n");
}

TEST(Incremental, SharedAnswerThatPassesThroughTheAskersFile)
{
    // a.py's h() goes through c.py back into a.py, on to e.py. c.py's answer
    // for h is shared: d.py's h() reuses it. An update of a.py answers a.py's
    // refs first, so a.py is then the file that asked -- what the shared
    // answer read of a.py must be kept all the same. a.py then takes k from
    // f.py instead: only a.py's binding of k says so.
    Session s("shared", {
        {"pkg/__init__.py", ""},
        {"pkg/a.py", "from .c import h\nfrom .e import k\n\n\ndef ah():\n    return h()\n"},
        {"pkg/c.py", "from .a import k as h\n"},
        {"pkg/d.py", "from .c import h\n\n\ndef dd():\n    return h()\n"},
        {"pkg/e.py", "def k():\n    return 0\n"},
        {"pkg/f.py", "def k():\n    return 1\n"},
    });
    EXPECT_EQ(s.ref("pkg/d.py", "h"), "pkg/e.py::k|resolved|exact");
    s.edit("pkg/a.py", "# first\nfrom .c import h\nfrom .e import k\n\n\ndef ah():\n    return h()\n");
    s.edit("pkg/a.py", "from .c import h\nfrom .f import k\n\n\ndef ah():\n    return h()\n");
    EXPECT_EQ(s.ref("pkg/d.py", "h"), "pkg/f.py::k|resolved|exact");
}

TEST(Incremental, StarImports)
{
    Session s("star", {
        {"pkg/__init__.py", ""},
        {"pkg/a.py", "def one():\n    return 0\n\n\ndef _two():\n    return 0\n"},
        {"pkg/b.py", "from .a import *\n"},
        {"pkg/use.py", "from .b import *\nfrom os import *\n\n\ndef u():\n    one()\n    _two()\n    three()\n"},
    });
    EXPECT_EQ(s.ref("pkg/use.py", "one"), "pkg/a.py::one|resolved|exact");
    EXPECT_EQ(s.ref("pkg/use.py", "three"), "NULL|external|exact");
    s.edit("pkg/a.py", "def one():\n    return 0\n\n\ndef three():\n    return 0\n");
    EXPECT_EQ(s.ref("pkg/use.py", "three"), "pkg/a.py::three|resolved|exact");
    // __all__ leaves a name out of the wildcard.
    s.edit("pkg/a.py", "__all__ = ['three']\n\n\ndef one():\n    return 0\n\n\ndef three():\n    return 0\n");
    EXPECT_EQ(s.ref("pkg/use.py", "one"), "NULL|external|exact");
    s.edit("pkg/a.py", "__all__ = ['one', '_two']\n\n\ndef one():\n    return 0\n\n\ndef _two():\n    return 0\n");
    EXPECT_EQ(s.ref("pkg/use.py", "_two"), "pkg/a.py::_two|resolved|exact");
    // The middle module stops re-exporting by wildcard.
    s.edit("pkg/b.py", "from .a import one\n", true);
    s.edit("pkg/b.py", "from .a import *\nfrom .a import one\n");
}

TEST(Incremental, PrecedenceBetweenBindingsFlips)
{
    // try/except: a project function beats a fallback variable, an outside
    // module beats a project variable. Each edit flips which binding wins.
    Session s("precedence", {
        {"pkg/__init__.py", ""},
        {"pkg/util.py", "def helper():\n    return 0\n"},
        {"pkg/use.py",
         "try:\n    from .util import helper\nexcept ImportError:\n    helper = None\n\n"
         "try:\n    from .util import Store\nexcept ImportError:\n    Store = object\n\n\n"
         "def u():\n    helper()\n    Store()\n"},
    });
    EXPECT_EQ(s.ref("pkg/use.py", "helper"), "pkg/util.py::helper|resolved|heuristic");
    s.edit("pkg/util.py", "helper = 1\n\n\nclass Store:\n    def __init__(self):\n        pass\n");
    EXPECT_EQ(s.ref("pkg/use.py", "Store"), "pkg/util.py::Store::__init__|resolved|heuristic");
    s.edit("pkg/util.py", "from os import Store\n\n\ndef helper():\n    return 0\n");
    EXPECT_EQ(s.ref("pkg/use.py", "Store"), "NULL|external|heuristic");
    s.edit("pkg/util.py", "");
}

TEST(Incremental, GlobalAndSameLineImports)
{
    Session s("globals", {
        {"pkg/__init__.py", ""},
        {"pkg/mod.py", "def x():\n    return 0\n"},
        {"pkg/h.py", "def f():\n    return 0\n"},
        {"pkg/user.py", "y = None\n\n\ndef hook():\n    global y\n    from .mod import x as y\n\n\n"
                        "def later():\n    return y()\n"},
        {"pkg/same.py", "import pkg.h, pkg.mod\n\n\ndef g():\n    pkg.h.f()\n    pkg.mod.x()\n"},
    });
    EXPECT_EQ(s.ref("pkg/same.py", "pkg.mod.x"), "pkg/mod.py::x|resolved|exact");
    s.edit("pkg/mod.py", "def z():\n    return 0\n");
    EXPECT_EQ(s.ref("pkg/same.py", "pkg.mod.x"), "NULL|unresolved|NULL");
    s.remove("pkg/h.py");
    s.edit("pkg/h.py", "def f():\n    return 1\n");
    s.edit("pkg/mod.py", "def x():\n    return 0\n", true);
}

// ── Files coming and going ────────────────────────────────────────────────────

TEST(Incremental, ModulesAddedDeletedRenamed)
{
    Session s("files", {
        {"app/__init__.py", ""},
        {"app/main.py",
         "import newpkg\nimport newpkg.sub\nfrom app import gone\nfrom . import later\n"
         "from .gone import g\nimport app.later as al\n\n\n"
         "def m():\n    newpkg.f()\n    newpkg.sub.s()\n    gone.g()\n    g()\n    later.h()\n    al.h()\n"},
        {"app/gone.py", "def g():\n    return 0\n"},
    });
    EXPECT_EQ(s.ref("app/main.py", "newpkg.f"), "NULL|external|exact");
    // An outside package becomes a project one.
    s.edit("newpkg/__init__.py", "def f():\n    return 0\n");
    EXPECT_EQ(s.ref("app/main.py", "newpkg.f"), "newpkg/__init__.py::f|resolved|exact");
    EXPECT_EQ(s.ref("app/main.py", "newpkg.sub.s"), "NULL|unresolved|NULL");
    s.edit("newpkg/sub.py", "def s():\n    return 0\n", true);
    EXPECT_EQ(s.ref("app/main.py", "newpkg.sub.s"), "newpkg/sub.py::s|resolved|exact");
    // A module that is imported goes, and comes back under another name.
    s.remove("app/gone.py", true);
    EXPECT_EQ(s.ref("app/main.py", "g"), "NULL|unresolved|NULL");
    s.edit("app/gone.py", "def g():\n    return 0\n");
    s.rename("app/gone.py", "app/later.py", true);
    EXPECT_EQ(s.ref("app/main.py", "later.h"), "NULL|unresolved|NULL");
    s.edit("app/later.py", "def h():\n    return 0\n");
    EXPECT_EQ(s.ref("app/main.py", "al.h"), "app/later.py::h|resolved|exact");
    // The package itself goes: back to external.
    s.removeMany({"newpkg/__init__.py", "newpkg/sub.py"});
    EXPECT_EQ(s.ref("app/main.py", "newpkg.f"), "NULL|external|exact");
    // A package's init goes: its submodules stay, as a namespace package.
    s.remove("app/__init__.py");
    s.edit("app/__init__.py", "from .later import h\n");
}

TEST(Incremental, SeveralFilesAtOnce)
{
    Session s("several", chainTree());
    s.editMany({{"pkg/r2.py", "deep = 0\n"}, {"pkg/r6.py", ""}, {"extra.py", "from pkg.r2 import deep\n"}});
    s.editMany({{"pkg/r2.py", "from .r3 import deep\n"}, {"pkg/r6.py", "def deep():\n    return 0\n"}});
    s.rename("extra.py", "pkg/extra.py");
}

// ── The fixture, file by file ─────────────────────────────────────────────────

TEST(Incremental, FixtureEveryFileTouched)
{
    // Unchanged, re-parsed: ref ids move; nothing else may.
    Session s("fix_touch");
    for (const std::string& f : s.tree().fileIds()) s.touch(f);
    expectLikeAScan(s.tree(), s.db(), "after touching every file", true);
}

TEST(Incremental, FixtureEveryFileDeletedAndRestored)
{
    Session s("fix_delete");
    for (const std::string& f : s.tree().fileIds()) {
        const std::string text = s.tree().read(f);
        s.remove(f);
        s.edit(f, text);
    }
    expectLikeAScan(s.tree(), s.db(), "after deleting every file", true);
}

TEST(Incremental, FixtureEveryFileRenamedAndBack)
{
    Session s("fix_rename");
    for (const std::string& f : s.tree().fileIds()) {
        const std::string moved = "moved/" + fs::path(f).filename().string();
        s.rename(f, moved);
        s.rename(moved, f);
    }
}

TEST(Incremental, FixtureEveryFileEmptiedAndRestored)
{
    Session s("fix_empty");
    for (const std::string& f : s.tree().fileIds()) {
        const std::string text = s.tree().read(f);
        s.edit(f, "");
        s.edit(f, text);
    }
}

// ── Facts no ref reads any more ───────────────────────────────────────────────

TEST(Incremental, RenameChurnLeavesNoFactsBehind)
{
    // Each round renames a function and its caller in another file: the old
    // names' facts (and the E answers that stood for them) are read by
    // nothing any more and must go -- every edit is checked against a scan,
    // fact / fact_edge / ref_dep included -- and the tables stay the size
    // they were.
    auto lib = [](int i) {
        return "def f" + std::to_string(i) + "():\n    return 0\n";
    };
    auto use = [](int i) {
        return "from pkg.lib import f" + std::to_string(i) + "\nimport pkg\n\n\ndef g" +
               std::to_string(i) + "():\n    f" + std::to_string(i) + "()\n    pkg.f" +
               std::to_string(i) + "()\n";
    };
    Session s("churn", {
        {"pkg/__init__.py", "from .lib import *\n"},
        {"pkg/lib.py", lib(0)},
        {"use.py", use(0)},
    });
    const Rows sizes = s.rows("SELECT (SELECT COUNT(*) FROM fact), (SELECT COUNT(*) FROM fact_edge),"
                              " (SELECT COUNT(*) FROM ref_dep);");
    for (int i = 1; i <= 30; ++i) {
        s.editMany({{"pkg/lib.py", lib(i)}, {"use.py", use(i)}});
        s.edit("pkg/lib.py", lib(i) + "\n\ndef h():\n    return f" + std::to_string(i) + "()\n");
        s.edit("pkg/lib.py", lib(i));
        if (::testing::Test::HasFailure()) return;
    }
    EXPECT_EQ(s.rows("SELECT (SELECT COUNT(*) FROM fact), (SELECT COUNT(*) FROM fact_edge),"
                     " (SELECT COUNT(*) FROM ref_dep);"), sizes);
}

TEST(Incremental, MixedEditsLeaveNoFactsBehind)
{
    // Edits of every kind one after another on the fixture, each checked
    // against a scan (fact / fact_edge / ref_dep by key included).
    Session s("mixed_gc");
    const std::vector<std::string> files = s.tree().fileIds();
    std::map<std::string, std::string> original;
    for (const std::string& f : files) original[f] = s.tree().read(f);
    for (std::size_t i = 0; i < files.size(); ++i) {
        const std::string& f = files[i];
        switch (i % 4) {
        case 0: s.edit(f, ""); s.edit(f, original[f]); break;
        case 1: s.remove(f); s.edit(f, original[f]); break;
        case 2: {
            const std::string moved = f.substr(0, f.size() - 3) + "_moved.py";
            s.rename(f, moved);
            s.rename(moved, f);
            break;
        }
        default: s.edit(f, original[f] + "\n\nclass Extra(object):\n    def m(self):\n"
                                         "        return self.m()\n");
                 s.edit(f, original[f]); break;
        }
        if (::testing::Test::HasFailure()) return;
    }
}

// ── Updates without a change left nothing to do ───────────────────────────────

TEST(Incremental, AnUnchangedFileReResolvesOnlyItsOwnRefs)
{
    Session s("scope_of_work");
    // The ref_ids of every file but the one updated stay where they were, and
    // so do their answers: nothing else was written.
    const Rows before = s.rows("SELECT ref_id, file_id FROM ref WHERE file_id <> 'resapp/util.py'"
                               " ORDER BY ref_id;");
    s.touch("resapp/util.py");
    EXPECT_EQ(s.rows("SELECT ref_id, file_id FROM ref WHERE file_id <> 'resapp/util.py'"
                     " ORDER BY ref_id;"), before);
}

TEST(Incremental, DependenciesAreRecorded)
{
    // The facts behind a few answers, as ref_dep has them.
    Session s("deps", {
        {"pkg/__init__.py", ""},
        {"pkg/lib.py", "class A:\n    pass\n"},
        {"pkg/use.py", "from .lib import A\nimport os\n\n\nclass C(A):\n    def go(self):\n"
                       "        self.m()\n        os.getcwd()\n        len([])\n"},
    });
    auto facts = [&](const std::string& path) {
        Rows r = s.rows("SELECT replace(f.key, char(10), ' ') FROM ref r"
                        " JOIN ref_dep d ON d.ref_id = r.ref_id JOIN fact f ON f.fact_id = d.fact_id"
                        " WHERE r.name_path = '" + path + "' ORDER BY 1;");
        return r;
    };
    // Not found on C (its own file, so not recorded), nor on A: A's member
    // and the bases of both are facts. How C's base was found is the INHERITS
    // ref's business: the import, the name in lib.py, what it is.
    EXPECT_EQ(facts("self.m"), (Rows{"C pkg/lib.py::A m", "K pkg/lib.py::A", "K pkg/use.py::C"}));
    // What lib.py's A comes to is an answer of its own, shared by every ref
    // that asks: its facts are its fact_edge rows.
    EXPECT_EQ(facts("A"), (Rows{"E pkg/lib.py value A", "M pkg/use.py .lib", "Y pkg/lib.py::A"}));
    EXPECT_EQ(s.rows("SELECT replace(c.key, char(10), ' ') FROM fact p"
                     " JOIN fact_edge e ON e.parent_id = p.fact_id JOIN fact c ON c.fact_id = e.child_id"
                     " WHERE p.key = 'E' || char(10) || 'pkg/lib.py' || char(10) || 'value' || char(10) || 'A'"
                     " ORDER BY 1;"),
              (Rows{"B pkg/lib.py value A", "Y pkg/lib.py::A"}));
    // An outside module, and the submodule getcwd might have been.
    EXPECT_EQ(facts("os.getcwd"), (Rows{"M pkg/use.py os", "M pkg/use.py os.getcwd"}));
    EXPECT_EQ(facts("len"), Rows{});
}

TEST(Incremental, ScanRefusesAForeignFileBeforeParsing)
{
    // Checked by the CLI (scan_main.cpp) before it reads the repo; here the
    // check itself.
    const std::string path = dbPath("foreign");
    removeDb(path);
    std::ofstream(path) << "not a database";
    EXPECT_EQ(checkDbVersion(path.c_str()), kNotADatabase);
    removeDb(path);
}

// ── What a scan writes ────────────────────────────────────────────────────────

namespace {

// Every object of the schema, as sqlite_master has it.
Rows schemaOf(const std::string& path)
{
    sqlite3* db = nullptr;
    EXPECT_EQ(initDb(path.c_str(), &db), SQLITE_OK);
    Rows r = query(db, "SELECT type, name, tbl_name, sql FROM sqlite_master ORDER BY type, name;");
    sqlite3_close(db);
    return r;
}

Rows newDatabaseSchema()
{
    const std::string path = dbPath("schema_init");
    removeDb(path);
    sqlite3* db = nullptr;
    EXPECT_EQ(initDb(path.c_str(), &db), SQLITE_OK);
    sqlite3_close(db);
    Rows r = schemaOf(path);
    removeDb(path);
    return r;
}

}  // namespace

TEST(Incremental, ScanEndsWithTheSchemaOfANewDatabase)
{
    // A scan builds the indexes once its rows are in (DbInserter::insertAll),
    // and writes with foreign keys off: the result must be what initDb
    // creates, every row with its parent.
    Session s("schema");
    EXPECT_EQ(schemaOf(s.db()), newDatabaseSchema());
    EXPECT_EQ(s.rows("PRAGMA foreign_key_check;"), Rows{});
    EXPECT_EQ(s.rows("PRAGMA integrity_check;"), Rows{"ok"});
}

TEST(Incremental, RescanReplacesEverythingButTheAlgoSettings)
{
    Session s("rescan");
    s.rows("INSERT INTO reading_sequence_config(config_key, config_value) VALUES('probe', '1');");
    s.tree().write("pkg/added.py", "from .lib import A\n\nclass B(A):\n    def run(self):\n        A()\n");
    s.tree().remove(s.tree().fileIds().front());
    ASSERT_EQ(scan(s.tree(), s.db()), SQLITE_OK);
    expectLikeAScan(s.tree(), s.db(), "rescan", true);
    EXPECT_EQ(schemaOf(s.db()), newDatabaseSchema());
    EXPECT_EQ(s.rows("SELECT config_value FROM reading_sequence_config WHERE config_key = 'probe';"),
              Rows{"1"});
    EXPECT_EQ(s.rows("PRAGMA foreign_key_check;"), Rows{});
}

TEST(Incremental, OneRefPerCalleeAndLine)
{
    // No index keeps a ref unique (db.cpp): the scan and DbInserter do.
    Session s("dupref", {{"a.py", "def f():\n    pass\n\ndef g():\n    f(); f()\n"}});
    EXPECT_EQ(s.rows("SELECT COUNT(*) FROM ref WHERE name_path = 'f';"), Rows{"1"});
    s.edit("a.py", "def f():\n    pass\n\ndef g():\n    f(); f(); f()\n    f()\n");
    EXPECT_EQ(s.rows("SELECT COUNT(*) FROM ref WHERE name_path = 'f';"), Rows{"2"});
}

TEST(Incremental, ChecksTakeExactlyTheirValues)
{
    // The CHECKs list their values with OR (db.cpp), not IN: each column must
    // still take every listed value, and nothing else.
    struct Column {
        const char* insert;   // ?1 is the value
        std::vector<const char*> values;
    };
    const std::vector<Column> columns = {
        {"INSERT INTO symbol(symbol_id, file_id, kind, name, start_line, end_line)"
         " VALUES(hex(randomblob(8)), 'f', ?1, 'n', 0, 0);",
         {"class", "interface", "struct", "trait", "enum", "function", "method", "field",
          "variable", "type_alias", "namespace"}},
        {"INSERT INTO symbol(symbol_id, file_id, kind, name, namespace, start_line, end_line)"
         " VALUES(hex(randomblob(8)), 'f', 'class', 'n', ?1, 0, 0);",
         {"value", "type"}},
        {"INSERT INTO scope(scope_id, file_id, kind, start_line, end_line)"
         " VALUES(hex(randomblob(8)), 'f', ?1, 0, 0);",
         {"file", "namespace", "class", "function", "block"}},
        {"INSERT INTO binding(scope_id, local_name, kind, file_id, line)"
         " VALUES('f', hex(randomblob(8)), ?1, 'f', 0);",
         {"def", "import_module", "import_symbol", "import_all", "reexport"}},
        {"INSERT INTO binding(scope_id, local_name, namespace, kind, file_id, line)"
         " VALUES('f', hex(randomblob(8)), ?1, 'def', 'f', 0);",
         {"value", "type"}},
        {"INSERT INTO ref(file_id, source_id, scope_id, kind, name_path, raw_text, line)"
         " VALUES('f', 'f', 'f', ?1, 'x', 'x', 0);",
         {"CALLS", "INHERITS", "IMPLEMENTS", "DECORATES"}},
        {"INSERT INTO ref(file_id, source_id, scope_id, kind, namespace, name_path, raw_text, line)"
         " VALUES('f', 'f', 'f', 'CALLS', ?1, 'x', 'x', 0);",
         {"value", "type"}},
        {"INSERT INTO ref(file_id, source_id, scope_id, kind, name_path, raw_text, line, status)"
         " VALUES('f', 'f', 'f', 'CALLS', 'x', 'x', 0, ?1);",
         {"pending", "resolved", "builtin", "external", "unresolved"}},
        {"INSERT INTO ref(file_id, source_id, scope_id, kind, name_path, raw_text, line, confidence)"
         " VALUES('f', 'f', 'f', 'CALLS', 'x', 'x', 0, ?1);",
         {"exact", "heuristic"}},
        {"INSERT INTO reading_sequence(entity_id, entity_type, file_id, file_rank, local_rank,"
         "                             pagerank_score, bc_score, combined_score)"
         " VALUES(hex(randomblob(8)), ?1, 'f', CASE WHEN ?1 = 'file' THEN 1 END,"
         "        CASE WHEN ?1 <> 'file' THEN 1 END, 0, 0, 0);",
         {"file", "class", "function"}},
    };
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);   // foreign keys off
    ASSERT_EQ(createSchema(db), SQLITE_OK);
    auto insert = [&](const char* sql, const char* value) {
        sqlite3_stmt* stmt = nullptr;
        EXPECT_EQ(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr), SQLITE_OK) << sql;
        sqlite3_bind_text(stmt, 1, value, -1, SQLITE_TRANSIENT);
        const int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        return rc;
    };
    for (const Column& c : columns) {
        for (const char* v : c.values) EXPECT_EQ(insert(c.insert, v), SQLITE_DONE) << c.insert << " " << v;
        // Not listed: another column's value, a case change, a near miss, empty.
        for (const char* v : {"bogus", "Class", "CALLS ", "", "file|class", "value"}) {
            if (std::find_if(c.values.begin(), c.values.end(),
                             [&](const char* x) { return std::string(x) == v; }) != c.values.end())
                continue;
            EXPECT_EQ(insert(c.insert, v), SQLITE_CONSTRAINT) << c.insert << " " << v;
        }
    }
    sqlite3_close(db);
}
