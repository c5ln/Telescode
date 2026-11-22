// Checks that each example's prewritten learning path still plays through the
// current Telescode MCP server, with no AI agent: the server takes the bundled
// curriculum, and a learner who answers every question right, or every one
// wrong, reaches an end the panel can show (finished, the step budget, or the
// prepared material used up, which the panel ends with End session). The
// example's material is the whole level ladder, so a learner who answers every
// question right climbs easy, medium and hard and finishes. The
// tries share one store, as in the app, each as its own learner as the
// example wrapper starts them, so trying an example again still gets questions.
// Before them, the store has learned from the example's previously committed
// material, as an app that ran an earlier version has: a concept whose
// definition changed since then must carry a new version to be accepted.
//
// Run it after regenerating the example data (build-examples.ts) or changing
// the learning server's validation. It downloads and scans each pinned
// commit once (cached under node_modules/.cache/examples-check) and needs
// build/TelescodeHeadless. From frontend/:
//   node --import ../mcp/node_modules/tsx/dist/loader.mjs scripts/check-examples.ts [name...]

import { spawn } from 'node:child_process'
import { existsSync } from 'node:fs'
import { randomUUID } from 'node:crypto'
import { mkdir, mkdtemp, readFile, rm, writeFile } from 'node:fs/promises'
import { tmpdir } from 'node:os'
import { dirname, join, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'

import { Client } from '../../mcp/node_modules/@modelcontextprotocol/sdk/dist/esm/client/index.js'
import { StdioClientTransport } from '../../mcp/node_modules/@modelcontextprotocol/sdk/dist/esm/client/stdio.js'

import { EXAMPLE_REPOSITORIES, type ExampleRepository } from '../src/components/examples'
import { requirementsGoal } from '../src/learning/goal'

const frontend = resolve(dirname(fileURLToPath(import.meta.url)), '..')
const root = resolve(frontend, '..')
const work = join(frontend, 'node_modules/.cache/examples-check')
const headless = join(root, 'build/TelescodeHeadless')
/** Where a learner's path ends; generation-needed is the prepared material used up. */
const ENDS = ['finished', 'budget-exhausted', 'generation-needed']

function run(command: string, args: string[]) {
  return new Promise<void>((done, fail) => {
    const child = spawn(command, args, { cwd: root, stdio: ['ignore', 'ignore', 'pipe'] })
    let stderr = ''
    child.stderr.on('data', (d) => (stderr += d))
    child.on('close', (code) => (code === 0 ? done() : fail(new Error(`${command} ${args[0]} exited with ${code}: ${stderr.slice(-2000)}`))))
  })
}

/** The pinned commit, scanned and ranked as the app does; reused once done. */
async function database(example: ExampleRepository) {
  const dir = join(work, `${example.name}@${example.commit}`)
  const db = join(dir, `${example.name}.db`)
  if (existsSync(`${db}.source`)) return db
  const source = join(dir, 'source')
  const tarball = join(dir, 'source.tgz')
  await rm(source, { recursive: true, force: true })
  await mkdir(source, { recursive: true })
  // A tarball already there (a finished download, or one copied in) is used as is.
  if (!existsSync(tarball)) await run('curl', ['-sfL', `https://api.github.com/repos/${example.owner}/${example.name}/tarball/${example.commit}`, '-o', tarball])
  await run('tar', ['-xzf', tarball, '-C', source, '--strip-components', '1'])
  await run(headless, ['scan', source, db])
  await run(headless, ['algo', db])
  await writeFile(`${db}.source`, source)
  return db
}

type Call = (tool: string, input: Record<string, unknown>) => Promise<any>

/** learning.rs prepare_learning_session with bundled material. */
async function prepare(call: Call, view: any, curriculum: any) {
  const job = await call('request_learning_preparation', { sessionId: view.session.id, revision: view.session.revision, provider: 'claude', ownerPid: process.pid, useCache: false })
  // A goal prepared before reuses its saved material: nothing to run, as in learning.rs.
  if (!job.shouldRun) {
    if (job.status === 'failed') throw new Error(`The preparation failed: ${job.error ?? ''}`)
    return call('get_learning_session', { sessionId: view.session.id })
  }
  let error: string | undefined
  try {
    await call('save_learning_curriculum', { jobId: job.id, curriculum: { ...curriculum, version: (job.baseVersion ?? 0) + 1 } })
  } catch (e) {
    error = (e as Error).message
  }
  await call('finish_learning_preparation', error ? { jobId: job.id, error } : { jobId: job.id })
  if (error) throw new Error(`The server refused the bundled curriculum: ${error}`)
  return call('get_learning_session', { sessionId: view.session.id })
}

/** Plays one learner through the path; returns how it went. */
async function play(example: ExampleRepository, db: string, data: any, answers: 'right' | 'wrong', output: string) {
  const client = new Client({ name: 'telescode-check-examples', version: '1' })
  await client.connect(new StdioClientTransport({
    command: process.execPath,
    args: ['--import', join(root, 'mcp/node_modules/tsx/dist/loader.mjs'), join(root, 'mcp/src/server.ts')],
    env: { ...process.env, TELESCODE_TOUR_OUTPUT: output, TELESCODE_HEADLESS: headless } as Record<string, string>,
    cwd: root,
    stderr: 'ignore',
  }))
  const call: Call = async (tool, input) => {
    const result = await client.callTool({ name: tool, arguments: input })
    const text = (result.content as { text: string }[])[0]?.text ?? ''
    if (result.isError) throw new Error(`${tool}: ${text}`)
    return JSON.parse(text)
  }
  try {
    // As examples/index.ts starts the suggested goal.
    let view = await call('start_learning_session', { path: db, goal: await requirementsGoal(example.requirements), learnerId: `example-${randomUUID()}` })
    view = await prepare(call, view, data.curriculum)
    if (view.session.status !== 'active' || !view.question) throw new Error(`The path did not start with a question: ${view.session.status}`)
    const questions = data.curriculum.items.flatMap((i: any) => i.questions ?? [])
    let asked = 0
    for (let step = 0; view.session.status === 'active' && view.session.current; step++) {
      if (step > 100) throw new Error('The path did not end within 100 steps')
      const s = view.session
      if (view.question) {
        const bundled = questions.find((q: any) => q.question === view.question.question)
        if (!bundled) throw new Error(`A question not in the bundled curriculum: ${view.question.question}`)
        const right = view.question.choices.indexOf(bundled.answer)
        const selectedIndex = answers === 'right' ? right : (right + 1) % view.question.choices.length
        const result = await call('record_learning_response', { sessionId: s.id, revision: s.revision, stopId: s.current.id, submissionId: randomUUID(), questionId: view.question.id, selectedIndex })
        if (result.feedback.correct !== (answers === 'right')) throw new Error(`An answer was marked ${result.feedback.correct ? 'right' : 'wrong'}: ${view.question.question}`)
        view = result.view
        asked++
      }
      view = await call('advance_learning_session', { sessionId: view.session.id, revision: view.session.revision, stopId: view.session.current?.id ?? s.current.id, outcome: 'continue' })
    }
    if (!ENDS.includes(view.session.status)) throw new Error(`The path stopped at ${view.session.status}`)
    if (answers === 'right') {
      if (view.session.status !== 'finished') throw new Error(`All right answers ended at ${view.session.status}, not finished: the material does not climb to hard`)
      const levels = new Set(view.answers.map((a: any) => a.level))
      if (!levels.has('hard')) throw new Error('All right answers never reached hard')
    }
    // The end summary's counts.
    const counted = view.answers.reduce((n: number, a: any) => n + a.asked, 0)
    const countedRight = view.answers.reduce((n: number, a: any) => n + a.correct, 0)
    if (counted !== asked || countedRight !== (answers === 'right' ? asked : 0)) throw new Error(`The summary counts ${countedRight} of ${counted} right, after ${asked} answers all ${answers}`)
    const ended = view.session.status
    if (ended === 'generation-needed') {
      // The panel's End session.
      const stopped = await call('advance_learning_session', { sessionId: view.session.id, revision: view.session.revision, stopId: '', outcome: 'stop' })
      if (stopped.session.status !== 'stopped') throw new Error(`End session left the path ${stopped.session.status}`)
    }
    return `${asked} questions, ${view.session.spent}/${view.session.goal.budget} steps, ${ended}`
  } finally {
    await client.close()
  }
}

/** The example's material as last committed, if it differs from the current one. */
function previous(example: ExampleRepository, current: any): Promise<any | null> {
  return new Promise((done) => {
    const child = spawn('git', ['show', `HEAD:frontend/src/examples/data/${example.name}.json`], { cwd: root, stdio: ['ignore', 'pipe', 'ignore'] })
    let text = ''
    child.stdout.on('data', (d) => (text += d))
    child.on('close', (code) => {
      if (code !== 0) return done(null)
      const data = JSON.parse(text)
      done(JSON.stringify(data.curriculum) === JSON.stringify(current.curriculum) ? null : data)
    })
  })
}

/** One session prepared with `data`'s material in `output`, as an earlier app version would have left it. */
async function learnFrom(example: ExampleRepository, db: string, data: any, output: string) {
  const client = new Client({ name: 'telescode-check-examples', version: '1' })
  await client.connect(new StdioClientTransport({
    command: process.execPath,
    args: ['--import', join(root, 'mcp/node_modules/tsx/dist/loader.mjs'), join(root, 'mcp/src/server.ts')],
    env: { ...process.env, TELESCODE_TOUR_OUTPUT: output, TELESCODE_HEADLESS: headless } as Record<string, string>,
    cwd: root,
    stderr: 'ignore',
  }))
  const call: Call = async (tool, input) => {
    const result = await client.callTool({ name: tool, arguments: input })
    const text = (result.content as { text: string }[])[0]?.text ?? ''
    if (result.isError) throw new Error(`${tool}: ${text}`)
    return JSON.parse(text)
  }
  try {
    const view = await call('start_learning_session', { path: db, goal: await requirementsGoal(example.requirements), learnerId: `previous-${randomUUID()}` })
    await prepare(call, view, data.curriculum)
  } finally {
    await client.close()
  }
}

const names = process.argv.slice(2)
const chosen = EXAMPLE_REPOSITORIES.filter((e) => !names.length || names.includes(e.name))
let failed = false
for (const example of chosen) {
  const data = JSON.parse(await readFile(join(frontend, 'src/examples/data', `${example.name}.json`), 'utf8'))
  const db = await database(example)
  const output = await mkdtemp(join(tmpdir(), 'telescode-check-'))
  const earlier = await previous(example, data)
  if (earlier) {
    try {
      await learnFrom(example, db, earlier, output)
      console.log(`[${example.name}] learned from the previously committed material first`)
    } catch (e) {
      console.log(`[${example.name}] the previously committed material no longer loads (${(e as Error).message.slice(0, 80)}); checking without it`)
    }
  }
  for (const [label, answers] of [['all right', 'right'], ['all wrong', 'wrong'], ['again, all right', 'right']] as const) {
    try {
      console.log(`[${example.name}] ${label}: ${await play(example, db, data, answers, output)}`)
    } catch (e) {
      failed = true
      console.error(`[${example.name}] ${label}: FAILED ${(e as Error).message}`)
    }
  }
  await rm(output, { recursive: true, force: true })
}
process.exit(failed ? 1 : 0)
