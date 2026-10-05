# Telescode agent tours

Local stdio MCP tools let Codex inspect an existing project analysis, propose a
tour, observe draft frames, and render a user-approved tour. Output is silent H.264
MP4 at 1920×1080 / 30fps, with captions burned into the video and an evidence panel.
A separate UTF-8 WebVTT file is retained as a reference artifact.
Onboarding tours have at most 8 stops and 60 seconds including movement. Files and classes
are supported; source-line and member stops are not.

## Setup (Linux / Windows)

Prerequisites: Node 22+, FFmpeg with `libx264` on PATH (`libvpx` for WebM), and the Chromium system
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
TELESCODE_VIDEO_FORMAT = "mp4"  # or "webm" (VP8) for browser-only playback
```

An existing `analyze` JSON or `graph` JSON also works without the C++ executable.
Graph-only snapshots have no reading ranks; file/class tours require an analyze
snapshot with reading sequence results. Run the core algorithm first when ranks
are missing. The reading path uses fileRank, then class localRank within each file.
The built frontend is served by the MCP process, so Vite and Tauri do not need to
run. Rebuild `frontend` after modifying tour UI code.

## Agent workflow

1. `open_project(path)` opens a DB or snapshot; it does not clone or scan.
2. `get_project_overview`, `get_reading_tour_order`, `search_nodes`, `inspect_node` provide existing data
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
  tour.mp4            # tour.webm when TELESCODE_VIDEO_FORMAT=webm
  tour.vtt
  tour.json
  snapshot.json
  manifest.json
  player.html
```

Captions are drawn at the bottom of the video during each stop's hold interval. Evidence is
rendered into the video; absent ranks are never invented. Class evidence labels
any inherited rank as **Containing file rank**. Identifiers and captions are
escaped when displayed. The manifest records snapshot hash, dimensions, codec,
frame rate, duration and revision. Rendering samples frames from the approved
timeline, so model latency is not included. A one-minute video can take longer
than one minute to render, depending on CPU and repository size.

Movement frames are captured individually. During each stop's hold interval,
the renderer captures the first settled screen after the camera, selection and
evidence panel update, then reuses that PNG for the remaining frames. Each stop
gets a fresh capture, even when it visits the same node. The encoder still
receives every 30fps frame, preserving duration and VTT timing. The manifest's
`rendering` object records `capturedFrames`, `reusedFrames` and `elapsedMs`
(startup through encoding completion). Hold reuse assumes the tour screen stays
static; animated panels or clocks would require revisiting this optimization.

## Onboarding prompt and subtitle size

The agent instructions are in [`prompts/onboarding.md`](prompts/onboarding.md).
`src/server.ts` loads this file as MCP initialization instructions; restart the
MCP server after editing it. It defines the onboarding purpose, reading sequence
policy, caption writing, evidence handling, and review workflow. The server does
not generate text itself; the connected agent writes captions using this guidance.

The video shows the current and upcoming stops in a reading path panel. The
caption is drawn into each captured frame at 40px: 100% of the 40px reference
size at 1920×1080. It appears in the video itself, so no HTML or external subtitle
file is needed for playback. Existing videos must be regenerated to include it.
The optional `player.html` does not activate the VTT track, avoiding duplicate
captions. VTT remains available as a reference file; burned captions cannot be
disabled or resized independently during playback.

Use `playerUrl` while the server runs. For portable playback, serve the output
directory with any local HTTP server and open `player.html`; loading VTT through
`file://` is restricted in some browsers. You can also import the video and VTT into a
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
