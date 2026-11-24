// Material written while the learner answers: the server works out which
// questions each outcome of the current one would need next (the level above
// if it confirms this one, the same level if it does not), asks an agent for
// just those, and adds them without disturbing the learner's own progress.
import { test } from 'node:test'
import assert from 'node:assert/strict'
import { mkdtemp, mkdir, writeFile } from 'node:fs/promises'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { TourStore } from './store.ts'
import { QuizStore } from './quiz.ts'
import { LearningStore } from './learning.ts'
import { LEARNING_INSTRUCTIONS } from './learning-tools.ts'
import type { LearningGoal } from '../../frontend/src/learning/model.ts'
import type { Curriculum, LearningView, PreparedItem } from '../../frontend/src/learning/contracts.ts'
import { buildQuizContext, type Difficulty } from '../../frontend/src/quiz/model.ts'

const fixturePath = fileURLToPath(new URL('../../frontend/src/graph/fixtures/sherlock.graph.json', import.meta.url))
const nodeA = 'file:sherlock_project/notify.py'
const nodeB = 'file:sherlock_project/result.py'
const ANSWER = 'So the receiver can inspect the outcome'
const LEVELS: Difficulty[] = ['easy', 'medium', 'hard']
const pid = process.pid

const question = (level: Difficulty, suffix: string) => ({ type: 'multiple_choice' as const, difficulty: level, concept: 'Notification state',
  question: `(${level}) Why preserve the result before notifying? ${suffix}`, answer: ANSWER,
  distractors: ['To start another network request', 'To mutate the site configuration'],
  explanation: 'The stored result supplies the outcome to notification consumers.', evidenceNodeIds: level === 'easy' ? [nodeA] : [nodeA, nodeB] })
const item = (id: string, level: Difficulty, suffixes: string[], conceptId = 'A'): PreparedItem => ({ id, kind: level === 'easy' ? 'read' : 'explain',
  nodeId: nodeA, conceptId, level, cost: 1, caption: `Study the notifier at the ${level} level.`,
  effects: [{ conceptId, level, learn: .5 }], questions: suffixes.map(s => question(level, s)) })
/** A small first preparation: the concept and two easy questions. */
const small = (): Curriculum => ({ version: 1, relations: [],
  concepts: [{ id: 'A', name: 'Notification state', definition: 'Explain how notification consumers receive the search outcome', version: 1 }],
  items: [item('easy-A', 'easy', ['first', 'second'])] })

async function setup() {
  const dir = await mkdtemp(join(tmpdir(), 'telescode-material-'))
  const source = join(dir, 'source')
  await mkdir(join(source, 'sherlock_project'), { recursive: true })
  await writeFile(join(source, 'sherlock_project/notify.py'), 'class QueryNotify:\n    def update(self, result):\n        self.result = result\n')
  await writeFile(join(source, 'sherlock_project/result.py'), 'class QueryResult:\n    def __init__(self, status):\n        self.status = status\n')
  const tours = new TourStore(join(dir, 'artifacts'))
  const project = await tours.open(fixturePath, undefined, source)
  const store = new LearningStore(tours.outputRoot, tours, new QuizStore(tours.outputRoot, tours))
  const goal: LearningGoal = { goalId: 'material', goalRevision: 1, rawQuery: 'Understand notification flow', retrievalQuery: 'notification result update',
    repositoryId: source, sourceSnapshotHash: project.hash, budget: 12, targetConcepts: LEVELS.map(level => ({ conceptId: 'A', level, weight: 1 })) }
  /** A learner's session, prepared with the small curriculum. */
  const learner = async (learnerId = 'local') => {
    const started = await store.start(project, goal, learnerId)
    return store.prepare(started.session.id, started.session.revision, small())
  }
  return { store, learner, project, goal }
}

let submission = 0
async function respond(store: LearningStore, view: LearningView, right: boolean) {
  const s = view.session
  const correct = view.question!.choices.indexOf(ANSWER)
  return store.respond({ sessionId: s.id, revision: s.revision, stopId: s.current!.id, submissionId: `material-${++submission}`,
    questionId: view.question!.id, selectedIndex: right ? correct : (correct + 1) % view.question!.choices.length })
}
async function answer(store: LearningStore, view: LearningView, right: boolean) {
  const result = await respond(store, view, right)
  return store.advance(view.session.id, result.view.session.revision, view.session.current!.id, 'continue')
}

test('no material is needed while the prepared questions can follow either answer', async () => {
  const { store, learner } = await setup()
  try {
    const view = await learner()
    const job = await store.requestMaterial(view.session.id, 'claude', pid)
    assert.equal(job.shouldRun, false)
    assert.deepEqual(job.slots, [])
  } finally { store.close() }
})

test('on the last prepared question, both likely outcomes are asked for: the level above if right, the same level if wrong', async () => {
  const { store, learner } = await setup()
  try {
    const view = await answer(store, await learner(), false)
    const job = await store.requestMaterial(view.session.id, 'claude', pid)
    assert.equal(job.shouldRun, true)
    assert.deepEqual(job.slots, [{ branch: 'correct', conceptId: 'A', level: 'medium' }, { branch: 'incorrect', conceptId: 'A', level: 'easy' }])
  } finally { store.close() }
})

test('material is added while the learner answers, without making their answer stale, and a right answer climbs to it', async () => {
  const { store, learner } = await setup()
  try {
    const view = await answer(store, await learner(), false)
    const job = await store.requestMaterial(view.session.id, 'claude', pid)
    await store.saveMaterial(job.id, [item('medium-A', 'medium', ['first', 'second']), item('easy-A-2', 'easy', ['third', 'fourth'])])
    // The learner answers the question they were shown before the material arrived.
    const next = await answer(store, view, true)
    assert.equal(next.session.current!.level, 'medium')
    assert.equal(next.session.current!.actionId, 'medium-A')
  } finally { store.close() }
})

test('a wrong answer takes the same-level material', async () => {
  const { store, learner } = await setup()
  try {
    const view = await answer(store, await learner(), false)
    const job = await store.requestMaterial(view.session.id, 'claude', pid)
    await store.saveMaterial(job.id, [item('medium-A', 'medium', ['first', 'second']), item('easy-A-2', 'easy', ['third', 'fourth'])])
    const next = await answer(store, view, false)
    assert.equal(next.session.current!.actionId, 'easy-A-2')
  } finally { store.close() }
})

test('material outside the asked slots, for an unknown concept, or for an ended job is refused', async () => {
  const { store, learner } = await setup()
  try {
    const view = await answer(store, await learner(), false)
    const job = await store.requestMaterial(view.session.id, 'claude', pid)
    await assert.rejects(store.saveMaterial(job.id, [item('hard-A', 'hard', ['first'])]), /slot/i)
    await assert.rejects(store.saveMaterial(job.id, [item('medium-Z', 'medium', ['first'], 'Z')]), /slot|concept/i)
    await store.finishMaterial(job.id, 'gave up')
    await assert.rejects(store.saveMaterial(job.id, [item('medium-A', 'medium', ['first'])]), /job/i)
  } finally { store.close() }
})

test('a learner already waiting for material continues once it is saved', async () => {
  const { store, learner } = await setup()
  try {
    let view = await answer(store, await learner(), true)
    view = await answer(store, view, false)
    assert.equal(view.session.status, 'generation-needed')
    const job = await store.requestMaterial(view.session.id, 'claude', pid)
    assert.deepEqual(job.slots, [{ branch: 'now', conceptId: 'A', level: 'easy' }])
    const resumed = await store.saveMaterial(job.id, [item('easy-A-2', 'easy', ['third', 'fourth'])])
    assert.equal(resumed.session.status, 'active')
    assert.equal(resumed.session.current!.actionId, 'easy-A-2')
  } finally { store.close() }
})

test('one material job runs per session at a time; asking again joins it', async () => {
  const { store, learner } = await setup()
  try {
    const view = await answer(store, await learner(), false)
    const first = await store.requestMaterial(view.session.id, 'claude', pid)
    const again = await store.requestMaterial(view.session.id, 'claude', pid)
    assert.equal(again.id, first.id)
    assert.equal(again.shouldRun, false)
  } finally { store.close() }
})

test('levels whose material failed twice are not asked for again', async () => {
  const { store, learner } = await setup()
  try {
    const view = await answer(store, await learner(), false)
    for (let i = 0; i < 2; i++) {
      const job = await store.requestMaterial(view.session.id, 'claude', pid)
      assert.equal(job.shouldRun, true)
      await store.finishMaterial(job.id, 'invalid questions')
    }
    const job = await store.requestMaterial(view.session.id, 'claude', pid)
    assert.equal(job.shouldRun, false)
    assert.equal(job.status, 'failed')
  } finally { store.close() }
})

test('material saved once is reused for the next learner with the same goal, with no agent', async () => {
  const { store, learner } = await setup()
  try {
    const first = await answer(store, await learner('first'), false)
    const job = await store.requestMaterial(first.session.id, 'claude', pid)
    await store.saveMaterial(job.id, [item('medium-A', 'medium', ['first', 'second']), item('easy-A-2', 'easy', ['third', 'fourth'])])

    const second = await answer(store, await learner('second'), false)
    const reused = await store.requestMaterial(second.session.id, 'claude', pid)
    assert.equal(reused.shouldRun, false)
    assert.equal(reused.status, 'ready')
    const next = await answer(store, second, true)
    assert.equal(next.session.current!.level, 'medium')
  } finally { store.close() }
})

test('the material context covers only the asked levels and lists the questions already written for them', async () => {
  const { store, learner } = await setup()
  try {
    const view = await answer(store, await learner(), false)
    const job = await store.requestMaterial(view.session.id, 'claude', pid)
    const context = await store.materialContext(job.id)
    assert.deepEqual(context.slots.map(s => s.level), ['medium', 'easy'])
    const easy = context.slots.find(s => s.level === 'easy')!
    assert.deepEqual(easy.written.sort(), ['(easy) Why preserve the result before notifying? first', '(easy) Why preserve the result before notifying? second'])
    for (const slot of context.slots) assert.ok(slot.nodeIds.length > 0)
    // Evidence only for the asked levels.
    for (const c of context.contexts) assert.deepEqual(Object.keys(c.evidence).sort(), ['easy', 'medium'])
    assert.equal(context.concepts[0]!.id, 'A')
  } finally { store.close() }
})

test('slots on the same concept share their source: each candidate is sent once, with the evidence of each level that lists it', async () => {
  const { store, learner, project } = await setup()
  try {
    const view = await answer(store, await learner(), false)
    const job = await store.requestMaterial(view.session.id, 'claude', pid)
    const context = await store.materialContext(job.id)
    assert.equal(context.slots.length, 2)
    const ids = context.contexts.map(c => c.nodeId)
    assert.equal(new Set(ids).size, ids.length, 'each candidate once')
    for (const slot of context.slots) {
      assert.equal(Object.hasOwn(slot, 'contexts'), false, 'slots name their candidates, they do not carry them')
      assert.ok(slot.nodeIds.length <= 3)
      assert.equal(slot.nodeIds[0], nodeA, "the concept's own node first")
      for (const id of slot.nodeIds) {
        const c = context.contexts.find(c => c.nodeId === id)
        assert.ok(c, `${id} is described`)
        // Exactly the nodes the validator accepts for that level.
        assert.deepEqual(c.evidence[slot.level], buildQuizContext(project.snapshot, id, slot.level).nodes.map(n => n.nodeId))
      }
    }
    // A level's evidence is given only on the candidates its slot lists.
    for (const c of context.contexts) for (const level of Object.keys(c.evidence)) {
      assert.ok(context.slots.some(s => s.level === level && s.nodeIds.includes(c.nodeId)), `${c.nodeId} ${level}`)
    }
    // No source line twice in the job's input.
    const lines = context.contexts.flatMap(c => c.excerpts.flatMap(e => e.text.split('\n').flatMap(l => /^(\d+)\| /.exec(l) ? [`${e.fileId}:${/^(\d+)/.exec(l)![1]}`] : [])))
    assert.ok(lines.length > 0)
    assert.equal(new Set(lines).size, lines.length)
  } finally { store.close() }
})

test('the first preparation is told to start small: the lowest goal level, two questions, a few concepts', async () => {
  const { store, learner } = await setup()
  try {
    const view = await learner()
    const context = await store.context(view.session.id)
    assert.deepEqual(context.rules.start, { levels: ['easy'], questions: 2, maxConcepts: 4, concepts: ['A'] })
  } finally { store.close() }
})

test('the agent instructions describe the first preparation and the material written later', () => {
  for (const part of ['rules.start', 'get_learning_material_context', 'save_learning_material', 'only the asked']) assert.ok(LEARNING_INSTRUCTIONS.includes(part), part)
})

test('a first preparation whose questions are all about other concepts is refused, since none of them serves the goal', async () => {
  const { store, project, goal } = await setup()
  try {
    const started = await store.start(project, goal, 'first')
    const curriculum = small()
    curriculum.concepts.push({ id: 'helper', name: 'Helper', definition: 'Explain the helper the notifier calls', version: 1 })
    curriculum.items = [item('easy-helper', 'easy', ['first', 'second'], 'helper')]
    await assert.rejects(store.prepare(started.session.id, started.session.revision, curriculum), /goal concept/i)
    // With one question about the goal concept at its starting level, it is taken, and the goal comes first.
    curriculum.items.push(item('easy-A', 'easy', ['first']))
    const view = await store.prepare(started.session.id, started.session.revision, curriculum)
    assert.equal(view.session.status, 'active')
    assert.equal(view.session.current!.conceptId, 'A')
  } finally { store.close() }
})

test('an outcome the learner is unlikely to reach is not written ahead, and is written once they reach it', async () => {
  const { store, learner } = await setup()
  try {
    // After a right answer the learner is very likely right again, so only the level above is written ahead.
    let view = await answer(store, await learner(), true)
    const ahead = await store.requestMaterial(view.session.id, 'claude', pid)
    assert.deepEqual(ahead.slots, [{ branch: 'correct', conceptId: 'A', level: 'medium' }])
    await store.saveMaterial(ahead.id, [item('medium-A', 'medium', ['first', 'second'])])
    // A wrong answer leaves nothing at easy: that is written now, while the learner waits.
    view = await answer(store, view, false)
    assert.equal(view.session.status, 'generation-needed')
    const now = await store.requestMaterial(view.session.id, 'claude', pid)
    assert.deepEqual(now.slots, [{ branch: 'now', conceptId: 'A', level: 'easy' }])
  } finally { store.close() }
})

test('the material context says which evidence an action may cite', async () => {
  const { store, learner } = await setup()
  try {
    const view = await answer(store, await learner(), false)
    const job = await store.requestMaterial(view.session.id, 'claude', pid)
    const context = await store.materialContext(job.id)
    // Said once for the job: the slot's candidates and that level's evidence.
    assert.match(context.cite, /from that slot's nodeIds/)
    assert.match(context.cite, /evidence\[/)
    // contexts[] holds every slot's candidates, so the looser first-preparation rule must give way.
    assert.match(context.cite, /over rules\.evidence/)
    for (const slot of context.slots) assert.equal(Object.hasOwn(slot, 'cite'), false)
  } finally { store.close() }
})
