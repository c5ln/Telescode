// tests/cli/test_cli_options.cpp
// ParseJsonCmdOptions and the --out safety check.
//
// The JSON commands close the database before opening the output file, so an
// --out path that names the database is not a locking conflict -- it is a
// truncation. A 233KB database became 30KB of JSON while the command reported
// success. The parser refuses that combination instead.

#include "cli/ts_cli.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

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
