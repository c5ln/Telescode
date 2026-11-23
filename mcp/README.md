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


## Measuring agent runs

How to measure quiz and adaptive learning generation before and after a change
(#82), and how to keep tests from spending real model tokens (#83).

### What every run records

Each agent run appends one JSON line to `agent-usage.jsonl` in the MCP
server's output folder (`mcp/artifacts/`, or `TELESCODE_TOUR_OUTPUT`). The
folder is git-ignored. The fields that were there before #83 are unchanged;
the rest were added.

| Field | Meaning |
| --- | --- |
| `atMs`, `work`, `provider`, `model`, `durationMs`, `ok`, `usage` | As before. `work` is `tour`, `quiz`, `learning` (first preparation) or `learning-material` (follow-up questions). `usage.inputTokens` is new input, `usage.cachedInputTokens` input read from the prompt cache. |
| `runId` | Unique per run. |
| `errorCode` | The failure's error code (`agent_cancelled`, `tour_generation_failed`, ...). Never its message. |
| `scenario` | `TELESCODE_RUN_SCENARIO` when the app was started with it. Letters, digits, `.`, `_`, `-` only, at most 64; anything else is dropped. |
| `correlation` | `tourId` and `stopIndex` for a quiz; `sessionId` and `jobId` for learning. |
| `metrics.firstEventMs` | From just before the CLI starts to its first event. |
| `metrics.readyMs` | To the CLI being ready: Claude Code's `system/init` (after its MCP server connected), Codex's `turn.started`. |
| `metrics.firstModelOutputMs`, `metrics.firstToolCallMs` | To the model's first output and first tool call. |
| `metrics.modelCalls` | Model requests (distinct assistant messages). Claude Code only; `null` for Codex. |
| `metrics.apiDurationMs` | Time in the model API, as Claude Code reports it; `null` for Codex. |
| `metrics.toolCalls[]` | Each tool call: `tool`, `startMs`, `durationMs`, `argsBytes`, `resultBytes`, `ok`. |
| `metrics.savedQuestions` | Questions in the save calls that succeeded. Exact for quizzes (saved whole or not at all); an upper bound for learning material, from which the server may still drop repeats. |
| `metrics.usageReported` | Whether the stream reported usage. When `false`, the token counts are not known. |

`null` means the agent's stream did not report it. Nothing is estimated. The
log holds names, IDs, timings and sizes only: no prompt, source, model reply,
error message or file path.

### Comparing

```sh
npm --prefix mcp run usage:report -- mcp/artifacts/agent-usage.jsonl
npm --prefix mcp run usage:report -- --before before.jsonl --after after.jsonl
# or one log with both: --before log --before-scenario baseline-quiz --after log --after-scenario after-quiz
```

The report groups runs by scenario, work, provider and model: runs, successes,
median duration, mean time to ready, mean new/cached/output tokens, model
calls, tool calls and tool result bytes, saved questions, and new input plus
output tokens per saved question. Unknown values are shown as `unknown` and
left out of means.

### Baseline scenarios

Measure on `main` at `de42183` (PR #81 merged), then on the change, under the
same conditions.

| | |
| --- | --- |
| Repository | `psf/requests` at `3c7baaa05ab4aa49cc819ce3a819d260de19c157`, cloned and opened as a local folder (opening the pinned example URL plays the bundled material and runs no agent) |
| Quiz | `work=quiz`: the first `file:` stop of a tour of that repository. Write the tour once beforehand with its own scenario name; it is not part of the comparison. |
| Adaptive learning | `work=learning`: start a session with the example goal for requests ("I'm a backend developer who calls requests every day..."). Then `work=learning-material`: answer the first question and let the follow-up questions be written. |
| Cold / warm | Cold: the first run after starting the app. Warm: the same scenario again in the same app session. Use separate scenario names, e.g. `baseline-quiz-cold`, `baseline-quiz-warm`. |
| Repeats | As few as give a stable median; 3 per scenario is suggested. |

Record next to the numbers, outside the repository if it names local paths:
the commit, OS, `claude --version` or `codex --version`, provider, model
(`light` is `sonnet` for Claude Code, `gpt-6-luna` at medium effort for Codex),
and whether anything else was running.

Start the app with the scenario name, e.g.
`TELESCODE_RUN_SCENARIO=baseline-quiz-cold npm --prefix frontend run desktop`.

Or run all of them on Claude Code in one go, without the app window, through
the app's IPC (`claude_generation_baseline_through_app_ipc` in
`frontend/src-tauri/src/app_tests.rs`). It saves the bundled requests tour
directly, so no tour is written, and needs the scanned example and a budget:

```sh
TELESCODE_TEST_DB=<requests.db, with its .source file> TELESCODE_BASELINE_OUTPUT=<folder for the usage log> TELESCODE_REAL_LLM=1 TELESCODE_REAL_LLM_MAX_RUNS=4 TELESCODE_REAL_LLM_MAX_TOKENS=800000 cargo test --manifest-path frontend/src-tauri/Cargo.toml --lib -- --ignored --exact app_tests::claude_generation_baseline_through_app_ipc
```

The follow-up step runs an agent only when the server finds material missing
for the next question; when it does not, that scenario has no run.

### Real model runs in tests

In test builds every agent run goes through `llm_budget::admit`
(`frontend/src-tauri/src/llm_budget.rs`) first, and is refused unless:

- `TELESCODE_AGENT_MOCK=1`: the agent is a fake (`TELESCODE_CLAUDE`/`TELESCODE_CODEX`
  set to a stand-in) that calls no model, or
- `TELESCODE_REAL_LLM=1`, `TELESCODE_REAL_LLM_MAX_RUNS=<n>` and
  `TELESCODE_REAL_LLM_MAX_TOKENS=<n>` are all set, and the ledger shows fewer
  runs and tokens used than that.

So `cargo test` runs no real model. The tests that do (`#[ignore]` in
`app_tests.rs`, and `tours::real_generation` behind `TELESCODE_TEST_CLAUDE`)
need the budget on top of their own switches.

The CLIs cannot stop at an exact token count, so the budget is checked before a
run and charged after it, from the usage the run reported. It is written to the
ledger, `TELESCODE_REAL_LLM_LEDGER` or `frontend/src-tauri/target/real-llm-ledger.jsonl`
(git-ignored), and the next run reads it. Tokens are new input, cached input and
output together. This means:

- one run can go past the budget by what that run used;
- a run whose usage was not reported (a crash, a timeout before the last event)
  is charged everything that was left, so nothing more runs until the ledger is
  deleted;
- parallel test threads take turns, but two test processes sharing a ledger
  are not coordinated.

`frontend/scripts/build-examples.ts` writes every example's tour, quizzes and
learning path with real runs. No npm script, test or build runs it
(`frontend/src/real-llm-guard.test.ts` checks this). Run it by hand, for one
example at a time, only when that example has to change.
