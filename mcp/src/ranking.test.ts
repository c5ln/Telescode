import { test } from 'node:test'
import assert from 'node:assert/strict'
import { mkdtemp, mkdir, writeFile, readFile, rm, readdir } from 'node:fs/promises'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { execFile } from 'node:child_process'
import { promisify } from 'node:util'
import { createHash } from 'node:crypto'
import { Client } from '@modelcontextprotocol/sdk/client/index.js'
import { StdioClientTransport } from '@modelcontextprotocol/sdk/client/stdio.js'
import { TourStore } from './store.ts'
import { applyRanking, type RankingResult } from './ranking.ts'
import { parseAnalysis } from '../../frontend/src/bridge/parse.ts'
import { readingTourOrder } from '../../frontend/src/tour/model.ts'
import { pickTutorial } from '../../frontend/src/tour/tutorial.ts'

const root = fileURLToPath(new URL('../..', import.meta.url))
const headless = join(root, 'build/TelescodeHeadless')
const run = promisify(execFile)
const base = JSON.parse(await readFile(new URL('../../frontend/src/graph/fixtures/sherlock.graph.json', import.meta.url), 'utf8'))
const ids = ['sherlock_project/result.py', 'sherlock_project/sites.py']
const scores = { pagerank: 0.1, betweenness: 0.2, combined: 0.3 }
const snapshot = { ...base, files: base.files.filter((f: any) => [...ids, 'sherlock_project/__init__.py'].includes(f.fileId)), readingSequence: [
  ...[...ids, 'sherlock_project/__init__.py'].map((fileId, i) => ({ entityId: fileId, entityType: 'file' as const, fileId, fileRank: i + 1, localRank: null, scores })),
  { entityId: 'class-a', entityType: 'class' as const, fileId: ids[1], fileRank: null, localRank: 2, scores },
  { entityId: 'class-b', entityType: 'class' as const, fileId: ids[1], fileRank: null, localRank: 1, scores },
  { entityId: 'init-class', entityType: 'class' as const, fileId: 'sherlock_project/__init__.py', fileRank: null, localRank: 1, scores },
] }
const ranked: RankingResult = { model: 'lambdarank-v1', query: 'onboarding', queryId: 'tour', files: [
  { fileId: ids[1], score: 0.4, rank: 1 }, { fileId: ids[0], score: -0.2, rank: 2 },
] }

test('model order replaces file ranks while preserving local order and baseline scores', () => {
  const before = structuredClone(snapshot)
  const result = applyRanking(snapshot, ranked)
  assert.deepEqual(result.readingSequence.filter(s => s.entityType === 'file').map(s => [s.fileId, s.fileRank]), [[ids[1], 1], [ids[0], 2]])
  assert.deepEqual(result.readingSequence.filter(s => s.entityType === 'class').map(s => [s.entityId, s.localRank]), [['class-b', 1], ['class-a', 2]])
  assert.deepEqual(result.readingSequence[0].scores, { ...scores, lambdarank: 0.4 })
  assert.equal(result.totals.sequenceCount, 4)
  assert.deepEqual(snapshot, before)
})

for (const [name, files] of Object.entries({
  empty: [], missing: ranked.files.slice(0, 1),
  unknown: [{ ...ranked.files[0], fileId: 'unknown.py' }, ranked.files[1]],
  duplicate: [ranked.files[0], { ...ranked.files[1], fileId: ids[1] }],
  'duplicate rank': [ranked.files[0], { ...ranked.files[1], rank: 1 }],
  'rank gap': [ranked.files[0], { ...ranked.files[1], rank: 3 }],
  'non-finite score': [{ ...ranked.files[0], score: NaN }, ranked.files[1]],
  'wrong score order': [{ ...ranked.files[0], score: -1 }, ranked.files[1]],
})) test(`invalid model results reject: ${name}`, () => {
  assert.throws(() => applyRanking(snapshot, { ...ranked, files }), /ranking/i)
})

async function project() {
  const dir = await mkdtemp(join(tmpdir(), 'telescode-ltr-tour-'))
  const repo = join(dir, 'repo with spaces')
  await mkdir(repo)
  await writeFile(join(repo, '__init__.py'), 'from .main import load_dataset\n')
  await writeFile(join(repo, 'main.py'), 'from helper import transform\nclass Application:\n    def run(self):\n        return transform()\ndef load_dataset():\n    return Application().run()\n')
  await writeFile(join(repo, 'helper.py'), 'def transform():\n    return "application data configuration"\n')
  const db = join(dir, 'project.db')
  await run(headless, ['scan', repo, db])
  await run(headless, ['algo', db])
  return { dir, repo, db }
}

test('DB open runs native ranking, validates draft order and restores the ranked snapshot', async () => {
  const p = await project()
  try {
    await writeFile(`${p.db}.source`, p.repo)
    const before = await readFile(p.db)
    const source = parseAnalysis((await run(headless, ['analyze', p.db])).stdout)
    const store = new TourStore(join(p.dir, 'tours'))
    const opened = await store.open(p.db, headless)
    assert.equal(opened.ranking?.model, 'lambdarank-v1')
    assert.ok(opened.ranking?.query)
    assert.equal(opened.sourceRoot, p.repo)
    assert.equal(opened.sourceSnapshotHash, createHash('sha256').update(JSON.stringify(source)).digest('hex'))
    assert.notEqual(opened.hash, opened.sourceSnapshotHash)
    const order = readingTourOrder(opened.snapshot).filter(n => n.localRank === null)
    assert.equal(order.length, 2)
    assert.ok(!order.some(n => n.nodeId.endsWith('__init__.py')))
    const stop = (nodeId: string) => ({ nodeId, caption: 'A verified stop.', transitionMs: 0, holdMs: 2000 })
    await assert.rejects(store.create(opened.id, 'Wrong order', 'en', order.toReversed().map(n => stop(n.nodeId))), /reading sequence order/)
    const draft = await store.create(opened.id, 'Ranked tour', 'en', order.map(n => stop(n.nodeId)))
    assert.deepEqual(draft.plan.stops.map(s => s.nodeId), order.map(n => n.nodeId))
    assert.ok(draft.plan.stops[0].evidence.some(e => e.label === 'LambdaRank score'))
    const restored = new TourStore(store.outputRoot)
    await restored.restore()
    assert.deepEqual(restored.draft(draft.plan.id).project, opened)
    const listed = await restored.list()
    assert.equal(listed[0].sourceSnapshotHash, opened.sourceSnapshotHash)
    assert.equal(pickTutorial(listed, () => true, opened.sourceSnapshotHash!)?.plan.id, draft.plan.id)
    assert.deepEqual(await readFile(p.db), before)
    assert.deepEqual(parseAnalysis((await run(headless, ['analyze', p.db])).stdout), source)
    assert.deepEqual((await readdir(p.repo)).sort(), ['__init__.py', 'helper.py', 'main.py'])
  } finally { await rm(p.dir, { recursive: true, force: true }) }
})

test('MCP DB workflow exposes native order and saves only tours following that order', async () => {
  const p = await project()
  const client = new Client({ name: 'ranked-tour-test', version: '1.0.0' })
  const env = Object.fromEntries(Object.entries(process.env).filter((e): e is [string, string] => e[1] !== undefined))
  const transport = new StdioClientTransport({
    command: process.execPath,
    args: ['--import', join(root, 'mcp/node_modules/tsx/dist/loader.mjs'), join(root, 'mcp/src/server.ts')],
    cwd: p.dir,
    env: { ...env, TELESCODE_HEADLESS: headless, TELESCODE_TOUR_OUTPUT: join(p.dir, 'tours') },
    stderr: 'pipe',
  })
  let stderr = ''
  transport.stderr?.on('data', data => { stderr += data.toString() })
  try {
    await client.connect(transport)
    const call = async (name: string, args: Record<string, unknown>) => {
      const response = await client.callTool({ name, arguments: args })
      assert.ok(!response.isError, JSON.stringify(response))
      return JSON.parse((response.content as { type: string; text: string }[]).find(c => c.type === 'text')!.text)
    }
    const opened = await call('open_project', { path: p.db, sourceRoot: p.repo })
    assert.equal(opened.ranking.model, 'lambdarank-v1')
    assert.equal(opened.hasSource, true)
    const ambiguous = await client.callTool({ name: 'open_project', arguments: { path: p.db, sourceRoot: p.repo, repoPath: p.dir } })
    assert.equal(ambiguous.isError, true)
    assert.match(JSON.stringify(ambiguous.content), /same source directory/)
    const overview = await call('get_project_overview', { projectId: opened.projectId })
    assert.equal(overview.ranking.query, opened.ranking.query)
    assert.ok(overview.readingSequence.every((s: any) => Number.isFinite(s.scores.lambdarank)))
    const fullOrder = await call('get_reading_tour_order', { projectId: opened.projectId })
    const order = fullOrder.filter((n: any) => n.localRank === null)
    assert.deepEqual(order.map((n: any) => n.title), overview.readingSequence.map((s: any) => s.fileId))
    const stops = order.map((n: any) => ({ nodeId: n.nodeId, caption: 'Verified component.', transitionMs: 0, holdMs: 2000 }))
    const rejected = await client.callTool({ name: 'create_tour_draft', arguments: { projectId: opened.projectId, title: 'Wrong order', stops: stops.toReversed() } })
    assert.equal(rejected.isError, true)
    assert.match(JSON.stringify(rejected.content), /reading sequence order/)
    const draft = await call('create_tour_draft', { projectId: opened.projectId, title: 'Native onboarding', stops })
    assert.deepEqual(draft.plan.stops.map((s: any) => s.nodeId), stops.map((s: any) => s.nodeId))
    const saved = await call('list_tours', {})
    assert.equal(saved[0].sourceSnapshotHash, opened.sourceSnapshotHash)
    const status = await call('get_tour_status', { tourId: draft.plan.id })
    assert.equal(status.state, 'draft')
    const context = await call('get_quiz_context', { projectId: opened.projectId, nodeId: stops[0].nodeId, difficulty: 'hard' })
    assert.ok(context.excerpts.some((e: any) => /def (transform|load_dataset)/.test(e.text)), 'quiz context must read source used by ranking')
    assert.deepEqual(context.learning.after, fullOrder.slice(1, 4).map((n: any) => n.nodeId))
    const quiz = await call('create_quiz', { projectId: opened.projectId, tourId: draft.plan.id, tourRevision: 1, stopIndex: 0, nodeId: stops[0].nodeId, questions: [{
      type: 'true_false', difficulty: 'easy', concept: 'Component behavior',
      question: stops[0].nodeId === 'file:main.py' ? 'load_dataset calls Application.run.' : 'transform returns application data configuration.',
      answer: 'True', explanation: 'The function body in the source excerpt shows this behavior.', evidenceNodeIds: [stops[0].nodeId],
    }] })
    const quizzes = await call('list_quizzes', {})
    const savedQuiz = quizzes.find((q: any) => q.id === quiz.quizSetId)
    assert.equal(savedQuiz.snapshotHash, opened.snapshotHash)
    const response = await call('record_quiz_response', { quizSetId: savedQuiz.id, quizId: savedQuiz.questions[0].id, selectedIndex: 0 })
    assert.equal(response.feedback.correct, true)
    const frames: string[] = []
    for (const timeMs of [0, 2000]) {
      const preview = await client.callTool({ name: 'preview_tour_frame', arguments: { tourId: draft.plan.id, timeMs } })
      assert.ok(!preview.isError, JSON.stringify(preview))
      const image = (preview.content as { type: string; data: string }[]).find(c => c.type === 'image')
      assert.ok(image?.data)
      frames.push(image.data)
    }
    assert.notEqual(frames[0], frames[1], 'the two ranked stops must produce different frames')
  } catch (error) {
    throw new Error(`${String(error)}\n${stderr}`, { cause: error })
  } finally { await client.close(); await rm(p.dir, { recursive: true, force: true }) }
})

test('external databases accept an explicit source root; missing roots reject without fallback', async () => {
  const p = await project()
  try {
    const store = new TourStore(join(p.dir, 'tours'))
    await assert.rejects(store.open(p.db, headless), /repoPath|source|reopen/i)
    assert.equal(store.projects.size, 0)
    const opened = await store.open(p.db, headless, p.repo)
    assert.equal(opened.ranking?.model, 'lambdarank-v1')
    await writeFile(`${p.db}.source`, join(p.dir, 'missing-source'))
    await assert.rejects(store.open(p.db, headless), /source|reopen/i)
  } finally { await rm(p.dir, { recursive: true, force: true }) }
})
