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
FetchContent_MakeAvailable(tree-sitter SDL3 imgui imnodes)

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

# ── Telescode executable ──────────────────────────────────────────────────────
add_executable(Telescode src/main.cpp)
target_include_directories(Telescode PRIVATE
    ${tree-sitter_SOURCE_DIR}/lib/include
)
target_link_libraries(Telescode PRIVATE
    tree-sitter
    tree-sitter-python
    SDL3::SDL3-static
    imgui
    imnodes
)
