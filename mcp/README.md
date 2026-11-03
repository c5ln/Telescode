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

## LambdaRank onboarding order

Opening an analysis **DB** now generates the released model's 19 features and
runs `TelescodeHeadless rank`. Files follow descending model score; classes keep
their existing local ranks. The default onboarding query is defined in
[`ltr/tour_rank.py`](../ltr/tour_rank.py). There is no query input in the desktop
UI yet. Its internal `rank_repository(..., query=...)` API is ready for that
extension. A generic onboarding query's ranking quality has not been evaluated
as a separate training task.

Build `TelescodeHeadless` and `TelescodePPR`, and use a Python environment with
the dependencies in `bench/requirements.txt`. The service discovers
`bench/.venv` or `.venv` under this repository. Override `TELESCODE_LTR_PYTHON`
and `TELESCODE_PPR` when those executables live elsewhere. Native inference
also needs the linked LightGBM shared library at runtime.

The desktop writes the absolute source directory to `<database>.source` after
a successful scan/algo pass. This is the same source record used by quizzes.
Reopen the folder or GitHub URL once for databases without this record. An
external DB can be opened directly with `open_project(path, sourceRoot)`;
`sourceRoot` is the source directory matching its latest scan/update and algo
pass. The earlier LambdaRank `repoPath` argument remains an alias. Downloaded
repositories can also use the existing `source` directory beside their DB.

Ranking changes only the tour snapshot. Baseline DB ranks and scores remain
available for feature generation. The project exposes `ranking` (model, query,
query ID), and file evidence includes its LambdaRank score. Invalid or failed
ranking does not fall back to a different ordering. Initializer files and their
contained entities are excluded from the tour reading path under the v1 model
policy. JSON inputs continue to use their recorded reading sequence.

Saved tours retain both the ranked snapshot hash and the original analysis hash,
so the desktop can find and play the tour against its original repository map.
Desktop onboarding regenerates tours made before model integration; existing
saved drafts remain available through MCP. Ranked tours are matched to their
source hash rather than unrelated repositories sharing the same file names.

## Agent workflow

1. `open_project(path)` opens a DB with default LambdaRank onboarding order or
   opens a JSON snapshot as recorded; it does not clone or scan. For external
   DBs without source metadata, also pass `sourceRoot` (`repoPath` is an alias).
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

## Tour quizzes

Comprehension quizzes on a tour stop. The desktop app has Claude Code write them
(`src-tauri/src/quizzes.rs`) with only the tools below; guidance is in
[`prompts/quiz.md`](prompts/quiz.md), served with the onboarding instructions.

1. `get_quiz_context(projectId, nodeId, difficulty)` is the GraphRAG retrieval step
   (`frontend/src/quiz/model.ts`). It returns the code-graph nodes and relations within
   reach, plus numbered source excerpts of them: easy covers the node and its children,
   medium adds one hop, hard adds two hops plus the reading order around the node
   (`learning`). Code relations and reading order are kept separate, and metrics and
   counts are left out so questions are about what the code does.
2. `create_quiz(projectId, tourId, tourRevision, stopIndex, nodeId, questions)` saves
   multiple-choice and true/false questions, 1–3 per difficulty. Each question's
   `evidenceNodeIds` must lie in the context for its difficulty and include the stop's
   node (easy ≥1, medium ≥2, hard ≥2 nodes); otherwise the question is refused.
3. `list_quizzes`, `record_quiz_response` and `list_quiz_responses` are for the app.
   The server grades each answer and appends it to `quiz-responses.jsonl` in the output
   directory with `quizId`, `concept`, `difficulty`, `result`, `selectedAnswer`,
   `attempt`, `tourContext`, `relatedNodes`, `responseTimeMs` and `timestamp`, for a
   later adaptive tour to read.

Questions are written like interview questions: easy at a newbie level (what the code
is for and does), medium at a junior level (flow across related code, debugging), hard at
a senior level (trade-offs, failure modes, extension). Excerpts are read from the source
folder the desktop shell records beside each scanned database (`<db>.source`), or a
`source` folder beside it; `open_project` also takes `sourceRoot`. Files outside that
folder are never read. Without a source folder, excerpts are empty.

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
