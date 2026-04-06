# Telescode

> A telescope for your codebase.

## Problem

AI generates code faster than humans can comprehend it. Developers ship features rapidly using AI tools, but often lose sight of how their own codebase is structured.

**Pain points:** Redundant code, misusing functions, fear of refactoring, slow debugging, over-reliance on AI tools

**What Telescode delivers:** Faster onboarding, faster debugging, better architectural decisions, improved maintainability, more efficient use of AI tools

## What is Telescode?

Google Earth for your codebase. A zoomable interface that lets you seamlessly navigate from high-level dependency graphs down to individual lines of code.

## Key Features

### 1. Zoomable Code Map

Adjust the abstraction level freely with the scroll wheel — continuously zoom between dependency graphs, class diagrams, and source code. Includes node rendering, connection rendering, and node layout algorithms, with interactions such as zoom in/out, panning, and a sidebar.

### 2. Code Complexity Estimation

Analyzes complexity along two axes.

**File-Level Complexity Matrix:**

- **Logical Density Analysis** — Measures the ratio of actual logic (conditionals, loops, etc.) to total lines in a file. Higher density indicates concentrated complex logic.
- **Cyclomatic Complexity** — Counts the number of independent execution paths through the code. More branch points (if, for, while, switch) mean higher values, indicating greater effort needed to test and understand.
- **Nesting Depth** — Measures how deeply conditionals and loops are nested. Deeper nesting increases cognitive load.
- **Resource Weight** — Measures the volume of external resources (modules, libraries) a file imports or depends on. More dependencies mean more background knowledge is required to understand the file's context.
- **Lines of Code** — Total line count of a file. Not an accurate complexity indicator on its own, but serves as foundational data when combined with other metrics.

**File Relationship Analysis:** Dependency mapping with In-Bound and Out-Bound link counting.

### 3. Reading Sequence Recommendation

Uses PageRank and Betweenness Centrality to determine module importance and recommend an optimal reading order.

- **PageRank** — Assigns higher scores to modules that are more frequently referenced (imported/called) by other modules, identifying core modules in the codebase.
- **Betweenness Centrality** — This metric measures how many shortest paths between all pairs of nodes pass through a given node. Intuitively, it answers the question: "Can other modules be reached without going through this one?" This is similar to a "hub" person in a company who must be involved in every new client acquisition. The time complexity is O(VE) for V vertices and E edges, so vertex sampling is used for approximation on large codebases.

These two metrics are combined to recommend the reading order that allows developers to grasp the codebase most efficiently — answering "which module should I read first?"

### 4. Code Parsing

Performs multi-layer parsing based on Tree-sitter.

- **File Layer** — Parses and stores file names and dependency information.
- **Class Layer** — Parses and stores class names, method names, and internal class structure.
- **Function Layer** — Parses and stores function names, input/output types, and inter-function dependencies.
- **Code Layer** — Stores function-level code with positional information.

## Future Directions

- **AI-powered Code Explanation** — Generate natural language descriptions of code via Claude API integration.

## Workflow

1. User inputs a GitHub repository URL into Telescode.
2. Telescode parses the code, estimates complexity, and recommends a reading sequence.
3. User explores the codebase through the zoomable interface.

## Tech Stack

| Area | Technology |
|------|------------|
| Language | C++17 |
| Build | CMake 3.20+ (FetchContent) |
| Code Parsing | Tree-sitter v0.25.3, tree-sitter-python v0.23.6 |
| Rendering | SDL3 (release-3.2.10, static) |
| UI | Dear ImGui v1.91.9, imnodes v0.5 |
| Data Storage | SQLite3 (bundled amalgamation, WAL mode) |

## Build

```bash
git clone https://github.com/<your-repo>/Telescode.git
cd Telescode
mkdir build && cd build
cmake ..
cmake --build .
```

All external dependencies are automatically downloaded via CMake FetchContent — no manual installation required. SQLite3 amalgamation source is included in `third_party/sqlite/`.


---

## Branching Convention

This project follows [GitHub Flow](https://docs.github.com/en/get-started/using-github/github-flow).

Branch format: `<type>/<issue-id>-<description>` (e.g., `feat/26-add-zoom`)

| Type        | Purpose               |
| ----------- | --------------------- |
| `feat/`     | New features          |
| `fix/`      | Bug fixes             |
| `refactor/` | Code refactoring      |
| `docs/`     | Documentation changes |
| `chore/`    | Miscellaneous tasks   |

All PRs require a code review before merging.
