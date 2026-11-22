// The level ladder: a goal concept is learned easy, then medium, then hard.
// A level is offered only once the one below it is confirmed (two independent
// right answers and a confident estimate), so a learner who answers well
// climbs, and one who does not stays at or below the level they are on.
import { test } from 'node:test'
import assert from 'node:assert/strict'
import { mkdtemp, mkdir, writeFile } from 'node:fs/promises'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { TourStore } from './store.ts'
import { QuizStore } from './quiz.ts'
import { LearningStore } from './learning.ts'
import type { LearningGoal } from '../../frontend/src/learning/model.ts'
import type { Curriculum, LearningView, PreparedItem } from '../../frontend/src/learning/contracts.ts'
import type { Difficulty } from '../../frontend/src/quiz/model.ts'

const fixturePath = fileURLToPath(new URL('../../frontend/src/graph/fixtures/sherlock.graph.json', import.meta.url))
const nodeA = 'file:sherlock_project/notify.py'
const nodeB = 'file:sherlock_project/result.py'
const ANSWER = 'So the receiver can inspect the outcome'
const LEVELS: Difficulty[] = ['easy', 'medium', 'hard']
const below = (level: Difficulty) => LEVELS[LEVELS.indexOf(level) - 1]

const question = (level: Difficulty, suffix: string) => ({ type: 'multiple_choice' as const, difficulty: level, concept: 'Notification state',
  question: `(${level}) Why preserve the result before notifying? ${suffix}`, answer: ANSWER,
  distractors: ['To start another network request', 'To mutate the site configuration'],
  explanation: 'The stored result supplies the outcome to notification consumers.', evidenceNodeIds: level === 'easy' ? [nodeA] : [nodeA, nodeB] })

/** One item per level, each able to ask three independent questions; a level builds on the one below. */
const item = (level: Difficulty, prerequisites = true): PreparedItem => {
  const lower = prerequisites ? below(level) : undefined
  return { id: `${level}-A`, kind: level === 'easy' ? 'read' : level === 'medium' ? 'explain' : 'compare', nodeId: nodeA, conceptId: 'A', level, cost: 1,
    caption: `Study the notifier at the ${level} level.`,
    effects: [{ conceptId: 'A', level, learn: .5, ...(lower ? { prerequisites: [{ conceptId: 'A', level: lower }] } : {}) }],
    questions: ['first', 'second', 'third'].map(s => question(level, s)) }
}
const ladder = (levels: Difficulty[] = LEVELS, prerequisites = true): Curriculum => ({ version: 1, relations: [],
  concepts: [{ id: 'A', name: 'Notification state', definition: 'Explain how notification consumers receive the search outcome', version: 1 }],
  items: levels.map(level => item(level, prerequisites)) })

async function session(curriculum = ladder(), weights: Record<Difficulty, number> = { easy: 1, medium: 1, hard: 1 }) {
  const dir = await mkdtemp(join(tmpdir(), 'telescode-ladder-'))
  const source = join(dir, 'source')
  await mkdir(join(source, 'sherlock_project'), { recursive: true })
  await writeFile(join(source, 'sherlock_project/notify.py'), 'class QueryNotify:\n    def update(self, result):\n        self.result = result\n')
  await writeFile(join(source, 'sherlock_project/result.py'), 'class QueryResult:\n    def __init__(self, status):\n        self.status = status\n')
  const tours = new TourStore(join(dir, 'artifacts'))
  const project = await tours.open(fixturePath, undefined, source)
  const store = new LearningStore(tours.outputRoot, tours, new QuizStore(tours.outputRoot, tours))
  // The goal concept at every level, as goal.ts builds it.
  const goal: LearningGoal = { goalId: 'ladder', goalRevision: 1, rawQuery: 'Understand notification flow', retrievalQuery: 'notification result update',
    repositoryId: source, sourceSnapshotHash: project.hash, budget: 12, targetConcepts: LEVELS.map(level => ({ conceptId: 'A', level, weight: weights[level] })) }
  const started = await store.start(project, goal)
  const view = await store.prepare(started.session.id, started.session.revision, curriculum)
  return { store, view }
}

let submission = 0
/** Answers the current question right or wrong, then continues; returns the next view. */
async function answer(store: LearningStore, view: LearningView, right: boolean) {
  const s = view.session
  assert.ok(s.current && view.question, `expected a question at ${s.status}`)
  const correct = view.question.choices.indexOf(ANSWER)
  const result = await store.respond({ sessionId: s.id, revision: s.revision, stopId: s.current.id, submissionId: `ladder-${++submission}`,
    questionId: view.question.id, selectedIndex: right ? correct : (correct + 1) % view.question.choices.length })
  return store.advance(s.id, result.view.session.revision, s.current.id, 'continue')
}

test('a learner who answers right climbs easy, medium, hard, two answers each, and finishes', async () => {
  const { store, view: first } = await session()
  try {
    let view = first
    const levels: Difficulty[] = []
    while (view.session.status === 'active' && levels.length < 12) {
      levels.push(view.session.current!.level)
      view = await answer(store, view, true)
    }
    assert.deepEqual(levels, ['easy', 'easy', 'medium', 'medium', 'hard', 'hard'])
    assert.equal(view.session.status, 'finished')
    assert.equal(view.session.spent, 6)
  } finally { store.close() }
})

test('a level is not offered before the one below it is confirmed', async () => {
  const { store, view: first } = await session()
  try {
    assert.equal(first.session.current!.level, 'easy')
    // One right answer is a single observation: not yet confirmed, so still easy.
    const view = await answer(store, first, true)
    assert.equal(view.session.current!.level, 'easy')
  } finally { store.close() }
})

test('a wrong answer at medium keeps the learner at medium or below, never hard', async () => {
  const { store, view: first } = await session()
  try {
    let view = await answer(store, first, true)
    view = await answer(store, view, true)
    assert.equal(view.session.current!.level, 'medium')
    view = await answer(store, view, false)
    assert.equal(view.session.status, 'active')
    assert.notEqual(view.session.current!.level, 'hard')
  } finally { store.close() }
})

test('a level the curriculum has no questions for does not block the one above it', async () => {
  const { store, view: first } = await session(ladder(['easy', 'hard']))
  try {
    let view = await answer(store, first, true)
    view = await answer(store, view, true)
    assert.equal(view.session.current!.level, 'hard')
  } finally { store.close() }
})

test('hard is the goal: confirming only easy and medium does not finish the session', async () => {
  const { store, view: first } = await session(ladder(['easy', 'medium']))
  try {
    let view = first
    for (let i = 0; i < 4; i++) view = await answer(store, view, true)
    // Nothing left to ask at hard, so more material is needed rather than the goal being met.
    assert.equal(view.session.status, 'generation-needed')
  } finally { store.close() }
})

test('the server keeps the order even when the curriculum states no prerequisites and puts hard first', async () => {
  // An agent may leave prerequisites out; hard weighing most would then pull the planner straight to it.
  const { store, view: first } = await session(ladder(['hard', 'medium', 'easy'], false), { easy: 1, medium: 1, hard: 5 })
  try {
    let view = first
    const levels: Difficulty[] = []
    while (view.session.status === 'active' && levels.length < 12) {
      levels.push(view.session.current!.level)
      view = await answer(store, view, true)
    }
    assert.deepEqual(levels, ['easy', 'easy', 'medium', 'medium', 'hard', 'hard'])
  } finally { store.close() }
})

test('once a level is confirmed it is not asked again while the level above has material', async () => {
  // Easy material is listed after medium and outnumbers it, so the planner could fill the step with easy again.
  const extra = (id: string): PreparedItem => ({ ...item('easy'), id, questions: ['fourth', 'fifth'].map(s => question('easy', `${id} ${s}`)) })
  const curriculum = ladder(['medium'])
  curriculum.items.push(item('easy'), extra('easy-A-2'), extra('easy-A-3'))
  // The learning effects the server's tools assign to an agent's actions (learning-tools.ts).
  for (const action of curriculum.items) {
    const lower = below(action.level)
    action.effects = [{ conceptId: 'A', level: action.level, learn: .6, ...(lower ? { prerequisites: [{ conceptId: 'A', level: lower }], unpreparedLearn: .15 } : {}) }]
  }
  const { store, view: first } = await session(curriculum)
  try {
    let view = await answer(store, first, true)
    view = await answer(store, view, true)
    assert.equal(view.session.current!.level, 'medium')
  } finally { store.close() }
})
