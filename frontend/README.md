# Telescode frontend

React + TypeScript (Vite) UI, hosted in a Tauri v2 desktop shell. All analysis
comes from the C++ core, which runs as the `TelescodeHeadless` sidecar.

```text
React component
   │  telescode.analyze(dbPath)                 src/bridge/api.ts
   ▼
Tauri IPC: invoke('run_headless', { op, dbPath })
   ▼
Rust command run_headless                        src-tauri/src/bridge.rs
   │  validates the path, spawns the sidecar, returns its stdout
   ▼
TelescodeHeadless analyze <absolute db path>     C++ core
   ▼
JSON on stdout → parsed and shape-checked in TS → typed AnalysisSnapshot
```

## Prerequisites

- CMake 3.20+ and a C++17 compiler (Visual Studio 2022 on Windows)
- Node.js 20+
- Rust (stable) via [rustup](https://rustup.rs)
- The Tauri system prerequisites for your OS: <https://v2.tauri.app/start/prerequisites/>
  (on Windows 10/11, WebView2 is usually already installed)

## Running

Run all commands from the repository root unless noted.

### 1. Build the C++ core

```bash
cmake -S . -B build
cmake --build build --config Release --target TelescodeHeadless
```

You need a database to open. Create one with the same binary:

```bash
build/Release/TelescodeHeadless scan <path/to/python/repo> telescode.db
build/Release/TelescodeHeadless algo telescode.db
```

(Single-config generators such as Ninja or Makefiles put the binary at
`build/TelescodeHeadless` instead of `build/Release/`.)

### 2. Frontend only (browser)

```bash
cd frontend
npm install
npm run dev          # http://localhost:5173
```

The UI renders, but it cannot reach the core outside the desktop shell.
Requests fail with `bridge_unavailable`.

### Opening a codebase

The app opens on a repository URL field. Paste a public GitHub repository
URL (`https://github.com/owner/repo`) and press **Analyze**:

```text
URL ──invoke('open_repository')──▶ Rust (src-tauri/src/repository.rs)
        │  GitHub API: check the repository, then download the default
        │  branch as a tarball into <app cache>/repositories/<owner>/<repo>/source
        │  TelescodeHeadless scan source <repo>.db, then algo <repo>.db
        ▼
database path ──telescode.analyze──▶ the same path as any database
```

Users never handle the database. Opening the same repository again downloads
its current default branch and scans it afresh. Only public repositories are
supported: no token is sent, and GitHub answers a private repository the same
way as a missing one. URLs that point inside a repository (branches, files,
pull requests) are refused. Downloads are capped at 500 MB compressed and
2 GB unpacked; symbolic links in the archive are skipped.

Development builds (`npm run dev`, `npm run desktop`) also show an
**Open a local database (dev)** link under the field, which opens a database
made with `TelescodeHeadless scan` directly. Release builds do not show it.

To look at the workspace without the core, add one of these to a dev URL.
They are development-only and not included in builds.

- `?preview=empty|loading|error|ready`. `ready` shows the code map for the
  Sherlock sample repository, from real core output saved in
  `src/graph/fixtures/sherlock.graph.json`.
- `?snapshot=<url>` shows the map for any JSON printed by
  `TelescodeHeadless graph` or `analyze`, fetched from the dev server. For a
  quick look at a large repository, save it under `node_modules/.cache/`
  (ignored by git) and open `?snapshot=/node_modules/.cache/<file>.json`.
- `&bench` (with either of the above) zooms from the whole repository into
  its busiest file and back, then pans, and prints draw-time statistics to
  the console and `document.body.dataset.bench`.

### 3. Desktop app

```bash
cd frontend
npm install
npm run desktop      # stages the sidecar, then runs `tauri dev`
```

`npm run desktop` first runs `npm run sidecar`. That copies
`build/<config>/TelescodeHeadless` to
`src-tauri/binaries/TelescodeHeadless-<target-triple>[.exe]`, the name Tauri's
`externalBin` requires. If your C++ build lives somewhere else, set
`TELESCODE_BUILD_DIR`:

```bash
TELESCODE_BUILD_DIR=/path/to/build npm run desktop
```

Re-run it after rebuilding the C++ core so the app picks up the new binary.

`npm run desktop:build` produces a release bundle under
`src-tauri/target/release/bundle/` with the sidecar included.

## Checks

```bash
cd frontend
npm run typecheck    # tsc -b
npm test             # bridge, UI and code-map tests (vitest; UI tests in jsdom)
npm run build        # typecheck + production bundle
npm run lint

cd src-tauri
cargo test           # needs the sidecar staged (npm run sidecar)
```

`cargo test` also exercises the real sidecar when both of these are set:

```bash
TELESCODE_HEADLESS=<build>/Release/TelescodeHeadless.exe \
TELESCODE_TEST_DB=<scanned database> cargo test
```

and downloads and scans a real repository when these are set:

```bash
TELESCODE_HEADLESS=<build>/Release/TelescodeHeadless.exe \
TELESCODE_TEST_REPOSITORY=https://github.com/sherlock-project/sherlock cargo test
```

## The bridge

`src/bridge` is the only way the UI reaches the core:

```ts
import { telescode } from './bridge'

const snapshot = await telescode.analyze(dbPath)   // AnalysisSnapshot
const graph    = await telescode.graph(dbPath)     // GraphResponse
const sequence = await telescode.sequence(dbPath)  // SequenceResponse
```

- **Coarse-grained.** Each call is one complete operation and one sidecar run.
  Fetch at load or refresh time and keep the result in state. Never call the
  bridge while rendering.
- **Read-only.** The bridge never passes `--algo`, so it cannot create or modify
  a database.
- **No logic in the frontend.** `src/bridge/models.ts` mirrors
  `src/core/json/AnalysisJson.cpp` (schemaVersion 1). Parsing, graph metrics,
  complexity and reading-sequence scores are all computed by the C++ core.
- **Sidecar location.** At runtime the Rust side looks for
  `TelescodeHeadless[.exe]` next to the app executable, where Tauri places
  `externalBin`. Set `TELESCODE_HEADLESS` to override the path.

### Errors

Every failure rejects with a `TelescodeError` whose `code` is one of:

| code | raised by | meaning |
|---|---|---|
| `invalid_argument` | Rust | empty or unresolvable path |
| `db_not_found` | Rust | no file at that path (nothing is created) |
| `db_not_a_file` | Rust | the path is a directory or similar |
| `db_invalid` | Rust | the file is not a SQLite database |
| `sidecar_missing` | Rust | TelescodeHeadless was not found |
| `sidecar_spawn_failed` | Rust | the sidecar exists but could not be started |
| `sidecar_failed` | Rust | non-zero exit; `details.exitCode` and `details.stderr` are set |
| `output_not_utf8` | Rust | stdout was not UTF-8 |
| `invalid_repository_url` | Rust | not a `https://github.com/owner/repo` URL |
| `repository_not_found` | Rust | the repository does not exist or is private |
| `repository_unreachable` | Rust | GitHub could not be reached, or is rate-limiting |
| `repository_too_large` | Rust | over the size limits above |
| `repository_empty` | Rust | the scan found no supported source files |
| `repository_fetch_failed` | Rust | the download or unpacking failed |
| `internal` | Rust | bridge worker failure |
| `malformed_json` | TS | stdout was not valid JSON |
| `unexpected_schema` | TS | valid JSON, wrong shape |
| `unsupported_schema_version` | TS | `schemaVersion` is not 1 |
| `bridge_unavailable` | TS | not running inside the desktop app |
| `unknown` | TS | anything unrecognised |

## UI structure

```text
src/
  app/          AppShell (top bar over the canvas) and useWorkspace (load state)
  components/   Shell pieces: TopBar, WorkspaceCanvas, CanvasControls, CodeMap
  graph/        The code map: model, layout, semantic zoom, camera, renderer
  ui/           Small reusable primitives: Breadcrumbs, SearchField, Dropdown,
                IconButton, Tooltip, Button, Spinner, EmptyState, ErrorState
  styles/       tokens.css (design tokens) and globals.css
```

- **Tokens only.** Components take colours, type, spacing and radii from
  `src/styles/tokens.css`. Primitives (`--gray-*` etc.) feed semantic tokens
  (`--bg-canvas`, `--text-muted`, ...), and components use only the semantic
  ones, so a future theme overrides that one layer.
- **Monochrome chrome.** Hue is reserved for what the graph will encode
  (complexity, selection, reading order). Status colours are for UI messages.
- **Pretendard** is bundled from the `pretendard` package (variable, dynamic
  subset), since the app's CSP only allows local assets.

## The code map

Agent-driven approved tours and silent WebM/VTT rendering are available through
the local MCP package. See [mcp/README.md](../mcp/README.md) for setup and the
review/approval workflow. The renderer reuses this code map and reserves a
right-hand column for analysis evidence. Captions are external WebVTT only.

The ready state is one zoomable map of the repository. Zooming changes what is
shown, not just its size: directories are regions, files are cards, classes
list their members, and each level resolves in place as you move closer.

```text
snapshot ──buildGraphModel──▶ tree ──layoutGraph──▶ boxes ──GraphRenderer──▶ <canvas>
(bridge, once)  (model.ts)          (layout.ts)            (renderer.ts, per frame)
```

- **Model** (`model.ts`). The tree comes straight from the core's output:
  directories from each file's repo-relative `fileId`, classes from their
  `fileId`, members from each class's fields and methods. Single-child
  directory chains merge (`src/core`). Edges are the core's file and class
  edges. Nothing is recomputed. The schema has no module-level function list
  or call edges, so the symbol level is classes and their members.
- **Layout** (`layout.ts`). One static, deterministic layout for every zoom
  level: a nested squarified treemap (area grows with the square root of a
  file's line count), with class members as rows, in columns for large
  classes. Children always sit inside their parent's box, so zooming never
  relays anything out, and the same repository always gives the same map.
- **Semantic zoom** (`semantic.ts`). There are no global levels. Each
  container has an *openness* between 0 and 1, a smooth function of how
  legible its children would be on screen (and, for directories, whether
  their header can carry a label). While it opens, its label first settles
  into a header band, then its children fade in. A node's visibility is its
  ancestors' openness multiplied, so zooming out folds detail back the same
  way. Thresholds are in `OPEN_RANGE` and are tuned by eye.
- **Edges.** Each edge end is a blend of its file and that file's ancestors,
  weighted by which one is currently standing in for it. Edges are grouped by
  the pair of stand-ins, so zoomed out, many imports read as one line between
  two regions, and the line splits and glides to the files as the regions
  open. Only edges with an end on screen are drawn, faint ones fade, and at
  most `EDGE_BUDGET` groups are shown at once, weakest fading first. Class
  relationships appear only at the symbol level.
- **Camera** (`camera.ts`, `renderer.ts`). Scrolling, with a mouse wheel
  or two fingers, zooms around the pointer (smoothed over about 55 ms),
  trackpad pinch zooms directly, and a sideways scroll and drag pan. Fit, zoom buttons, breadcrumbs and
  double-click move the camera along a van Wijk–Nuij path, which pulls back
  just enough on long moves to keep context. With `prefers-reduced-motion`,
  camera moves are instant.
- **Interaction.** Hover outlines a node, emphasises its edges and dims
  unrelated files and classes. Click selects (the one accent colour), and
  double-click or Enter focuses. Escape clears the selection. `+`, `-` and
  `0` zoom and fit, and the arrow keys pan.
- **Breadcrumbs** show the selection, else the node last navigated to, else
  the deepest open node under the view centre. Clicking an ancestor flies
  back to it.
- **Rendering.** React mounts the canvas and hears only about context and
  selection changes, so camera movement never re-renders React. Frames are
  drawn on demand, not in an idle loop, and only visible nodes are visited.
  The bridge is called once per load, never during interaction. Colours come
  from the `--graph-*` tokens.

On the Python standard library (1,848 files, 72k nodes, 63k edges), headless
Chrome with software rendering draws a zoom-and-pan run in about 7 ms per
frame on average, 13 ms at the 95th percentile (`&bench`). Building and
laying out the map takes about 250 ms once per load.
