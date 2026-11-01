// tests/cli/test_cli_options.cpp
// ParseJsonCmdOptions and the --out safety check.
//
// The JSON commands close the database before opening the output file, so an
// --out path that names the database is not a locking conflict -- it is a
// truncation. A 233KB database became 30KB of JSON while the command reported
// success. The parser refuses that combination instead.

#include "cli/ts_cli.h"
#include "db/db.h"

#include <gtest/gtest.h>

#include <sqlite3.h>

#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#ifndef _WIN32
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

const char* const kUsage = "Usage: Telescode analyze <db_path> [-o <file>]";

// ParseJsonCmdOptions takes argv as the subcommand saw it: argv[0] is the
// subcommand name, so the caller's first real argument sits at index 1.
bool parse(std::vector<std::string> args, TS::JsonCmdOptions& out)
{
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>("analyze"));
    for (std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    return TS::ParseJsonCmdOptions(static_cast<int>(argv.size()), argv.data(), kUsage, out);
}

} // anonymous namespace

// ── Normal parsing ───────────────────────────────────────────────────────────

TEST(CliOptions, ParsesDatabasePathAndFlags) {
    TS::JsonCmdOptions o;
    ASSERT_TRUE(parse({"db.sqlite", "--json", "--pretty", "--algo"}, o));
    EXPECT_EQ(o.db_path, "db.sqlite");
    EXPECT_TRUE(o.pretty);
    EXPECT_TRUE(o.run_algo);
    EXPECT_TRUE(o.out_path.empty());
}

TEST(CliOptions, DefaultsAreCompactStdoutNoAlgo) {
    TS::JsonCmdOptions o;
    ASSERT_TRUE(parse({"db.sqlite"}, o));
    EXPECT_FALSE(o.pretty);
    EXPECT_FALSE(o.run_algo);
    EXPECT_TRUE(o.out_path.empty());
}

TEST(CliOptions, AcceptsBothOutSpellings) {
    for (const char* flag : {"-o", "--out"}) {
        TS::JsonCmdOptions o;
        ASSERT_TRUE(parse({"db.sqlite", flag, "report.json"}, o)) << flag;
        EXPECT_EQ(o.out_path, "report.json") << flag;
    }
}

TEST(CliOptions, RejectsMissingDatabasePath) {
    TS::JsonCmdOptions o;
    EXPECT_FALSE(parse({"--pretty"}, o));
}

TEST(CliOptions, RejectsUnknownOptionAndExtraArgument) {
    { TS::JsonCmdOptions o; EXPECT_FALSE(parse({"db.sqlite", "--nope"}, o)); }
    { TS::JsonCmdOptions o; EXPECT_FALSE(parse({"a.db", "b.db"}, o)); }
}

TEST(CliOptions, RejectsOutWithNoFollowingPath) {
    TS::JsonCmdOptions o;
    EXPECT_FALSE(parse({"db.sqlite", "-o"}, o));
}

// ── The --out safety check ───────────────────────────────────────────────────

TEST(CliOptions, RejectsOutPathEqualToDatabasePath) {
    TS::JsonCmdOptions o;
    EXPECT_FALSE(parse({"db.sqlite", "-o", "db.sqlite"}, o))
        << "writing the report over its own database must be refused";
}

TEST(CliOptions, RejectsOutPathReachingTheDatabaseIndirectly) {
    TS::JsonCmdOptions o;
    EXPECT_FALSE(parse({"db.sqlite", "-o", "./db.sqlite"}, o));
}

TEST(CliOptions, AllowsAGenuinelyDifferentOutPath) {
    TS::JsonCmdOptions o;
    ASSERT_TRUE(parse({"db.sqlite", "-o", "report.json"}, o));
    EXPECT_EQ(o.out_path, "report.json");
}

TEST(CliOptions, AllowsOutPathThatMerelySharesAPrefix) {
    // db.sqlite-report is a different file; only an exact match may be refused.
    TS::JsonCmdOptions o;
    ASSERT_TRUE(parse({"db.sqlite", "-o", "db.sqlite-report"}, o));
}

// ── SameFilePath ─────────────────────────────────────────────────────────────

TEST(SameFilePath, IdenticalStringsMatch) {
    EXPECT_TRUE(TS::SameFilePath("a/b.db", "a/b.db"));
}

TEST(SameFilePath, DifferentFilesDoNotMatch) {
    EXPECT_FALSE(TS::SameFilePath("a.db", "b.db"));
    EXPECT_FALSE(TS::SameFilePath("a.db", "a.db.json"));
    EXPECT_FALSE(TS::SameFilePath("dir/a.db", "other/a.db"));
}

TEST(SameFilePath, DotSlashResolvesToTheSameFile) {
    EXPECT_TRUE(TS::SameFilePath("a.db", "./a.db"));
    EXPECT_TRUE(TS::SameFilePath("./a.db", "a.db"));
}

TEST(SameFilePath, RedundantSegmentsResolve) {
    EXPECT_TRUE(TS::SameFilePath("dir/../a.db", "a.db"));
}

TEST(SameFilePath, RelativeAndAbsoluteSpellingsOfOneFileMatch) {
    // Uses the real working directory, so this exercises the resolving branch
    // rather than the literal-equality shortcut.
    const fs::path abs = fs::current_path() / "telescode_same_file_probe.db";
    EXPECT_TRUE(TS::SameFilePath("telescode_same_file_probe.db", abs.string()));
}

TEST(SameFilePath, NeitherPathNeedsToExist) {
    // The output file is normally absent until it is written, which is why the
    // implementation cannot use canonical() or equivalent().
    EXPECT_FALSE(fs::exists("telescode_definitely_absent_xyz.json"));
    EXPECT_TRUE(TS::SameFilePath("telescode_definitely_absent_xyz.json",
                                 "./telescode_definitely_absent_xyz.json"));
}

// ── update dangling ──────────────────────────────────────────────────────────
// One importing file per line on stdout, sorted, nothing else.

namespace {

int run(const std::vector<std::string>& args, int (*cmd)(int, char*[]), std::string* out = nullptr,
        std::string* err = nullptr)
{
    std::vector<std::string> copy = args;
    std::vector<char*> argv;
    for (std::string& a : copy) argv.push_back(a.data());
    ::testing::internal::CaptureStdout();
    ::testing::internal::CaptureStderr();
    const int rc = cmd(static_cast<int>(argv.size()), argv.data());
    const std::string text = ::testing::internal::GetCapturedStdout();
    const std::string errText = ::testing::internal::GetCapturedStderr();
    if (out) *out = text;
    if (err) *err = errText;
    return rc;
}

class DanglingTree {
public:
    DanglingTree() : root_(fs::temp_directory_path() / "ts_cli_dangling") {
        fs::remove_all(root_);
        fs::create_directories(root_ / "pkg");
        write("pkg/__init__.py", "");
        write("pkg/mod.py", "def f():\n    return 0\n");
        write("pkg/rel.py", "from .mod import f\n");
        write("absolute.py", "import pkg.mod\n");
        write("other.py", "import os\n");
    }
    ~DanglingTree() { std::error_code ec; fs::remove_all(root_, ec); }

    std::string root() const { return (root_ / "src").string(); }
    std::string db() const { return (root_ / "t.db").string(); }

private:
    void write(const std::string& rel, const char* text) {
        const fs::path p = root_ / "src" / rel;
        fs::create_directories(p.parent_path());
        std::FILE* f = std::fopen(p.string().c_str(), "w");
        std::fputs(text, f);
        std::fclose(f);
    }
    fs::path root_;
};

} // anonymous namespace

TEST(CliScan, RefusesAFileThatIsNotADatabaseBeforeParsing) {
    // The refusal comes before the repo is read: nothing is parsed, nothing
    // written over.
    DanglingTree tree;
    std::FILE* f = std::fopen(tree.db().c_str(), "w");
    std::fputs("my notes, not a database\n", f);
    std::fclose(f);
    std::string out, err;
    EXPECT_EQ(run({"scan", tree.root(), tree.db()}, TS::CmdScan, &out, &err), 1);
    EXPECT_EQ(out.find("Parsed"), std::string::npos) << out;
    EXPECT_NE(err.find("is not a Telescode database"), std::string::npos) << err;
    std::ifstream in(tree.db());
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()),
              "my notes, not a database\n");
}

TEST(CliScan, RefusesAPathItCannotWriteBeforeParsing) {
    // A directory that is not there: SQLite could only say so once the whole
    // repo had been parsed.
    DanglingTree tree;
    const std::string db = (fs::path(tree.db()).parent_path() / "missing" / "t.db").string();
    std::string out, err;
    EXPECT_EQ(run({"scan", tree.root(), db}, TS::CmdScan, &out, &err), 1);
    EXPECT_EQ(out.find("Parsed"), std::string::npos) << out;
    EXPECT_NE(err.find("cannot write a database at " + db), std::string::npos) << err;
    EXPECT_FALSE(fs::exists(fs::path(db).parent_path()));
}

TEST(CliScan, ChecksANewPathWithoutLeavingAFile) {
    // The check creates the file to see that it can; the scan then makes it.
    DanglingTree tree;
    EXPECT_FALSE(fs::exists(tree.db()));
    EXPECT_EQ(checkScanTarget(tree.db().c_str()), SQLITE_OK);
    EXPECT_FALSE(fs::exists(tree.db()));
    std::string out;
    ASSERT_EQ(run({"scan", tree.root(), tree.db()}, TS::CmdScan, &out), 0);
    EXPECT_NE(out.find("Parsed"), std::string::npos) << out;
    // A database that is there, of this version: scanned over again.
    EXPECT_EQ(checkScanTarget(tree.db().c_str()), SQLITE_OK);
    ASSERT_EQ(run({"scan", tree.root(), tree.db()}, TS::CmdScan, &out), 0);
}

TEST(CliScan, WritesThroughADanglingSymlink) {
    // The database goes where the link points; the link stays, and nothing
    // else is left beside them.
    DanglingTree tree;
    const fs::path dir = fs::path(tree.db()).parent_path();
    const fs::path link = dir / "link.db";
    fs::create_symlink("target.db", link);
    std::string out, err;
    ASSERT_EQ(run({"scan", tree.root(), link.string()}, TS::CmdScan, &out, &err), 0) << err;
    EXPECT_TRUE(fs::is_symlink(fs::symlink_status(link)));
    EXPECT_TRUE(fs::is_regular_file(dir / "target.db"));
    EXPECT_GT(fs::file_size(dir / "target.db"), 0u);
    std::vector<std::string> names;
    for (const auto& e : fs::directory_iterator(dir)) names.push_back(e.path().filename().string());
    std::sort(names.begin(), names.end());
    EXPECT_EQ(names, (std::vector<std::string>{"link.db", "pkg", "src", "target.db"}));
}

TEST(CliScan, ReplacesAnOldVersionThroughASymlink) {
    // A database of another version is replaced where the link points; the
    // link stays.
    DanglingTree tree;
    const fs::path dir = fs::path(tree.db()).parent_path();
    const fs::path target = dir / "target.db";
    const fs::path link = dir / "link.db";
    ASSERT_EQ(run({"scan", tree.root(), target.string()}, TS::CmdScan), 0);
    {
        sqlite3* h = nullptr;
        ASSERT_EQ(sqlite3_open(target.string().c_str(), &h), SQLITE_OK);
        ASSERT_EQ(sqlite3_exec(h, "PRAGMA user_version = 1;", nullptr, nullptr, nullptr), SQLITE_OK);
        sqlite3_close(h);
    }
    ASSERT_EQ(checkDbVersion(target.string().c_str()), kWrongDbVersion);
    fs::create_symlink("target.db", link);
    std::string out, err;
    ASSERT_EQ(run({"scan", tree.root(), link.string()}, TS::CmdScan, &out, &err), 0) << err;
    EXPECT_TRUE(fs::is_symlink(fs::symlink_status(link)));
    EXPECT_TRUE(fs::is_regular_file(fs::symlink_status(target)));
    EXPECT_EQ(checkDbVersion(target.string().c_str()), SQLITE_OK);
    sqlite3* h = nullptr;
    ASSERT_EQ(sqlite3_open_v2(target.string().c_str(), &h, SQLITE_OPEN_READONLY, nullptr), SQLITE_OK);
    sqlite3_stmt* st = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(h, "SELECT user_version, (SELECT COUNT(*) FROM file)"
                                    " FROM pragma_user_version;", -1, &st, nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(st), SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_int(st, 0), kDbVersion);
    EXPECT_GT(sqlite3_column_int(st, 1), 0);
    sqlite3_finalize(st);
    sqlite3_close(h);
}

namespace {

// Permissions mean nothing to root: the read-only cases are skipped there.
bool runningAsRoot()
{
#ifndef _WIN32
    return geteuid() == 0;
#else
    return true;
#endif
}

// Scans into db, expecting a refusal before the repo is parsed.
void expectRefusedBeforeParsing(const DanglingTree& tree, const std::string& db)
{
    std::string out, err;
    EXPECT_EQ(run({"scan", tree.root(), db}, TS::CmdScan, &out, &err), 1);
    EXPECT_EQ(out.find("Parsed"), std::string::npos) << out;
    EXPECT_NE(err.find("cannot write a database at " + db), std::string::npos) << err;
}

} // anonymous namespace

TEST(CliScan, RefusesReadOnlyPlacesBeforeParsing) {
    if (runningAsRoot()) GTEST_SKIP() << "root ignores permissions";
    DanglingTree tree;
    const fs::path dir = fs::path(tree.db()).parent_path() / "ro";
    fs::create_directories(dir);
    const std::string db = (dir / "t.db").string();
    ASSERT_EQ(run({"scan", tree.root(), db}, TS::CmdScan), 0);

    // A writable database in a directory that is not: SQLite needs the
    // directory for the -wal.
    fs::permissions(dir, fs::perms::owner_read | fs::perms::owner_exec);
    expectRefusedBeforeParsing(tree, (dir / "new.db").string());
    expectRefusedBeforeParsing(tree, db);
    fs::permissions(dir, fs::perms::owner_all);

    // A read-only database, of this version and of an older one (which a
    // scan would otherwise remove and write anew).
    fs::permissions(db, fs::perms::owner_read);
    expectRefusedBeforeParsing(tree, db);
    {
        sqlite3* h = nullptr;
        fs::permissions(db, fs::perms::owner_read | fs::perms::owner_write);
        ASSERT_EQ(sqlite3_open(db.c_str(), &h), SQLITE_OK);
        ASSERT_EQ(sqlite3_exec(h, "PRAGMA user_version = 1;", nullptr, nullptr, nullptr), SQLITE_OK);
        sqlite3_close(h);
    }
    ASSERT_EQ(checkDbVersion(db.c_str()), kWrongDbVersion);
    fs::permissions(db, fs::perms::owner_read);
    expectRefusedBeforeParsing(tree, db);
    fs::permissions(db, fs::perms::owner_read | fs::perms::owner_write);
}

TEST(CliUpdate, DanglingListsImportersOnePerLine) {
    DanglingTree tree;
    ASSERT_EQ(run({"scan", tree.root(), tree.db()}, TS::CmdScan), 0);

    std::string out;
    ASSERT_EQ(run({"update", "dangling", tree.db(), "pkg/mod.py"}, TS::CmdUpdate, &out), 0);
    EXPECT_EQ(out, "absolute.py\npkg/rel.py\n");

    ASSERT_EQ(run({"update", "dangling", tree.db(), "other.py"}, TS::CmdUpdate, &out), 0);
    EXPECT_EQ(out, "");
}

// ── Database version ─────────────────────────────────────────────────────────
// A database of another version: scan rebuilds it, every other command says
// so and leaves the file exactly as it was.

namespace {

std::string fileBytes(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void setUserVersion(const std::string& path, int version)
{
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(path.c_str(), &db), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(db, ("PRAGMA user_version = " + std::to_string(version) + ";").c_str(),
                           nullptr, nullptr, nullptr), SQLITE_OK);
    sqlite3_close(db);
}

// Every file next to the database, with its bytes: a refusal must not change
// the main file, its -wal or -shm, nor create either.
std::vector<std::pair<std::string, std::string>> dirState(const std::string& db)
{
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& e : fs::directory_iterator(fs::path(db).parent_path()))
        if (e.is_regular_file())
            out.emplace_back(e.path().filename().string(), fileBytes(e.path().string()));
    std::sort(out.begin(), out.end());
    return out;
}

// A WAL-mode database of version 5 at path whose last transaction is still
// only in its -wal, as a crash leaves it: copied while the writer has it open.
void makeLiveWalDb(const std::string& path)
{
    const std::string src = path + ".writer";
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(src.c_str(), &db), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(db,
        "PRAGMA journal_mode = WAL; PRAGMA wal_autocheckpoint = 0;"
        "CREATE TABLE file(file_id TEXT PRIMARY KEY); CREATE TABLE link(source_id, target_id, link_type);"
        "INSERT INTO file VALUES('a.py'); PRAGMA user_version = 5;",
        nullptr, nullptr, nullptr), SQLITE_OK);
    for (const char* suffix : {"", "-wal", "-shm"})
        fs::copy_file(src + suffix, path + suffix, fs::copy_options::overwrite_existing);
    sqlite3_close(db);
    for (const char* suffix : {"", "-wal", "-shm"}) fs::remove(src + suffix);
}

// Every command but scan, on db: each fails, saying `message` (if given), and
// changes nothing.
void expectAllRefused(const DanglingTree& tree,
                      const std::string& message = "created by a different Telescode version; rescan it")
{
    const auto before = dirState(tree.db());
    ASSERT_FALSE(fileBytes(tree.db()).empty());
    const std::string file = tree.root() + "/pkg/mod.py";
    const std::vector<std::pair<std::vector<std::string>, int (*)(int, char*[])>> commands = {
        {{"algo", tree.db()}, TS::CmdAlgo},
        {{"graph", tree.db()}, TS::CmdGraph},
        {{"sequence", tree.db()}, TS::CmdSequence},
        {{"analyze", tree.db()}, TS::CmdAnalyze},
        {{"analyze", tree.db(), "--algo"}, TS::CmdAnalyze},
        {{"update", "file", tree.db(), tree.root(), file}, TS::CmdUpdate},
        {{"update", "files", tree.db(), tree.root(), file}, TS::CmdUpdate},
        {{"update", "delete", tree.db(), "pkg/mod.py"}, TS::CmdUpdate},
        {{"update", "rename", tree.db(), tree.root(), "pkg/mod.py", file}, TS::CmdUpdate},
        {{"update", "dangling", tree.db(), "pkg/mod.py"}, TS::CmdUpdate},
    };
    for (const auto& [args, cmd] : commands) {
        std::string out, err;
        EXPECT_NE(run(args, cmd, &out, &err), 0) << args[0] << ' ' << args[1];
        EXPECT_NE(err.find(message), std::string::npos) << args[0] << ' ' << args[1] << ": " << err;
        EXPECT_EQ(out, "") << args[0] << ' ' << args[1];
        EXPECT_EQ(dirState(tree.db()), before) << args[0] << ' ' << args[1];
    }
}

}  // anonymous namespace

TEST(CliDbVersion, OtherVersionIsRefusedByEverythingButScan) {
    DanglingTree tree;
    ASSERT_EQ(run({"scan", tree.root(), tree.db()}, TS::CmdScan), 0);
    setUserVersion(tree.db(), 5);
    // Closed cleanly: nothing beside it, and a refusal must not add a -wal / -shm.
    ASSERT_FALSE(fs::exists(tree.db() + "-wal"));
    expectAllRefused(tree);
    EXPECT_FALSE(fs::exists(tree.db() + "-wal"));
    EXPECT_FALSE(fs::exists(tree.db() + "-shm"));

    // scan starts it over, and then everything works again.
    ASSERT_EQ(run({"scan", tree.root(), tree.db()}, TS::CmdScan), 0);
    std::string out;
    ASSERT_EQ(run({"update", "dangling", tree.db(), "pkg/mod.py"}, TS::CmdUpdate, &out), 0);
    EXPECT_EQ(out, "absolute.py\npkg/rel.py\n");
    EXPECT_EQ(run({"algo", tree.db()}, TS::CmdAlgo), 0);
}

TEST(CliDbVersion, FileThatIsNotADatabaseIsRefusedEvenByScan) {
    // A text file, and someone else's SQLite file: neither is ever read or
    // replaced, and every command says what it is.
    for (bool sqlite : {false, true}) {
        DanglingTree tree;
        if (sqlite) {
            sqlite3* db = nullptr;
            ASSERT_EQ(sqlite3_open(tree.db().c_str(), &db), SQLITE_OK);
            ASSERT_EQ(sqlite3_exec(db, "CREATE TABLE important(x); INSERT INTO important VALUES('precious');",
                                   nullptr, nullptr, nullptr), SQLITE_OK);
            sqlite3_close(db);
        } else {
            std::ofstream(tree.db(), std::ios::binary) << "def main():\n    return 0\n";
        }
        const std::string before = fileBytes(tree.db());
        expectAllRefused(tree, tree.db() + " is not a Telescode database");

        std::string err;
        EXPECT_NE(run({"scan", tree.root(), tree.db()}, TS::CmdScan, nullptr, &err), 0);
        EXPECT_NE(err.find(tree.db() + " is not a Telescode database; refusing to overwrite it"),
                  std::string::npos) << err;
        EXPECT_EQ(fileBytes(tree.db()), before);
    }
}

TEST(CliDbVersion, LiveWalOfAnotherVersionIsLeftByteForByte) {
    // A refusal that opened it read-write would checkpoint the WAL into the
    // main file on close and delete the -wal / -shm.
    DanglingTree tree;
    makeLiveWalDb(tree.db());
    ASSERT_GT(fs::file_size(tree.db() + "-wal"), 0u);
    ASSERT_TRUE(fs::exists(tree.db() + "-shm"));
    expectAllRefused(tree);

    // A Telescode database: scan replaces it.
    ASSERT_EQ(run({"scan", tree.root(), tree.db()}, TS::CmdScan), 0);
    std::string out;
    ASSERT_EQ(run({"update", "dangling", tree.db(), "pkg/mod.py"}, TS::CmdUpdate, &out), 0);
    EXPECT_EQ(out, "absolute.py\npkg/rel.py\n");
}

// ── A database with no files ─────────────────────────────────────────────────
// An empty result from it is not an empty project: the reading commands warn,
// and still succeed. `update file` may start a database, so it does not.

TEST(CliEmptyDb, ReadingCommandsWarnThatThereAreNoFiles) {
    DanglingTree tree;
    std::ofstream(tree.db(), std::ios::binary).close();   // 0 bytes
    const std::vector<std::pair<std::vector<std::string>, int (*)(int, char*[])>> commands = {
        {{"graph", tree.db()}, TS::CmdGraph},
        {{"sequence", tree.db()}, TS::CmdSequence},
        {{"analyze", tree.db()}, TS::CmdAnalyze},
        {{"algo", tree.db()}, TS::CmdAlgo},          // gives the file the schema
        {{"analyze", tree.db(), "--algo"}, TS::CmdAnalyze},
    };
    for (const auto& [args, cmd] : commands) {
        std::string err;
        EXPECT_EQ(run(args, cmd, nullptr, &err), 0) << args[0];
        EXPECT_NE(err.find("the database has no files; run `Telescode scan"), std::string::npos)
            << args[0] << ": " << err;
    }

    // Scanned, nothing to warn about.
    ASSERT_EQ(run({"scan", tree.root(), tree.db()}, TS::CmdScan), 0);
    for (const auto& [args, cmd] : commands) {
        std::string err;
        EXPECT_EQ(run(args, cmd, nullptr, &err), 0) << args[0];
        EXPECT_EQ(err.find("has no files"), std::string::npos) << args[0] << ": " << err;
    }
}

TEST(CliEmptyDb, UpdateFileStartsADatabaseWithoutAWarning) {
    DanglingTree tree;
    std::string err;
    ASSERT_EQ(run({"update", "file", tree.db(), tree.root(), tree.root() + "/pkg/mod.py"},
                  TS::CmdUpdate, nullptr, &err), 0);
    EXPECT_EQ(err.find("has no files"), std::string::npos) << err;
}

#ifndef _WIN32

namespace {

// TMPDIR set to `dir` for the life of the object.
class TmpDir {
public:
    explicit TmpDir(const std::string& dir) {
        if (const char* old = std::getenv("TMPDIR")) old_ = old, had_ = true;
        setenv("TMPDIR", dir.c_str(), 1);
    }
    ~TmpDir() {
        if (had_) setenv("TMPDIR", old_.c_str(), 1);
        else unsetenv("TMPDIR");
    }
private:
    std::string old_;
    bool had_ = false;
};

}  // anonymous namespace

TEST(CliDbVersion, WalWithoutShmIsJudgedFromACopy) {
    // SQLite cannot read a WAL without a -shm and not create one, so the check
    // looks at a copy -- in TMPDIR, and gone afterwards.
    DanglingTree tree;
    makeLiveWalDb(tree.db());
    fs::remove(tree.db() + "-shm");
    const fs::path tmp = fs::path(tree.db()).parent_path() / "tmp";
    fs::create_directories(tmp);
    {
        TmpDir env(tmp.string());
        expectAllRefused(tree);
    }
    EXPECT_FALSE(fs::exists(tree.db() + "-shm"));
    EXPECT_TRUE(fs::is_empty(tmp));
}

TEST(CliDbVersion, DatabaseThatCannotBeCheckedIsNotRead) {
    // No TMPDIR to copy into: the check fails, and no command reads the
    // database regardless.
    DanglingTree tree;
    makeLiveWalDb(tree.db());
    fs::remove(tree.db() + "-shm");
    TmpDir env((fs::path(tree.db()).parent_path() / "missing").string());
    expectAllRefused(tree, tree.db() + ": unable to open database file");
}

TEST(CliPaths, PathStartingWithTwoSlashesWorks) {
    // "//tmp/x.db" is the same file as "/tmp/x.db", not a URI authority.
    DanglingTree tree;
    ASSERT_EQ(tree.db()[0], '/');
    const std::string db = "/" + tree.db();
    ASSERT_EQ(run({"scan", tree.root(), db}, TS::CmdScan), 0);
    std::string err;
    EXPECT_EQ(run({"algo", db}, TS::CmdAlgo, nullptr, &err), 0) << err;
    EXPECT_EQ(run({"update", "file", db, tree.root(), tree.root() + "/pkg/mod.py"}, TS::CmdUpdate,
                  nullptr, &err), 0) << err;
    std::string out;
    EXPECT_EQ(run({"graph", db}, TS::CmdGraph, &out, &err), 0) << err;
    EXPECT_NE(out.find("pkg/rel.py"), std::string::npos);
    ASSERT_EQ(run({"scan", tree.root(), db}, TS::CmdScan, nullptr, &err), 0) << err;
}

#endif
