cmake_minimum_required(VERSION 3.20)
project(Telescode CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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

# ── SDL3 ──────────────────────────────────────────────────────────────────────
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON  CACHE BOOL "" FORCE)
FetchContent_Declare(
    SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG        release-3.2.10
)

# ── Dear ImGui ────────────────────────────────────────────────────────────────
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.9
)

# ── imnodes ───────────────────────────────────────────────────────────────────
FetchContent_Declare(
    imnodes
    GIT_REPOSITORY https://github.com/Nelarius/imnodes.git
    GIT_TAG        v0.5
)

# ── Fetch all ─────────────────────────────────────────────────────────────────
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
)

FetchContent_MakeAvailable(tree-sitter SDL3 imgui imnodes googletest)

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

# ── ImGui static library ──────────────────────────────────────────────────────
add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlgpu3.cpp
)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui PUBLIC SDL3::SDL3-static)

# ── imnodes static library ────────────────────────────────────────────────────
add_library(imnodes STATIC
    ${imnodes_SOURCE_DIR}/imnodes.cpp
)
target_include_directories(imnodes PUBLIC ${imnodes_SOURCE_DIR})
target_compile_definitions(imnodes PRIVATE IMGUI_DEFINE_MATH_OPERATORS)
target_link_libraries(imnodes PUBLIC imgui)

# ── SQLite static library (bundled amalgamation) ──────────────────────────────
add_library(sqlite3 STATIC
    ${CMAKE_SOURCE_DIR}/third_party/sqlite/sqlite3.c
)
target_include_directories(sqlite3 PUBLIC
    ${CMAKE_SOURCE_DIR}/third_party/sqlite
)
set_target_properties(sqlite3 PROPERTIES C_STANDARD 11)

# ── Telescode executable ──────────────────────────────────────────────────────
add_executable(Telescode
    src/main.cpp
    src/ui/ts_app.cpp
    src/ui/ts_canvas.cpp
    src/ui/class_diagram/cd_view.cpp
    src/ui/class_diagram/cd_builder.cpp
    src/db/db.cpp
)
target_include_directories(Telescode PRIVATE
    ${tree-sitter_SOURCE_DIR}/lib/include
    ${CMAKE_SOURCE_DIR}/src
)

# Copy font assets next to the executable so LoadFonts() can find them.
add_custom_command(TARGET Telescode POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:Telescode>/assets/fonts"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/assets/fonts/Inter-Regular.ttf"
        "${CMAKE_SOURCE_DIR}/assets/fonts/Inter-Medium.ttf"
        "${CMAKE_SOURCE_DIR}/assets/fonts/JetBrainsMono-Regular.ttf"
        "$<TARGET_FILE_DIR:Telescode>/assets/fonts/"
    COMMENT "Copying fonts to build output"
)
target_link_libraries(Telescode PRIVATE
    tree-sitter
    tree-sitter-python
    SDL3::SDL3-static
    imgui
    imnodes
    sqlite3
)
target_compile_definitions(Telescode PRIVATE IMGUI_DEFINE_MATH_OPERATORS)

# MSVC requires /utf-8 to parse UTF-8 source files correctly on non-UTF-8 locales.
if(MSVC)
    target_compile_options(Telescode PRIVATE /utf-8)
endif()

# Enable the Warm Cream token preview panel (dev only -- off by default).
# cmake -DTELESCODE_STYLE_PREVIEW=ON ..
option(TELESCODE_STYLE_PREVIEW "Show Warm Cream token preview panel at startup" OFF)
if(TELESCODE_STYLE_PREVIEW)
    target_compile_definitions(Telescode PRIVATE TELESCODE_STYLE_PREVIEW)
endif()

# ── TelescodeScanner executable ───────────────────────────────────────────────
add_executable(TelescodeScanner
    src/scan_main.cpp
    src/db/DbInserter.cpp
    src/db/db.cpp
    src/parser/PythonParser.cpp
    src/parser/ParserRegistry.cpp
)
target_include_directories(TelescodeScanner PRIVATE
    ${tree-sitter_SOURCE_DIR}/lib/include
    src
)
target_link_libraries(TelescodeScanner PRIVATE
    tree-sitter
    tree-sitter-python
    sqlite3
)

# ── TelescodeUpdater executable ───────────────────────────────────────────────
add_executable(TelescodeUpdater
    src/update_main.cpp
    src/db/DbUpdater.cpp
    src/db/DbInserter.cpp
    src/db/db.cpp
    src/parser/PythonParser.cpp
    src/parser/ParserRegistry.cpp
)
target_include_directories(TelescodeUpdater PRIVATE
    ${tree-sitter_SOURCE_DIR}/lib/include
    src
)
target_link_libraries(TelescodeUpdater PRIVATE
    tree-sitter
    tree-sitter-python
    sqlite3
)

# ── algo static library ────────────────────────────────────────────────────────
add_library(algo STATIC
    src/algo/Graph.cpp
    src/algo/GraphBuilder.cpp
    src/algo/Scoring.cpp
    src/algo/BetweennessCentrality.cpp
    src/algo/ReadingSequencer.cpp
    src/algo/AlgoRunner.cpp
    src/algo/AlgoDbWriter.cpp
    src/db/db.cpp
)
target_include_directories(algo PUBLIC src)
target_link_libraries(algo PUBLIC sqlite3)

# ── TelescodeAlgo executable ──────────────────────────────────────────────────
add_executable(TelescodeAlgo
    src/algo_main.cpp
)
target_include_directories(TelescodeAlgo PRIVATE src)
target_link_libraries(TelescodeAlgo PRIVATE algo sqlite3)

# ── Unit tests ────────────────────────────────────────────────────────────────
enable_testing()

add_executable(AlgoTests
    tests/algo/test_reading_sequencer.cpp
    tests/algo/test_pagerank.cpp
    tests/algo/test_score_combiner.cpp
    tests/algo/test_betweenness_centrality.cpp
    tests/algo/test_algo_merge.cpp
)
target_include_directories(AlgoTests PRIVATE src)
target_link_libraries(AlgoTests PRIVATE algo GTest::gtest_main)
add_test(NAME AlgoTests COMMAND AlgoTests)

add_executable(CanvasTests
    tests/ui/test_ts_canvas.cpp
    src/ui/ts_canvas.cpp
    src/ui/class_diagram/cd_view.cpp
    src/ui/class_diagram/cd_builder.cpp
)
target_include_directories(CanvasTests PRIVATE
    ${imgui_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(CanvasTests PRIVATE
    imgui
    imnodes
    sqlite3
    GTest::gtest_main
)
add_test(NAME CanvasTests COMMAND CanvasTests)

add_executable(DiagramTests
    tests/ui/test_cd_arrowhead.cpp
    src/ui/class_diagram/cd_arrowhead.cpp
)
target_include_directories(DiagramTests PRIVATE
    ${imgui_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
)
target_compile_definitions(DiagramTests PRIVATE IMGUI_DEFINE_MATH_OPERATORS)
target_link_libraries(DiagramTests PRIVATE
    imgui
    GTest::gtest_main
)
add_test(NAME DiagramTests COMMAND DiagramTests)
