// Writes the example repositories' tours, quizzes and learning material in
// advance, so opening an example on the first screen plays them at once
// instead of having an agent write them.
//
// Runs the same pipeline as the app, once: download the pinned commit, scan
// and rank it with TelescodeHeadless, then have Claude Code write through the
// Telescode MCP server with the app's own prompts (see tours.rs, quizzes.rs
// and learning.rs). The results land in src/examples/data/<name>.json.
//
// Needs the README prerequisites (build/TelescodeHeadless and TelescodePPR,
// bench/.venv) and a signed-in Claude Code. From frontend/:
//   node --import ../mcp/node_modules/tsx/dist/loader.mjs scripts/build-examples.ts [name...]
// Set CLAUDE to the claude executable if it is not on PATH.
//
// Rerun after changing the examples, their commits, or the core's analysis:
// the tours refer to the map's node IDs at that commit.

import { spawn } from 'node:child_process'
import { existsSync } from 'node:fs'
import { mkdir, readdir, readFile, rm, writeFile } from 'node:fs/promises'
import { dirname, join, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'

import { Client } from '../../mcp/node_modules/@modelcontextprotocol/sdk/dist/esm/client/index.js'
import { StdioClientTransport } from '../../mcp/node_modules/@modelcontextprotocol/sdk/dist/esm/client/stdio.js'

import { EXAMPLE_REPOSITORIES, type ExampleRepository } from '../src/components/examples'
import { requirementsGoal } from '../src/learning/goal'

const frontend = resolve(dirname(fileURLToPath(import.meta.url)), '..')
const root = resolve(frontend, '..')
const work = join(frontend, 'node_modules/.cache/examples')
const out = join(frontend, 'src/examples/data')
const headless = join(root, 'build/TelescodeHeadless')
const claude = process.env.CLAUDE ?? 'claude'
const MINUTE = 60_000

const TOUR_TOOLS = ['open_project', 'get_project_overview', 'search_nodes', 'get_reading_tour_order', 'inspect_node', 'create_tour_draft', 'revise_tour_draft', 'get_tour_status']
const QUIZ_TOOLS = ['open_project', 'inspect_node', 'get_quiz_context', 'create_quiz']
const LEARNING_TOOLS = ['get_learning_preparation_context', 'save_learning_curriculum']

function run(command: string, args: string[], options: { env?: NodeJS.ProcessEnv; cwd?: string; timeout?: number } = {}) {
  return new Promise<string>((done, fail) => {
    const child = spawn(command, args, { cwd: options.cwd ?? root, env: { ...process.env, ...options.env }, stdio: ['ignore', 'pipe', 'pipe'] })
    let stdout = ''
    let stderr = ''
    child.stdout.on('data', (d) => (stdout += d))
    child.stderr.on('data', (d) => (stderr += d))
    const timer = setTimeout(() => child.kill(), options.timeout ?? 15 * MINUTE)
    child.on('close', (code) => {
      clearTimeout(timer)
      if (code === 0) done(stdout)
      else fail(new Error(`${command} ${args[0]} exited with ${code}: ${(stderr || stdout).slice(-2000)}`))
    })
  })
}

/** The Telescode MCP server, as the app starts it. */
function server(artifacts: string) {
  return {
    command: process.execPath,
    args: ['--import', join(root, 'mcp/node_modules/tsx/dist/loader.mjs'), join(root, 'mcp/src/server.ts')],
    env: { TELESCODE_TOUR_OUTPUT: artifacts, TELESCODE_HEADLESS: headless },
  }
}

/** Claude Code in print mode with only the Telescode server and the given tools, as claude.rs runs it. */
function agent(prompt: string, artifacts: string, tools: string[], extra: string[] = []) {
  const { command, args, env } = server(artifacts)
  const config = { mcpServers: { telescode: { type: 'stdio', command, args, env } } }
  return run(claude, ['-p', prompt, '--mcp-config', JSON.stringify(config), '--strict-mcp-config',
    '--allowedTools', tools.map((t) => `mcp__telescode__${t}`).join(','), ...extra], { cwd: work })
}

async function prepareRepository(example: ExampleRepository, dir: string) {
  const source = join(dir, 'source')
  const db = join(dir, `${example.name}.db`)
  if (existsSync(db)) return db
  await mkdir(source, { recursive: true })
  const tarball = join(dir, 'source.tgz')
  await run('curl', ['-sfL', `https://api.github.com/repos/${example.owner}/${example.name}/tarball/${example.commit}`, '-o', tarball])
  await run('tar', ['-xzf', tarball, '-C', source, '--strip-components', '1'])
  await run(headless, ['scan', source, db])
  await run(headless, ['algo', db])
  // Where the MCP server reads source from, as repository.rs records it.
  await writeFile(`${db}.source`, source)
  return db
}

async function artifactsOf(artifacts: string, suffix: string) {
  const names = (await readdir(artifacts).catch(() => [])).filter((n) => n.endsWith(suffix))
  return Promise.all(names.map(async (n) => JSON.parse(await readFile(join(artifacts, n), 'utf8'))))
}

async function writeTour(db: string, artifacts: string) {
  const prompt = `Create a Telescode onboarding tutorial for the repository analyzed in the database ${db}.
Use the telescode MCP tools: open_project with exactly that path, then get_project_overview, get_reading_tour_order and inspect_node, and save the tour with create_tour_draft. Opening the DB computes the LambdaRank onboarding order using the default onboarding query. Use that returned order. If ranking fails, report the error instead of inventing a reading order. Follow the server's onboarding instructions for the reading order and captions, and write the title and captions in English.
Only save the draft. Do not preview frames, render a video, or wait for approval: the user plays the draft as an interactive tutorial. Reply with the tour ID.`
  await agent(prompt, artifacts, TOUR_TOOLS)
  const drafts = await artifactsOf(artifacts, '.draft.json')
  if (drafts.length !== 1) throw new Error(`Expected one tour, found ${drafts.length}`)
  return drafts[0]
}

async function writeQuizzes(db: string, artifacts: string, plan: { id: string; revision: number; stops: { nodeId: string }[] }) {
  const missing = async () => {
    const saved = await artifactsOf(artifacts, '.quiz.json')
    return plan.stops.map((stop, i) => ({ stop, i })).filter(({ i }) => !saved.some((q) => q.stopIndex === i))
  }
  // Three at a time keeps the agent runs from crowding each other out.
  const queue = await missing()
  await Promise.all([0, 1, 2].map(async () => {
    for (let next = queue.shift(); next; next = queue.shift()) {
      const { stop, i } = next
      const prompt = `Create Telescode comprehension quiz questions for stop ${i} (node ${stop.nodeId}) of tour ${plan.id} revision ${plan.revision}, for the repository analyzed in the database ${db}.
Use the telescode MCP tools: open_project with exactly that path, then get_quiz_context for the node at each difficulty (easy, medium, hard), and save with create_quiz. Write two interview-style questions per difficulty where the source supports them: easy at a newbie level, medium at a junior level, hard at a senior level, about what the code does and why, never counts or metrics. Follow the server's quiz instructions on question style and grounding, and write the questions in English.
Only save the quiz. Reply with the quiz set ID.`
      await agent(prompt, artifacts, QUIZ_TOOLS).catch((e) => console.warn(`[${plan.id}] quiz ${i}: ${e.message}`))
    }
  }))
  const left = await missing()
  if (left.length) throw new Error(`No quiz for stops ${left.map((m) => m.i).join(', ')}`)
  return (await artifactsOf(artifacts, '.quiz.json')).sort((a, b) => a.stopIndex - b.stopIndex)
}

/** The curriculum the agent proposes for the example's goal, as it passed it to save_learning_curriculum. */
async function writeCurriculum(db: string, artifacts: string, requirements: string) {
  const { command, args, env } = server(artifacts)
  const client = new Client({ name: 'telescode-examples', version: '1' })
  await client.connect(new StdioClientTransport({ command, args, env: { ...process.env, ...env } as Record<string, string>, cwd: root }))
  const call = async (name: string, input: Record<string, unknown>) => {
    const result = await client.callTool({ name, arguments: input })
    const text = (result.content as { type: string; text: string }[])[0]?.text ?? ''
    if (result.isError) throw new Error(`${name}: ${text}`)
    return JSON.parse(text)
  }
  try {
    const view = await call('start_learning_session', { path: db, goal: await requirementsGoal(requirements) })
    const job = await call('request_learning_preparation', { sessionId: view.session.id, revision: view.session.revision, provider: 'claude', ownerPid: process.pid })
    const prompt = `Prepare Telescode adaptive learning job ${job.id}. Call get_learning_preparation_context with exactly this job ID, inspect its grounded code excerpts and immutable goal, then save_learning_curriculum with the same job ID. Follow the server's adaptive learning instructions. Prepare at least two independent scenario questions per target when supported. Use the requested target levels and existing concept definitions exactly. Save only material; never answer or advance a learner session. Reply with the job ID.`
    const stream = await agent(prompt, artifacts, LEARNING_TOOLS, ['--output-format', 'stream-json', '--verbose'])
    await call('finish_learning_preparation', { jobId: job.id })
    // The last accepted save wins; earlier ones may have been rejected and retried.
    let curriculum: unknown = null
    for (const line of stream.split('\n')) {
      if (!line.trim()) continue
      const event = JSON.parse(line)
      for (const part of event.message?.content ?? []) {
        if (part.type === 'tool_use' && part.name === 'mcp__telescode__save_learning_curriculum') curriculum = part.input.curriculum
      }
    }
    const saved = await call('get_learning_session', { sessionId: view.session.id })
    if (!curriculum || saved.session.status === 'preparing' || !saved.concepts?.length) throw new Error('The agent saved no curriculum')
    return curriculum
  } finally {
    await client.close()
  }
}

async function build(example: ExampleRepository) {
  const dir = join(work, example.name)
  const artifacts = join(dir, 'artifacts')
  await mkdir(artifacts, { recursive: true })
  const log = (step: string) => console.log(`[${example.owner}/${example.name}] ${step}`)
  log('scanning the pinned commit')
  const db = await prepareRepository(example, dir)
  log('writing the onboarding tour')
  const existing = await artifactsOf(artifacts, '.draft.json')
  const draft = existing[0] ?? (await writeTour(db, artifacts))
  log(`writing quizzes for ${draft.plan.stops.length} steps`)
  const quizzes = await writeQuizzes(db, artifacts, draft.plan)
  log('preparing the learning path for the suggested goal')
  const curriculum = await writeCurriculum(db, join(dir, 'learning'), example.requirements)
  await mkdir(out, { recursive: true })
  await writeFile(join(out, `${example.name}.json`), JSON.stringify({
    schemaVersion: 1, owner: example.owner, name: example.name, commit: example.commit, requirements: example.requirements,
    tour: { plan: draft.plan, ranking: draft.project.ranking ?? null },
    quizzes: quizzes.map(({ snapshotHash: _, ...quiz }) => quiz),
    curriculum,
  }, null, 1) + '\n')
  log('done')
}

const names = process.argv.slice(2)
const chosen = EXAMPLE_REPOSITORIES.filter((e) => !names.length || names.includes(e.name))
const results = await Promise.allSettled(chosen.map(build))
for (const [i, r] of results.entries()) if (r.status === 'rejected') console.error(`[${chosen[i]!.name}] failed:`, r.reason)
process.exit(results.some((r) => r.status === 'rejected') ? 1 : 0)
