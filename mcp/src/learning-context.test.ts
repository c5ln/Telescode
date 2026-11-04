import { test } from 'node:test'
import assert from 'node:assert/strict'
import { mkdtemp, mkdir, readFile, writeFile } from 'node:fs/promises'
import { tmpdir } from 'node:os'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { Client } from '@modelcontextprotocol/sdk/client/index.js'
import { StdioClientTransport } from '@modelcontextprotocol/sdk/client/stdio.js'
import { TourStore } from './store.ts'
import { QuizStore } from './quiz.ts'
import { LearningStore, PREPARATION_CONTEXT_CHARS } from './learning.ts'
import { makeQuestions, type QuestionInput } from '../../frontend/src/quiz/model.ts'
import { buildGraphModel } from '../../frontend/src/graph/model.ts'
import type { LearningGoal } from '../../frontend/src/learning/model.ts'

const fixturePath = fileURLToPath(new URL('../../frontend/src/graph/fixtures/sherlock.graph.json', import.meta.url))
const ranked = ['sherlock_project/result.py', 'sherlock_project/notify.py', 'sherlock_project/sherlock.py', 'sherlock_project/sites.py',
  'sherlock_project/__main__.py', 'sherlock_project/__init__.py', 'tests/test_probes.py', 'tests/conftest.py', 'devel/site-list.py']
type Level = 'easy' | 'medium' | 'hard'
type Context = { nodeId: string; excerpts: { text: string }[]; evidence: Record<Level, string[]> }
type Rules = {
  curriculum: { concepts: number; actions: number; relations: number; questionsPerAction: number
    chars: { conceptName: number; definition: number; caption: number; relationReason: number } }
  questions: { minEvidence: Record<Level, number>; distractors: { min: number; max: number }
    chars: { concept: number; question: number; answer: number; distractor: number; explanation: number } }
}

/** Sherlock's graph with long source files and a goal-ranked reading order, like a large real repository. */
async function large() {
  const dir = await mkdtemp(join(tmpdir(), 'telescode-learning-context-'))
  const source = join(dir, 'source')
  const tours = new TourStore(join(dir, 'artifacts'))
  const project = await tours.open(fixturePath, undefined, source)
  for (const f of project.snapshot.files) {
    await mkdir(join(source, dirname(f.fileId)), { recursive: true })
    const body = Array.from({ length: 2500 }, (_, i) => `    value_${i} = compute_${i % 17}(argument_${i}, option=${i})  # step ${i} of ${f.fileId}`)
    await writeFile(join(source, f.fileId), `class Module${f.fileId.length}:\n    def run(self):\n${body.join('\n')}\n`)
  }
  project.snapshot.readingSequence = ranked.map((fileId, i) => ({ entityId: fileId, entityType: 'file', fileId, fileRank: i + 1, localRank: null,
    scores: { pagerank: null, betweenness: null, combined: null } }))
  const store = new LearningStore(tours.outputRoot, tours, new QuizStore(tours.outputRoot, tours))
  const goal: LearningGoal = { goalId: 'large', goalRevision: 1, rawQuery: 'How results reach notifications', retrievalQuery: 'result notify',
    repositoryId: source, sourceSnapshotHash: project.hash, targetConcepts: [{ conceptId: 'A', level: 'medium', weight: 1 }], budget: 6 }
  const started = await store.start(project, goal)
  const job = await store.requestPreparation(started.session.id, started.session.revision, 'claude', process.pid)
  const context = await store.jobContext(job.id) as unknown as { rules: Rules; contexts: Context[]; job: Record<string, unknown> }
  return { project, store, goal, context }
}

test('preparation context stays within its size budget, keeps goal-ranked nodes and drops the per-file source hashes', async () => {
  const f = await large()
  try {
    const size = JSON.stringify(f.context).length
    assert.ok(size <= PREPARATION_CONTEXT_CHARS, `context is ${size} characters, budget ${PREPARATION_CONTEXT_CHARS}`)
    assert.ok(f.context.contexts.length >= 3, 'the top-ranked candidates are kept')
    assert.deepEqual(f.context.contexts.map(c => c.nodeId), ranked.slice(0, f.context.contexts.length).map(id => `file:${id}`))
    assert.ok(f.context.contexts[0]!.excerpts.some(e => e.text.length > 0), 'the most relevant node still comes with its source')
    assert.equal(Object.hasOwn(f.context.job ?? {}, 'sourceEvidence'), false, 'per-file source hashes are internal, not material for the AI')
  } finally { f.store.close() }
})

test('every evidence node the validator accepts for an action node and level is listed in its context, and nothing else', async () => {
  const f = await large()
  try {
    // Every node a question may cite: all non-member graph nodes, including folders and the root.
    const universe = [...buildGraphModel(f.project.snapshot, 'Repository').byId.values()].filter(n => n.kind !== 'member').map(n => n.id)
    assert.ok(f.context.contexts.length > 0)
    for (const c of f.context.contexts) for (const level of ['easy', 'medium', 'hard'] as const) {
      const listed = new Set(c.evidence?.[level] ?? [])
      assert.ok(listed.has(c.nodeId), `${c.nodeId}/${level} lists its own node`)
      for (const candidate of universe) {
        const q: QuestionInput = { type: 'true_false', difficulty: level, concept: 'c', question: 'q?', answer: 'True', distractors: [], explanation: 'e',
          evidenceNodeIds: [...new Set([c.nodeId, candidate, ...[...listed].slice(0, 2)])] }
        let accepted = true
        try { makeQuestions(f.project.snapshot, 's', c.nodeId, [q]) } catch (e) { if (/outside/.test(String(e))) accepted = false; else if (!/at least/.test(String(e))) throw e }
        assert.equal(listed.has(candidate), accepted, `${candidate} for ${c.nodeId}/${level}`)
      }
      // Every advertised level must be answerable from its own list.
      assert.ok(listed.size >= f.context.rules.questions.minEvidence[level], `${c.nodeId}/${level} has enough evidence nodes`)
    }
  } finally { f.store.close() }
})

test('save_learning_curriculum accepts exactly the advertised limits and rejects one step past each of them', { timeout: 120000 }, async () => {
  const cwd = fileURLToPath(new URL('..', import.meta.url))
  const dir = await mkdtemp(join(tmpdir(), 'telescode-learning-limits-'))
  const source = join(dir, 'source')
  const graph = JSON.parse(await readFile(fixturePath, 'utf8')) as { files: { fileId: string }[] }
  for (const file of graph.files.map(f => f.fileId)) {
    await mkdir(join(source, dirname(file)), { recursive: true })
    await writeFile(join(source, file), 'class QueryResult:\n    def __init__(self, status):\n        self.status = status\n')
  }
  const env = Object.fromEntries(Object.entries(process.env).filter((v): v is [string, string] => v[1] !== undefined))
  const client = new Client({ name: 'learning-limits-test', version: '1' })
  await client.connect(new StdioClientTransport({ command: process.execPath, args: ['--import', 'tsx', 'src/server.ts'], cwd,
    env: { ...env, TELESCODE_TOUR_OUTPUT: join(dir, 'artifacts') }, stderr: 'pipe' }))
  try {
    const raw = (name: string, args: Record<string, unknown>) => client.callTool({ name, arguments: args })
    const call = async (name: string, args: Record<string, unknown>) => {
      const result = await raw(name, args)
      assert.ok(!result.isError, JSON.stringify(result))
      return JSON.parse((result.content as { text: string }[])[0]!.text)
    }
    const project = await call('open_project', { path: fixturePath, sourceRoot: source })
    const started = await call('start_learning_session', { projectId: project.projectId, goal: { goalId: 'limits', goalRevision: 1, rawQuery: 'result flow',
      retrievalQuery: 'result flow', targetConcepts: [{ conceptId: 'A', level: 'medium', weight: 1 }], budget: 4 } })
    const job = await call('request_learning_preparation', { sessionId: started.session.id, revision: started.session.revision, provider: 'claude', ownerPid: process.pid })
    const context = await call('get_learning_preparation_context', { jobId: job.id }) as { rules: Rules; contexts: Context[] }
    const { rules } = context
    assert.ok(rules?.questions && rules.curriculum, 'rules are part of the preparation context')
    const R = rules.curriculum, Q = rules.questions
    const anchor = context.contexts.find(c => c.evidence.medium.length >= Q.minEvidence.medium)!
    const evidence = (n: number) => [anchor.nodeId, ...anchor.evidence.medium.filter(id => id !== anchor.nodeId)].slice(0, n)
    // A real graph node that is listed for hard but not for medium.
    const outside = anchor.evidence.hard.find(id => !anchor.evidence.medium.includes(id))!
    assert.ok(outside, 'the outside-evidence probe exists')
    const text = (n: number, tag: string) => `${tag} `.padEnd(n, 'x')
    const question = (i: number) => ({ type: 'multiple_choice', difficulty: 'medium', concept: text(Q.chars.concept, `c${i}`),
      question: text(Q.chars.question, `q${i}`), answer: text(Q.chars.answer, `a${i}`), explanation: text(Q.chars.explanation, `e${i}`),
      distractors: Array.from({ length: Q.distractors.max }, (_, j) => text(Q.chars.distractor, `d${i}.${j}`)), evidenceNodeIds: evidence(Q.minEvidence.medium) })
    const ids = Array.from({ length: R.concepts }, (_, i) => i ? `extra-${i}` : 'A')
    const atLimit = () => ({ version: 1,
      concepts: ids.map((id, i) => ({ id, name: text(R.chars.conceptName, `n${i}`), definition: text(R.chars.definition, `d${i}`), version: 1 })),
      items: Array.from({ length: R.actions }, (_, i) => ({ id: `act-${i}`, kind: 'read', nodeId: anchor.nodeId, conceptId: 'A', level: 'medium',
        caption: text(R.chars.caption, `cap${i}`), questions: i ? [] : Array.from({ length: R.questionsPerAction }, (_, j) => question(j)) })),
      relations: Array.from({ length: R.relations }, (_, i) => ({ from: ids[1 + (i % (ids.length - 1))]!, to: ids[(i % (ids.length - 1) + 2) % ids.length]!, kind: 'similar',
        reason: text(R.chars.relationReason, `r${i}`), evidenceNodeIds: [anchor.nodeId] })) })
    type C = ReturnType<typeof atLimit>
    const over: [string, (c: C) => void][] = [
      ['concepts', c => { c.concepts.push({ id: 'one-more', name: 'n', definition: 'd', version: 1 }) }],
      ['actions', c => { c.items.push({ ...c.items[1]!, id: 'one-more' }) }],
      ['relations', c => { c.relations.push({ ...c.relations[0]! }) }],
      ['questions per action', c => { c.items[0]!.questions.push(question(99)) }],
      ['minimum evidence', c => { c.items[0]!.questions[0]!.evidenceNodeIds = evidence(Q.minEvidence.medium - 1) }],
      ['distractors max', c => { c.items[0]!.questions[0]!.distractors.push('one more wrong choice') }],
      ['distractors min', c => { c.items[0]!.questions[0]!.distractors = c.items[0]!.questions[0]!.distractors.slice(0, Q.distractors.min - 1) }],
      ['evidence outside the level', c => { c.items[0]!.questions[0]!.evidenceNodeIds = [...evidence(Q.minEvidence.medium), outside] }],
      ['relation evidence node', c => { c.relations[0]!.evidenceNodeIds = ['file:not/in/graph.py'] }],
      ...(['concept', 'question', 'answer', 'explanation'] as const).map((k): [string, (c: C) => void] => [`question ${k} chars`, c => { (c.items[0]!.questions[0] as Record<string, unknown>)[k] = text(Q.chars[k] + 1, k) }]),
      ['distractor chars', c => { c.items[0]!.questions[0]!.distractors[0] = text(Q.chars.distractor + 1, 'd') }],
      ['concept name chars', c => { c.concepts[0]!.name = text(R.chars.conceptName + 1, 'n') }],
      ['definition chars', c => { c.concepts[1]!.definition = text(R.chars.definition + 1, 'd') }],
      ['caption chars', c => { c.items[0]!.caption = text(R.chars.caption + 1, 'c') }],
      ['relation reason chars', c => { c.relations[0]!.reason = text(R.chars.relationReason + 1, 'r') }],
    ]
    for (const [name, mutate] of over) {
      const c = atLimit(); mutate(c)
      const result = await raw('save_learning_curriculum', { jobId: job.id, curriculum: c })
      assert.equal(result.isError, true, `one step past ${name} must be rejected`)
    }
    const saved = await call('save_learning_curriculum', { jobId: job.id, curriculum: atLimit() })
    assert.equal(saved.session.status, 'active', 'a curriculum exactly at every advertised limit is accepted')
  } finally { await client.close() }
})
