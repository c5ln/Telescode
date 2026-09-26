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
npm test             # bridge unit tests (vitest)
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
| `internal` | Rust | bridge worker failure |
| `malformed_json` | TS | stdout was not valid JSON |
| `unexpected_schema` | TS | valid JSON, wrong shape |
| `unsupported_schema_version` | TS | `schemaVersion` is not 1 |
| `bridge_unavailable` | TS | not running inside the desktop app |
| `unknown` | TS | anything unrecognised |

## Rendering large views

React owns application UI and state. The upcoming code-map and graph viewports
can reach thousands of nodes, so they should render into a single
`<canvas>` (2D or WebGL) owned by one component rather than one DOM element
per node. The bridge already returns whole snapshots, so a viewport can load
once and redraw without further IPC.
