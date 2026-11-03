cmake_minimum_required(VERSION 3.20)
project(Telescode CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

include(FetchContent)

# ── tree-sitter ────────────────────────────────────────────────────────────────
FetchContent_Declare(
    tree-sitter
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter.git
    GIT_TAG        v0.25.3
    SOURCE_SUBDIR  lib
)

FetchContent_Declare(
    tree-sitter-python
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-python.git
    GIT_TAG        v0.23.6
)

# ── Fetch all ─────────────────────────────────────────────────────────────────
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
)

FetchContent_MakeAvailable(tree-sitter googletest)

# tree-sitter-python has no CMakeLists — fetch manually
FetchContent_GetProperties(tree-sitter-python)
if(NOT tree-sitter-python_POPULATED)
    FetchContent_Populate(tree-sitter-python)
endif()

# ── tree-sitter-python static library ────────────────────────────────────────
add_library(tree-sitter-python STATIC
    ${tree-sitter-python_SOURCE_DIR}/src/parser.c
    ${tree-sitter-python_SOURCE_DIR}/src/scanner.c
)
target_include_directories(tree-sitter-python PUBLIC
    ${tree-sitter-python_SOURCE_DIR}/bindings/c
)
target_link_libraries(tree-sitter-python PUBLIC tree-sitter)
set_target_properties(tree-sitter-python PROPERTIES C_STANDARD 11)

# ── SQLite static library (bundled amalgamation) ──────────────────────────────
add_library(sqlite3 STATIC
    ${CMAKE_SOURCE_DIR}/third_party/sqlite/sqlite3.c
)
target_include_directories(sqlite3 PUBLIC
    ${CMAKE_SOURCE_DIR}/third_party/sqlite
)
set_target_properties(sqlite3 PROPERTIES C_STANDARD 11)

# Native inference links the LightGBM C ABI. An existing Python wheel can
# supply the shared library at build time; inference never invokes Python.
file(GLOB TS_LIGHTGBM_HINTS
    "${CMAKE_SOURCE_DIR}/bench/.venv/lib/python*/site-packages/lightgbm/lib"
    "${CMAKE_SOURCE_DIR}/.venv/lib/python*/site-packages/lightgbm/lib")
find_library(TS_LIGHTGBM_LIBRARY NAMES _lightgbm lightgbm lib_lightgbm
    HINTS ${TS_LIGHTGBM_HINTS}
          "${CMAKE_SOURCE_DIR}/bench/.venv/Lib/site-packages/lightgbm/lib")
if(NOT TS_LIGHTGBM_LIBRARY)
    message(FATAL_ERROR "LightGBM C library required: set -DTS_LIGHTGBM_LIBRARY=/path/to/lib_lightgbm.so")
endif()
add_library(LightGBM::C UNKNOWN IMPORTED)
set_target_properties(LightGBM::C PROPERTIES IMPORTED_LOCATION "${TS_LIGHTGBM_LIBRARY}")
add_library(ranking STATIC src/ranking/LambdaRankModel.cpp)
target_include_directories(ranking PUBLIC src)
target_link_libraries(ranking PUBLIC sqlite3 LightGBM::C Threads::Threads)
if(MSVC)
    target_compile_options(ranking PRIVATE /utf-8)
endif()

# ── Telescode core (headless) ─────────────────────────────────────────────────
# Parser, database, analysis and serialization. Nothing in this target or
# anything it links needs a graphical environment, so any frontend can sit on
# top of it without touching an algorithm.
#
# db/db.cpp deliberately stays in `algo` rather than being listed here -- it is
# already part of that target, and compiling it into both would collide at link
# time.
add_library(telescode_core STATIC
    src/core/AnalysisService.cpp
    src/core/FileMetricsQuery.cpp
    src/core/ReadingSequenceQuery.cpp
    src/core/diagram/cd_builder.cpp
    src/core/diagram/cd_layout.cpp
    src/core/json/AnalysisJson.cpp
    src/core/json/JsonWriter.cpp
    src/db/DbInserter.cpp
    src/db/DbUpdater.cpp
    src/parser/PythonParser.cpp
    src/parser/ParserRegistry.cpp
)
target_include_directories(telescode_core PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${tree-sitter_SOURCE_DIR}/lib/include
)
target_link_libraries(telescode_core PUBLIC
    algo
    resolve
    tree-sitter
    tree-sitter-python
    sqlite3
    Threads::Threads
)
if(MSVC)
    target_compile_options(telescode_core PRIVATE /utf-8)
endif()

# ── Telescode CLI (headless) ──────────────────────────────────────────────────
# The subcommands and their dispatch table, kept apart from the entry point so
# the tests can drive them directly. Depends on telescode_core only.
add_library(telescode_cli STATIC
    src/cli/ts_cli.cpp
    src/cli/scan_main.cpp
    src/cli/update_main.cpp
    src/cli/algo_main.cpp
    src/cli/json_cmds.cpp
    src/cli/rank_main.cpp
)
target_link_libraries(telescode_cli PUBLIC telescode_core ranking)
if(MSVC)
    target_compile_options(telescode_cli PRIVATE /utf-8)
endif()

# ── TelescodeHeadless executable ──────────────────────────────────────────────
# The command-line entry point to the core.
add_executable(TelescodeHeadless src/cli/headless_main.cpp)
target_link_libraries(TelescodeHeadless PRIVATE telescode_cli)
if(MSVC)
    target_compile_options(TelescodeHeadless PRIVATE /utf-8)
endif()

# ── lang static library ────────────────────────────────────────────────────────
# Language plugins (ILanguage): module resolution and builtin scopes, one
# implementation per source language. Pure C++, no database or parser.
add_library(lang STATIC
    src/lang/LanguageRegistry.cpp
    src/lang/python/PythonLanguage.cpp
)
target_include_directories(lang PUBLIC src)

# ── resolve static library ─────────────────────────────────────────────────────
# The language-neutral name resolver: reads symbol/scope/binding/ref from a
# database, writes import_resolution and each ref's answer, from which the
# `link` view derives every link. Language rules come only through `lang`.
add_library(resolve STATIC
    src/resolve/Resolver.cpp
)
target_include_directories(resolve PUBLIC src)
target_link_libraries(resolve PUBLIC lang sqlite3)

# ── algo static library ────────────────────────────────────────────────────────
add_library(algo STATIC
    src/algo/Graph.cpp
    src/algo/Scoring.cpp
    src/algo/ReadingSequencer.cpp
    src/algo/AlgoRunner.cpp
    src/algo/AlgoDbWriter.cpp
    src/algo/SeedIo.cpp
    src/db/db.cpp
)
target_include_directories(algo PUBLIC src)
target_link_libraries(algo PUBLIC sqlite3)


# ── TelescodePPR executable ───────────────────────────────────────────────────
# Personalized PageRank for one instance DB; consumed by the bench harness.
add_executable(TelescodePPR
    src/ppr_main.cpp
)
target_include_directories(TelescodePPR PRIVATE src)
target_link_libraries(TelescodePPR PRIVATE algo sqlite3)

# ── Unit tests ────────────────────────────────────────────────────────────────
enable_testing()

# tests/** is gitignored, so a checkout may carry only some of the test sources.
# A missing file in add_executable() is a configure-time error that takes every
# target down with it, so drop the absent files here and
# skip a target once it has no test source left to run.
function(ts_test_sources out_var)
    set(found "")
    set(has_test FALSE)
    foreach(src IN LISTS ARGN)
        if(EXISTS "${CMAKE_SOURCE_DIR}/${src}")
            list(APPEND found "${src}")
            if(src MATCHES "^tests/")
                set(has_test TRUE)
            endif()
        elseif(src MATCHES "^tests/")
            message(STATUS "test source absent, skipping: ${src}")
        else()
            message(WARNING "missing source file: ${src}")
        endif()
    endforeach()
    # A test target with no test file left would build and pass vacuously.
    if(NOT has_test)
        set(found "")
    endif()
    set(${out_var} "${found}" PARENT_SCOPE)
endfunction()

ts_test_sources(TS_CORE_TEST_SRC
    tests/core/test_json_writer.cpp
    tests/core/test_analysis_readonly.cpp
    tests/core/test_cd_builder.cpp
)
if(TS_CORE_TEST_SRC)
    add_executable(CoreTests ${TS_CORE_TEST_SRC})
    target_link_libraries(CoreTests PRIVATE telescode_core GTest::gtest_main)
    if(MSVC)
        target_compile_options(CoreTests PRIVATE /utf-8)
    endif()
    add_test(NAME CoreTests COMMAND CoreTests)
endif()

ts_test_sources(TS_CLI_TEST_SRC
    tests/cli/test_cli_options.cpp
)
if(TS_CLI_TEST_SRC)
    add_executable(CliTests ${TS_CLI_TEST_SRC})
    target_link_libraries(CliTests PRIVATE telescode_cli GTest::gtest_main)
    if(MSVC)
        target_compile_options(CliTests PRIVATE /utf-8)
    endif()
    add_test(NAME CliTests COMMAND CliTests)
endif()

ts_test_sources(TS_ALGO_TEST_SRC
    tests/algo/test_reading_sequencer.cpp
    tests/algo/test_pagerank.cpp
    tests/algo/test_score_combiner.cpp
    tests/algo/test_betweenness_centrality.cpp
    tests/algo/test_algo_merge.cpp
    tests/algo/test_percentile_rank.cpp
    tests/algo/test_complexity_scorer.cpp
    tests/algo/test_graph_weights.cpp
    tests/algo/test_personalized_pagerank.cpp
    tests/algo/test_seed_io.cpp
    tests/algo/test_module_resolution.cpp
    tests/algo/test_isolated_entities.cpp
)
if(TS_ALGO_TEST_SRC)
    add_executable(AlgoTests ${TS_ALGO_TEST_SRC})
    target_include_directories(AlgoTests PRIVATE src)
    target_link_libraries(AlgoTests PRIVATE algo GTest::gtest_main)
    add_test(NAME AlgoTests COMMAND AlgoTests)
endif()

ts_test_sources(TS_LANG_TEST_SRC
    tests/lang/test_python_language.cpp
)
if(TS_LANG_TEST_SRC)
    add_executable(LangTests ${TS_LANG_TEST_SRC})
    target_link_libraries(LangTests PRIVATE lang GTest::gtest_main)
    add_test(NAME LangTests COMMAND LangTests)
endif()

ts_test_sources(TS_PARSER_TEST_SRC
    tests/parser/test_python_extraction.cpp
)
if(TS_PARSER_TEST_SRC)
    add_executable(ParserTests ${TS_PARSER_TEST_SRC})
    target_link_libraries(ParserTests PRIVATE telescode_core GTest::gtest_main)
    # Scanned as-is by the tests; also the resolver's fixture.
    target_compile_definitions(ParserTests PRIVATE
        TS_RESOLUTION_FIXTURE="${CMAKE_SOURCE_DIR}/tests/fixtures/resolution")
    if(MSVC)
        target_compile_options(ParserTests PRIVATE /utf-8)
    endif()
    add_test(NAME ParserTests COMMAND ParserTests)
endif()

ts_test_sources(TS_RESOLVE_TEST_SRC
    tests/resolve/test_resolver.cpp
    tests/resolve/test_incremental.cpp
)
if(TS_RESOLVE_TEST_SRC)
    add_executable(ResolverTests ${TS_RESOLVE_TEST_SRC})
    target_link_libraries(ResolverTests PRIVATE telescode_core GTest::gtest_main)
    target_compile_definitions(ResolverTests PRIVATE
        TS_RESOLUTION_FIXTURE="${CMAKE_SOURCE_DIR}/tests/fixtures/resolution")
    if(MSVC)
        target_compile_options(ResolverTests PRIVATE /utf-8)
    endif()
    add_test(NAME ResolverTests COMMAND ResolverTests)
endif()

ts_test_sources(TS_RANKING_TEST_SRC
    tests/ranking/test_lambdarank.cpp
)
if(TS_RANKING_TEST_SRC)
    add_executable(RankingTests ${TS_RANKING_TEST_SRC})
    target_link_libraries(RankingTests PRIVATE telescode_core ranking GTest::gtest_main)
    target_compile_definitions(RankingTests PRIVATE
        TS_LTR_ARTIFACT="${CMAKE_SOURCE_DIR}/ltr/models/lambdarank-v1"
        TS_LTR_FIXTURE="${CMAKE_SOURCE_DIR}/tests/fixtures/ltr")
    if(MSVC)
        target_compile_options(RankingTests PRIVATE /utf-8)
    endif()
    add_test(NAME RankingTests COMMAND RankingTests)
endif()

ts_test_sources(TS_LAYOUT_TEST_SRC
    tests/ui/test_cd_layout.cpp
    src/core/diagram/cd_layout.cpp
)
if(TS_LAYOUT_TEST_SRC)
    add_executable(LayoutTests ${TS_LAYOUT_TEST_SRC})
    target_include_directories(LayoutTests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(LayoutTests PRIVATE GTest::gtest_main)
    add_test(NAME LayoutTests COMMAND LayoutTests)
endif()
