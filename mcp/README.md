# Telescode agent tours

Local stdio MCP tools let Codex inspect an existing project analysis, propose a
tour, observe draft frames, and render a user-approved tour. Output is silent VP8
WebM at 1920×1080 / 30fps, with a separate UTF-8 WebVTT file and an evidence panel.
Tours have at most 8 stops and 120 seconds including movement. Files and classes
are supported; source-line and member stops are not.

## Setup (Linux / Windows)

Prerequisites: Node 22+, FFmpeg with `libvpx` on PATH, and the Chromium system
libraries required by Playwright. No OpenAI API key is needed: Codex provides
the agent; the MCP server never calls an LLM itself.

From the repository root:

```sh
npm --prefix frontend install
npm --prefix frontend run build
npm --prefix mcp install
npm --prefix mcp run install:browser
```

Browser installation uses `mcp/node_modules/.cache/playwright` on either OS.
`PLAYWRIGHT_BROWSERS_PATH` can override it. On Linux, missing Chromium libraries
must be installed through the OS package manager / Playwright system dependency
installer. `TELESCODE_FFMPEG` can point to `ffmpeg.exe` or another FFmpeg binary.

## Connect Codex

Add the following to your Codex MCP configuration, replacing paths:

```toml
[mcp_servers.telescode]
command = "node"
args = ["--import", "tsx", "src/server.ts"]
cwd = "/absolute/path/to/Telescode/mcp"
```

Windows example:

```toml
[mcp_servers.telescode]
command = "node"
args = ["--import", "tsx", "src/server.ts"]
cwd = "C:/projects/Telescode/mcp"
```

Optional environment settings:

```toml
[mcp_servers.telescode.env]
TELESCODE_HEADLESS = "/absolute/path/to/build/TelescodeHeadless"
TELESCODE_TOUR_OUTPUT = "/absolute/path/to/tours"
```

An existing `analyze` JSON or `graph` JSON also works without the C++ executable.
Graph-only snapshots have no reading ranks; missing values are omitted.
The built frontend is served by the MCP process, so Vite and Tauri do not need to
run. Rebuild `frontend` after modifying tour UI code.

## Agent workflow

1. `open_project(path)` opens a DB or snapshot; it does not clone or scan.
2. `get_project_overview`, `search_nodes`, `inspect_node` provide existing data
   and valid node IDs. Use these to support factual captions.
3. `create_tour_draft(projectId, title, language, stops)` accepts explicit
   `nodeId`, `caption`, `transitionMs`, `holdMs`. Hold must be at least 2 seconds.
   Captions are at most 300 characters; use several short stops for long text.
4. Optionally `preview_tour_frame(tourId, timeMs)` returns a PNG to the agent.
5. Show the plan, captions, evidence, duration, and `reviewUrl` to the user.
   The user opens the URL and clicks approval. The agent must not submit that
   form. Rendering before approval fails. Revisions invalidate prior approval.
6. Poll `get_tour_status`; call `render_approved_tour` for the approved revision.
   Rendering returns immediately with a job ID; poll status for completion.
7. Return the output directory and `playerUrl` to the user.

The review URL is available only on the machine running the server. A remote
Codex environment needs a browser-accessible forwarding route; this local MVP
does not deploy a public server. Drafts and snapshots persist in the output
directory and are restored on restart. An interrupted render is marked failed;
revise and approve again to retry. Review/player URLs change port on restart;
get a fresh URL from `get_tour_status`.

## Artifacts

```text
artifacts/<tour-id>/revision-<revision>/
  tour.webm
  tour.vtt
  tour.json
  snapshot.json
  manifest.json
  player.html
```

Captions appear only in the VTT, during each stop's hold interval. Evidence is
rendered into the video; absent ranks are never invented. Class evidence labels
any inherited rank as **Containing file rank**. Identifiers and captions are
escaped when displayed. The manifest records snapshot hash, dimensions, codec,
frame rate, duration and revision. Rendering samples frames from the approved
timeline, so model latency is not included. A two-minute video can take longer
than two minutes to render, depending on CPU and repository size.

Use `playerUrl` while the server runs. For portable playback, serve the output
directory with any local HTTP server and open `player.html`; loading VTT through
`file://` is restricted in some browsers. You can also import WebM and VTT into a
player with external subtitle support.

## Checks

```sh
npm --prefix mcp run typecheck
npm --prefix mcp test
npm --prefix mcp run smoke
```

The smoke command makes an 11-second Sherlock fixture tour with repository →
file → class → repository movement, plus VTT and manifest. It is a developer
fixture, independent of the real-user approval flow. Linux rendering is tested;
Windows execution needs verification on a Windows machine.
