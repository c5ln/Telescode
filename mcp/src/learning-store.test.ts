import { test } from 'node:test'
import assert from 'node:assert/strict'
import { mkdtemp, mkdir, writeFile } from 'node:fs/promises'
import { tmpdir } from 'node:os'
import { spawnSync } from 'node:child_process'
import { join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { TourStore } from './store.ts'
import { QuizStore } from './quiz.ts'
import { LearningStore } from './learning.ts'
import { makeQuestions } from '../../frontend/src/quiz/model.ts'
import type { LearningGoal } from '../../frontend/src/learning/model.ts'
import type { Curriculum, LearningView, LearningAnswer } from '../../frontend/src/learning/contracts.ts'

const fixturePath = fileURLToPath(new URL('../../frontend/src/graph/fixtures/sherlock.graph.json', import.meta.url))
const nodeA = 'file:sherlock_project/notify.py'
const nodeB = 'file:sherlock_project/result.py'
const question = (suffix: string) => ({ type: 'multiple_choice' as const, difficulty: 'easy' as const, concept: 'Notification state',
  question: `Why preserve the result before notifying? ${suffix}`, answer: 'So the receiver can inspect the outcome',
  distractors: ['To start another network request', 'To mutate the site configuration'],
  explanation: 'The stored result supplies the outcome to notification consumers.', evidenceNodeIds: [nodeA] })

const curriculum = (): Curriculum => ({ version: 1,
  concepts: [{ id: 'A', name: 'Notification state', definition: 'Explain how notification consumers receive the search outcome', version: 1 }],
  relations: [], items: [{ id: 'read-A', kind: 'read', nodeId: nodeA, conceptId: 'A', level: 'easy', cost: 1,
    caption: 'Read how the notifier stores and forwards the outcome.', effects: [{ conceptId: 'A', level: 'easy', learn: .6 }],
    questions: [question('initial condition'), question('a later update'), question('a failed search')] }],
})

async function fixture() {
  const dir = await mkdtemp(join(tmpdir(), 'telescode-learning-'))
  const source = join(dir, 'source')
  await mkdir(join(source, 'sherlock_project'), { recursive: true })
  await writeFile(join(source, 'sherlock_project/notify.py'), 'class QueryNotify:\n    def update(self, result):\n        self.result = result\n')
  await writeFile(join(source, 'sherlock_project/result.py'), 'class QueryResult:\n    def __init__(self, status):\n        self.status = status\n')
  const tours = new TourStore(join(dir, 'artifacts'))
  const project = await tours.open(fixturePath, undefined, source)
  const quizzes = new QuizStore(tours.outputRoot, tours)
  const store = new LearningStore(tours.outputRoot, tours, quizzes)
  const goal: LearningGoal = { goalId: 'notification-flow', goalRevision: 1, rawQuery: '알림 결과 전달 이해',
    retrievalQuery: 'notification result update', repositoryId: source, sourceSnapshotHash: project.hash,
    targetConcepts: [{ conceptId: 'A', level: 'easy', weight: 1 }], budget: 3 }
  const started = await store.start(project, goal)
  return { dir, source, tours, project, quizzes, store, goal, started }
}
async function ready() {
  const f = await fixture()
  const view = await f.store.prepare(f.started.session.id, f.started.session.revision, curriculum())
  assert.ok(view.session.current, 'a prepared teachable goal must produce a current step')
  assert.ok(view.question, 'a prepared step must offer a diagnostic question')
  return { ...f, view }
}
const answer = (view: LearningView, submissionId = 'submission-1', selectedIndex = 0): LearningAnswer => ({ sessionId: view.session.id,
  revision: view.session.revision, stopId: view.session.current!.id, submissionId, questionId: view.question!.id, selectedIndex })

test('session freezes the exact goal revision and starts unconfirmed until curriculum is ready', async () => {
  const f = await fixture()
  try {
    assert.equal(f.started.session.status, 'preparing')
    f.goal.targetConcepts[0]!.weight = 99
    const read = await f.store.get(f.started.session.id)
    assert.equal(read.session.goal.targetConcepts[0]!.weight, 1)
    assert.equal(read.session.goal.rawQuery, '알림 결과 전달 이해')
    assert.equal(read.session.current, null)
  } finally { f.store.close() }
})

test('prepared curriculum validates IDs/evidence/depths and does not accept hidden evaluator data', async () => {
  const f = await fixture()
  try {
    const variants = [
      (c: Curriculum) => { c.items[0]!.conceptId = 'not-registered' },
      (c: Curriculum) => { c.items[0]!.nodeId = 'file:missing.py' },
      (c: Curriculum) => { c.items[0]!.questions[0]!.evidenceNodeIds = [nodeB] },
      (c: Curriculum) => { c.items[0]!.effects.push({ ...c.items[0]!.effects[0]! }) },
      (c: Curriculum) => { c.items[0]!.effects[0]!.level = 'unknown' as 'easy' },
      (c: Curriculum) => { c.items[0]!.questions[0]!.difficulty = 'medium' },
      (c: Curriculum) => { c.items[0]!.observation = { conceptId: 'other', level: 'easy', guess: .3, slip: .1 } },
      (c: Curriculum) => { Object.assign(c, { evaluationQuestions: [{ answer: 'hidden answer' }] }) },
    ]
    for (const mutate of variants) {
      const invalid = curriculum(); mutate(invalid)
      await assert.rejects(f.store.prepare(f.started.session.id, f.started.session.revision, invalid), /concept|node|evidence|duplicate|level|observation|evaluation|curriculum/i)
    }
    assert.equal((await f.store.get(f.started.session.id)).session.status, 'preparing')
  } finally { f.store.close() }
})

test('answer is server-graded once; duplicate submission survives restart and current code stays until advance', async () => {
  const f = await ready()
  try {
    assert.equal(f.view.knowledge[0]!.observations, 0)
    for (const field of ['answer', 'distractors', 'explanation']) assert.equal(Object.hasOwn(f.view.question!, field), false)
    const correctIndex = f.view.question!.choices.indexOf(question('').answer)
    const input = answer(f.view, 'once', correctIndex)
    const result = await f.store.respond(input)
    assert.equal(result.feedback.correct, true)
    assert.equal(result.counted, true)
    assert.equal(result.view.knowledge.find(k => k.conceptId === 'A' && k.level === 'easy')!.observations, 1)
    assert.equal(result.view.session.current!.id, f.view.session.current!.id)
    assert.equal(result.view.session.current!.nodeId, nodeA)
    assert.ok(result.view.session.revision > f.view.session.revision)
    assert.deepEqual(await f.store.respond(input), result)
    f.store.close()
    const restarted = new LearningStore(f.tours.outputRoot, f.tours, f.quizzes)
    try {
      assert.deepEqual(await restarted.respond(input), result)
      const restored = await restarted.get(result.view.session.id)
      assert.equal(restored.knowledge[0]!.observations, 1)
      await assert.rejects(restarted.respond({ ...input, selectedIndex: (correctIndex + 1) % 3 }), /submission/i)
    } finally { restarted.close() }
  } finally { f.store.close() }
})

test('executed teaching uses the same transition-plus-observation model as planning, exactly once', async () => {
  const f = await ready()
  try {
    const input = answer(f.view, 'teach-model', f.view.question!.choices.indexOf(question('').answer))
    const result = await f.store.respond(input)
    // Prior .3 -> assumed teach learn .6 => .72. Correct 3-choice question:
    const expected = (.72 * .9) / (.72 * .9 + .28 / 3)
    const state = result.view.knowledge.find(k => k.conceptId === 'A' && k.level === 'easy')!
    assert.ok(Math.abs(state.probability - expected) < 1e-10)
    assert.equal(state.observations, 1)
    assert.deepEqual((await f.store.respond(input)).view.knowledge, result.view.knowledge)
    const moved = await f.store.advance(result.view.session.id, result.view.session.revision, result.view.session.current!.id, 'continue')
    assert.deepEqual(moved.knowledge, result.view.knowledge, 'advance must not apply the teaching transition again')
  } finally { f.store.close() }
})

test('executed pure diagnostic applies Bayesian evidence without teaching gain', async () => {
  const f = await fixture()
  try {
    const g = { ...f.goal, goalId: 'diagnostic-choice', targetConcepts: [...f.goal.targetConcepts, { conceptId: 'B', level: 'easy' as const, weight: 1 }] }
    const started = await f.store.start(f.project, g)
    const c = curriculum()
    c.concepts.push({ id: 'B', name: 'Result state', definition: 'Explain the result outcome', version: 1 })
    c.items[0]!.id = 'diagnose-A'; c.items[0]!.kind = 'diagnose'; c.items[0]!.effects = []
    c.items.push({ ...structuredClone(c.items[0]!), id: 'teach-A', kind: 'read', cost: 2,
      effects: [{ conceptId: 'A', level: 'easy', learn: 1 }], questions: [question('new application')] })
    c.items.push({ ...structuredClone(c.items[0]!), id: 'teach-B', kind: 'read', conceptId: 'B', cost: 2,
      effects: [{ conceptId: 'B', level: 'easy', learn: 1 }], questions: [question('another application')] })
    const view = await f.store.prepare(started.session.id, started.session.revision, c)
    assert.equal(view.session.current?.kind, 'diagnose')
    const result = await f.store.respond(answer(view, 'diagnostic-only', view.question!.choices.indexOf(question('').answer)))
    const expected = (.3 * .9) / (.3 * .9 + .7 / 3)
    assert.ok(Math.abs(result.view.knowledge.find(k => k.conceptId === 'A' && k.level === 'easy')!.probability - expected) < 1e-10)
    assert.equal(result.view.knowledge.find(k => k.conceptId === 'B' && k.level === 'easy')!.observations, 0)
  } finally { f.store.close() }
})

test('explicit completion without a quiz applies provisional acquisition with zero confirming observations', async () => {
  const f = await fixture()
  try {
    const c = curriculum(); c.items[0]!.questions = []
    const view = await f.store.prepare(f.started.session.id, f.started.session.revision, c)
    assert.ok(view.session.current)
    assert.equal(view.question, null)
    const completed = await f.store.advance(view.session.id, view.session.revision, view.session.current!.id, 'continue')
    const state = completed.knowledge.find(k => k.conceptId === 'A' && k.level === 'easy')!
    assert.ok(Math.abs(state.probability - .72) < 1e-10)
    assert.equal(state.observations, 0, 'model prediction is not confirming quiz evidence')
  } finally { f.store.close() }
})

test('two store connections serialize duplicate requests and reject stale revisions without extra observations', async () => {
  const f = await ready()
  const second = new LearningStore(f.tours.outputRoot, f.tours, f.quizzes)
  try {
    const input = answer(f.view, 'parallel')
    const [a, b] = await Promise.all([f.store.respond(input), second.respond(input)])
    assert.equal(a.responseId, b.responseId)
    assert.equal((await second.get(f.view.session.id)).knowledge[0]!.observations, 1)
    const fresh = await ready()
    const other = new LearningStore(fresh.tours.outputRoot, fresh.tours, fresh.quizzes)
    try {
      const raced = await Promise.allSettled([fresh.store.respond(answer(fresh.view, 'distinct-1')), other.respond(answer(fresh.view, 'distinct-2'))])
      assert.equal(raced.filter(r => r.status === 'fulfilled').length, 1)
      const rejected = raced.find(r => r.status === 'rejected') as PromiseRejectedResult
      assert.match(String(rejected.reason), /revision|stale/i)
      assert.equal((await other.get(fresh.view.session.id)).knowledge[0]!.observations, 1)
    } finally { other.close(); fresh.store.close() }
    await assert.rejects(second.respond({ ...input, submissionId: 'new-but-stale' }), /revision|stale/i)
    assert.equal((await second.get(f.view.session.id)).knowledge[0]!.observations, 1)
  } finally { second.close(); f.store.close() }
})

test('independent sessions merge distinct observations for the same learner instead of overwriting newer mastery', async () => {
  const f = await ready()
  try {
    const secondSession = await f.store.start(f.project, { ...f.goal, goalId: 'parallel-goal' })
    const second = await f.store.prepare(secondSession.session.id, secondSession.session.revision, curriculum())
    await f.store.respond(answer(f.view, 'session-one', f.view.question!.choices.indexOf(question('').distractors[0]!)))
    const moved = await f.store.advance(second.session.id, second.session.revision, second.session.current!.id, 'continue')
    assert.ok(moved.question)
    assert.notEqual(moved.question!.question, f.view.question!.question)
    await f.store.respond(answer(moved, 'session-two', moved.question!.choices.indexOf(question('').distractors[0]!)))
    const thirdSession = await f.store.start(f.project, { ...f.goal, goalId: 'aggregate-evidence' })
    const third = await f.store.prepare(thirdSession.session.id, thirdSession.session.revision, curriculum())
    assert.equal(third.knowledge.find(k => k.conceptId === 'A' && k.level === 'easy')!.observations, 2)
  } finally { f.store.close() }
})

test('advance uses a stable new stop and fresh question; skip and stop do not raise mastery', async () => {
  const f = await ready()
  try {
    const before = structuredClone(f.view.knowledge)
    const skipped = await f.store.advance(f.view.session.id, f.view.session.revision, f.view.session.current!.id, 'skip')
    assert.deepEqual(skipped.knowledge, before)
    assert.ok(skipped.session.spent > 0)
    assert.equal(skipped.session.history[0]!.outcome, 'skip')
    const stopFixture = await ready()
    try {
      const ended = await stopFixture.store.advance(stopFixture.view.session.id, stopFixture.view.session.revision, stopFixture.view.session.current!.id, 'stop')
      assert.equal(ended.session.status, 'stopped')
      assert.deepEqual(ended.knowledge, before)
      assert.equal(ended.session.current, null)
    } finally { stopFixture.store.close() }
    const another = await ready()
    try {
      const answered = await another.store.respond(answer(another.view, 'wrong', another.view.question!.choices.indexOf(question('').distractors[0]!)))
      const moved = await another.store.advance(answered.view.session.id, answered.view.session.revision, answered.view.session.current!.id, 'continue')
      assert.ok(moved.session.current)
      assert.notEqual(moved.session.current!.id, another.view.session.current!.id)
      assert.notEqual(moved.question!.id, another.view.question!.id)
    } finally { another.store.close() }
  } finally { f.store.close() }
})

test('already exposed legacy question and regenerated duplicate never count as a new diagnostic', async () => {
  const f = await fixture()
  try {
    const questions = makeQuestions(f.project.snapshot, 'legacy', nodeA, [question('initial condition')])
    await mkdir(f.tours.outputRoot, { recursive: true })
    await writeFile(join(f.tours.outputRoot, 'legacy.quiz.json'), JSON.stringify({ schemaVersion: 1, id: 'legacy', tourId: 'old-tour',
      tourRevision: 1, stopIndex: 0, nodeId: nodeA, snapshotHash: f.project.hash, language: 'en', questions, createdMs: 1 }))
    await f.quizzes.respond('legacy', questions[0]!.id, 0, null)
    const prepared = await f.store.prepare(f.started.session.id, f.started.session.revision, curriculum())
    assert.ok(prepared.question)
    assert.notEqual(prepared.question!.question, question('initial condition').question)
    assert.equal(prepared.knowledge[0]!.observations, 0)
    await f.store.respond(answer(prepared, 'new'))
    const nextSession = await f.store.start(f.project, { ...f.goal, goalId: 'different-query', rawQuery: '같은 코드 다른 목적' })
    const next = await f.store.prepare(nextSession.session.id, nextSession.session.revision, curriculum())
    assert.notEqual(next.question?.question, prepared.question!.question)
    assert.equal(next.knowledge[0]!.observations, 1)
  } finally { f.store.close() }
})

test('changed source evidence invalidates answering without modifying recorded understanding', async () => {
  const f = await ready()
  try {
    await writeFile(join(f.source, 'sherlock_project/notify.py'), 'class QueryNotify:\n    def update(self, result):\n        self.result = None\n')
    const unchangedAnalysis = await f.tours.open(fixturePath, undefined, f.source)
    assert.equal(unchangedAnalysis.hash, f.project.hash)
    await assert.rejects(f.store.respond(answer(f.view)), /source|evidence/i)
    assert.equal((await f.store.get(f.view.session.id)).knowledge[0]!.observations, 0)
  } finally { f.store.close() }
})

test('knowledge reuse is isolated by learner, immutable concept definition and actual source evidence', async () => {
  const f = await ready()
  try {
    await f.store.respond(answer(f.view, 'learned', f.view.question!.choices.indexOf(question('').answer)))
    const otherLearner = await f.store.start(f.project, { ...f.goal, goalId: 'same-goal-other-user' }, 'other-user')
    const independent = await f.store.prepare(otherLearner.session.id, otherLearner.session.revision, curriculum())
    assert.equal(independent.knowledge[0]!.observations, 0)
    const next = await f.store.start(f.project, { ...f.goal, goalId: 'changed-definition' })
    const changedDefinition = curriculum(); changedDefinition.concepts[0]!.definition = 'A different concept despite reusing the same ID'
    await assert.rejects(f.store.prepare(next.session.id, next.session.revision, changedDefinition), /definition|version|concept/i)
    await writeFile(join(f.source, 'sherlock_project/notify.py'), 'class QueryNotify:\n    def update(self, result):\n        self.result = None\n')
    const projectAfter = await f.tours.open(fixturePath, undefined, f.source)
    assert.equal(projectAfter.hash, f.project.hash)
    const afterSourceChange = await f.store.start(projectAfter, { ...f.goal, goalId: 'same-analysis-new-source' })
    const freshEvidence = await f.store.prepare(afterSourceChange.session.id, afterSourceChange.session.revision, curriculum())
    assert.equal(freshEvidence.knowledge[0]!.observations, 0)
  } finally { f.store.close() }
})

test('executable relevant candidate is selected before the five-action cap', async () => {
  const f = await fixture()
  try {
    const c = curriculum()
    for (let i = 0; i < 5; i++) {
      c.concepts.push({ id: `noise-${i}`, name: `Noise ${i}`, definition: `Unrelated responsibility ${i}`, version: 1 })
      c.items.unshift({ ...structuredClone(c.items[0]!), id: `noise-${i}`, conceptId: `noise-${i}`,
        effects: [{ conceptId: `noise-${i}`, level: 'easy', learn: .6 }], questions: [] })
    }
    const view = await f.store.prepare(f.started.session.id, f.started.session.revision, c)
    assert.equal(view.session.current?.actionId, 'read-A')
  } finally { f.store.close() }
})

test('preparation jobs join, retry only twice, and stale job results cannot replace the active curriculum', async () => {
  const f = await fixture()
  try {
    const first = await f.store.requestPreparation(f.started.session.id, f.started.session.revision, 'codex', process.pid)
    assert.equal(first.shouldRun, true)
    const joined = await f.store.requestPreparation(f.started.session.id, f.started.session.revision, 'codex', process.pid)
    assert.equal(joined.id, first.id)
    assert.equal(joined.shouldRun, false)
    await f.store.finishPreparation(first.id, 'quota exhausted')
    assert.equal((await f.store.get(f.started.session.id)).session.status, 'generation-needed')
    const second = await f.store.requestPreparation(f.started.session.id, (await f.store.get(f.started.session.id)).session.revision, 'codex', process.pid)
    assert.equal(second.shouldRun, true)
    assert.equal(second.attempt, 2)
    await assert.rejects(f.store.prepare(f.started.session.id, first.revision, curriculum(), first.id), /job|stale|revision/i)
    await f.store.finishPreparation(second.id, 'still unavailable')
    const blocked = await f.store.requestPreparation(f.started.session.id, (await f.store.get(f.started.session.id)).session.revision, 'codex', process.pid)
    assert.equal(blocked.shouldRun, false)
    assert.equal(blocked.status, 'failed')
    assert.equal(blocked.attempt, 2)
  } finally { f.store.close() }
})

test('a client that asked to prepare the same revision receives the finished job instead of a stale-revision error', async () => {
  const f = await fixture()
  try {
    const first = await f.store.requestPreparation(f.started.session.id, f.started.session.revision, 'codex', process.pid)
    assert.equal(first.shouldRun, true)
    await f.store.saveJobCurriculum(first.id, curriculum())
    const late = await f.store.requestPreparation(f.started.session.id, f.started.session.revision, 'codex', process.pid)
    assert.equal(late.id, first.id)
    assert.equal(late.shouldRun, false)
    assert.equal(late.status, 'ready')
    const failing = await f.store.start(f.project, { ...f.goal, goalId: 'joined-failure', rawQuery: 'different', retrievalQuery: 'different' })
    const job = await f.store.requestPreparation(failing.session.id, failing.session.revision, 'codex', process.pid)
    await f.store.finishPreparation(job.id, 'quota exhausted')
    const lateFailure = await f.store.requestPreparation(failing.session.id, failing.session.revision, 'codex', process.pid)
    assert.equal(lateFailure.id, job.id)
    assert.equal(lateFailure.shouldRun, false, 'a late joiner must not start a second CLI run')
    assert.equal(lateFailure.status, 'failed')
    await assert.rejects(f.store.requestPreparation(failing.session.id, failing.session.revision - 5, 'codex', process.pid), /stale|revision/i)
  } finally { f.store.close() }
})

test('abandoned worker leases recover after restart and exhausted recovery leaves a resumable generation-needed state', async () => {
  const f = await fixture()
  const clock = Date.now
  const now = clock()
  try {
    const first = await f.store.requestPreparation(f.started.session.id, f.started.session.revision, 'codex', process.pid)
    f.store.close()
    const restarted = new LearningStore(f.tours.outputRoot, f.tours, f.quizzes)
    try {
      Date.now = () => now + 11 * 60_000
      const recovered = await restarted.requestPreparation(first.sessionId, (await restarted.get(first.sessionId)).session.revision, 'codex', process.pid)
      assert.equal(recovered.shouldRun, true)
      assert.equal(recovered.attempt, 2)
      assert.notEqual(recovered.id, first.id)
      await assert.rejects(restarted.saveJobCurriculum(first.id, curriculum()), /stale|revision|job/i)
      Date.now = () => now + 22 * 60_000
      const exhausted = await restarted.requestPreparation(first.sessionId, (await restarted.get(first.sessionId)).session.revision, 'codex', process.pid)
      assert.equal(exhausted.shouldRun, false)
      assert.equal(exhausted.status, 'failed')
      assert.equal((await restarted.get(first.sessionId)).session.status, 'generation-needed')
    } finally { restarted.close() }
  } finally { Date.now = clock; f.store.close() }
})

test('a running job whose app process died is expired on reopen instead of joined, while a live owner is still joined', async () => {
  const f = await fixture()
  const exited = spawnSync(process.execPath, ['-e', ''])
  assert.equal(exited.status, 0)
  const deadPid = exited.pid!
  try {
    // kill(0) and negative pids signal process groups and would make every orphan look alive.
    for (const invalid of [0, -1, 1.5]) await assert.rejects(f.store.requestPreparation(f.started.session.id, f.started.session.revision, 'codex', invalid), /owner|pid/i)
    const orphan = await f.store.requestPreparation(f.started.session.id, f.started.session.revision, 'codex', deadPid)
    assert.equal(orphan.shouldRun, true)
    f.store.close()
    const reopened = new LearningStore(f.tours.outputRoot, f.tours, f.quizzes)
    try {
      const pending = await reopened.get(f.started.session.id)
      const retried = await reopened.requestPreparation(pending.session.id, pending.session.revision, 'codex', process.pid)
      assert.equal(retried.shouldRun, true, 'the crashed worker will never save; a new one must start without waiting for the lease')
      assert.notEqual(retried.id, orphan.id)
      assert.equal(retried.attempt, 2)
      await assert.rejects(reopened.saveJobCurriculum(orphan.id, curriculum()), /stale|revision|job/i)
      const joined = await reopened.requestPreparation(pending.session.id, retried.revision, 'codex', deadPid)
      assert.equal(joined.id, retried.id, 'a job owned by a live process is joined')
      assert.equal(joined.shouldRun, false)
    } finally { reopened.close() }
  } finally { f.store.close() }
})

test('bounded on-demand supplement preserves goal, history and knowledge while replacing exhausted material', async () => {
  const f = await fixture()
  try {
    const initialJob = await f.store.requestPreparation(f.started.session.id, f.started.session.revision, 'codex', process.pid)
    let view = await f.store.saveJobCurriculum(initialJob.id, curriculum())
    for (let i = 0; i < 2; i++) {
      const wrong = view.question!.choices.indexOf(question('').distractors[0]!)
      const answered = await f.store.respond(answer(view, `supplement-${i}`, wrong))
      view = await f.store.advance(view.session.id, answered.view.session.revision, view.session.current!.id, 'continue')
    }
    assert.equal(view.session.spent, 2)
    assert.equal(view.session.status, 'generation-needed')
    const before = structuredClone(view)
    const job = await f.store.requestPreparation(view.session.id, view.session.revision, 'codex', process.pid)
    assert.equal(job.shouldRun, true)
    const supplement = curriculum(); supplement.version = 2
    supplement.items[0]!.id = 'alternate-A'
    supplement.items[0]!.caption = 'Compare a concrete result update with the missing-result case.'
    supplement.items[0]!.questions = [question('independent alternate scenario')]
    const changedTarget = structuredClone(supplement)
    changedTarget.concepts[0]!.version = 2
    changedTarget.concepts[0]!.definition = 'A different assessment target hidden behind the same goal ID'
    await assert.rejects(f.store.saveJobCurriculum(job.id, changedTarget), /target|goal|definition/i)
    await assert.rejects(f.store.saveJobCurriculum(initialJob.id, curriculum()), /stale|revision|job/i)
    const updated = await f.store.saveJobCurriculum(job.id, supplement)
    assert.deepEqual(updated.session.goal, before.session.goal)
    assert.deepEqual(updated.session.history, before.session.history)
    assert.deepEqual(updated.knowledge, before.knowledge)
    assert.equal(updated.session.current?.actionId, 'alternate-A')
    assert.notEqual(updated.question!.question, question('initial condition').question)
    assert.ok(updated.session.spent < updated.session.goal.budget)
  } finally { f.store.close() }
})

test('attaching a supplementary example from another file keeps the concept answer history', async () => {
  const f = await fixture()
  try {
    const initialJob = await f.store.requestPreparation(f.started.session.id, f.started.session.revision, 'codex', process.pid)
    let view = await f.store.saveJobCurriculum(initialJob.id, curriculum())
    for (let i = 0; i < 2; i++) {
      const wrong = view.question!.choices.indexOf(question('').distractors[0]!)
      const answered = await f.store.respond(answer(view, `example-${i}`, wrong))
      view = await f.store.advance(view.session.id, answered.view.session.revision, view.session.current!.id, 'continue')
    }
    assert.equal(view.session.status, 'generation-needed')
    const before = view.knowledge.find(k => k.conceptId === 'A' && k.level === 'easy')!
    assert.equal(before.observations, 2)
    const job = await f.store.requestPreparation(view.session.id, view.session.revision, 'codex', process.pid)
    const supplement = curriculum(); supplement.version = 2
    supplement.items[0]!.id = 'example-B'
    supplement.items[0]!.nodeId = nodeB
    supplement.items[0]!.caption = 'See how the result object carries the status the notifier forwards.'
    supplement.items[0]!.questions = [{ ...question('result object'), evidenceNodeIds: [nodeB] }]
    const updated = await f.store.saveJobCurriculum(job.id, supplement)
    assert.equal(updated.session.current?.nodeId, nodeB)
    assert.deepEqual(updated.knowledge.find(k => k.conceptId === 'A' && k.level === 'easy'), before, 'new material is not a source change')
    const otherMaterial = { ...structuredClone(supplement), version: 1 }
    const next = await f.store.start(f.project, { ...f.goal, goalId: 'later-session-other-material' })
    const reused = await f.store.prepare(next.session.id, next.session.revision, otherMaterial)
    assert.deepEqual(reused.knowledge.find(k => k.conceptId === 'A' && k.level === 'easy'), before, 'history is reused when only material differs')
    await writeFile(join(f.source, 'sherlock_project/notify.py'), 'class QueryNotify:\n    def update(self, result):\n        self.result = None\n')
    const afterChange = await f.store.start(f.project, { ...f.goal, goalId: 'after-history-source-change' })
    const invalidated = await f.store.prepare(afterChange.session.id, afterChange.session.revision, otherMaterial)
    assert.equal(invalidated.knowledge.find(k => k.conceptId === 'A' && k.level === 'easy')!.observations, 0, 'changing the source the answers were based on invalidates them')
  } finally { f.store.close() }
})

test('AI curriculum cannot be accepted against source changed after its retrieved context', async () => {
  const f = await fixture()
  try {
    const job = await f.store.requestPreparation(f.started.session.id, f.started.session.revision, 'codex', process.pid)
    const context = await f.store.jobContext(job.id)
    assert.ok(context.contexts.some(c => c.excerpts.length > 0))
    await writeFile(join(f.source, 'sherlock_project/notify.py'), 'class QueryNotify:\n    def update(self, result):\n        self.result = None\n')
    await assert.rejects(f.store.saveJobCurriculum(job.id, curriculum()), /source|evidence/i)
    assert.equal((await f.store.get(f.started.session.id)).session.status, 'preparing')
  } finally { f.store.close() }
})

test('identical prepared materials are reused across sessions without another AI execution', async () => {
  const f = await fixture()
  try {
    const first = await f.store.requestPreparation(f.started.session.id, f.started.session.revision, 'codex', process.pid)
    await f.store.saveJobCurriculum(first.id, curriculum())
    const next = await f.store.start(f.project, { ...f.goal, goalId: 'same-goal-new-session' })
    const cached = await f.store.requestPreparation(next.session.id, next.session.revision, 'codex', process.pid)
    assert.equal(cached.shouldRun, false)
    assert.equal(cached.status, 'ready')
    assert.ok((await f.store.get(next.session.id)).session.current)
    const changedGoal = await f.store.start(f.project, { ...f.goal, goalId: 'different-purpose', rawQuery: '다른 목표', retrievalQuery: 'different goal' })
    assert.equal((await f.store.requestPreparation(changedGoal.session.id, changedGoal.session.revision, 'codex', process.pid)).shouldRun, true)
    const changedProvider = await f.store.start(f.project, { ...f.goal, goalId: 'different-provider' })
    assert.equal((await f.store.requestPreparation(changedProvider.session.id, changedProvider.session.revision, 'claude', process.pid)).shouldRun, true)
    await writeFile(join(f.source, 'sherlock_project/notify.py'), 'class QueryNotify:\n    def update(self, result):\n        self.result = None\n')
    const changedSource = await f.store.start(f.project, { ...f.goal, goalId: 'different-source-same-analysis' })
    assert.equal((await f.store.requestPreparation(changedSource.session.id, changedSource.session.revision, 'codex', process.pid)).shouldRun, true)
  } finally { f.store.close() }
})

test('fresh generation context exposes the existing target definition instead of silently redefining its ID', async () => {
  const f = await ready()
  try {
    const next = await f.store.start(f.project, { ...f.goal, goalId: 'new-session-same-target' })
    const context = await f.store.context(next.session.id)
    assert.equal(context.priorConcepts.find(c => c.id === 'A')?.definition, curriculum().concepts[0]!.definition)
  } finally { f.store.close() }
})

test('a session can end without a current step and invalidates unfinished generation', async () => {
  const f = await fixture()
  try {
    const job = await f.store.requestPreparation(f.started.session.id, f.started.session.revision, 'codex', process.pid)
    const pending = await f.store.get(f.started.session.id)
    const ended = await f.store.advance(pending.session.id, pending.session.revision, '', 'stop')
    assert.equal(ended.session.status, 'stopped')
    assert.equal(ended.session.spent, 0)
    assert.ok(ended.session.revision > job.revision)
    await assert.rejects(f.store.saveJobCurriculum(job.id, curriculum()), /stale|revision|job/i)
  } finally { f.store.close() }
})

test('a cached material that no longer applies falls back to an AI run instead of leaving a running job', async () => {
  const f = await fixture()
  try {
    const first = await f.store.requestPreparation(f.started.session.id, f.started.session.revision, 'codex', process.pid)
    await f.store.saveJobCurriculum(first.id, curriculum())
    // A cache entry the store can no longer apply (here: it fails curriculum validation).
    f.store.db.prepare('UPDATE learning_material_cache SET data=?').run(JSON.stringify({ ...curriculum(), items: [] }))
    const next = await f.store.start(f.project, { ...f.goal, goalId: 'cache-cannot-apply' })
    const job = await f.store.requestPreparation(next.session.id, next.session.revision, 'codex', process.pid)
    assert.equal(job.shouldRun, true, 'the CLI must run when the cached material cannot be applied')
    assert.equal(job.status, 'running')
    assert.equal(f.store.db.prepare('SELECT COUNT(*) AS n FROM learning_material_cache').get()!.n, 0, 'the unusable cache entry is dropped')
    await f.store.saveJobCurriculum(job.id, curriculum())
    assert.equal((await f.store.get(next.session.id)).session.status, 'active')
  } finally { f.store.close() }
})
