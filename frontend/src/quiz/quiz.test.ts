import { describe, expect, it } from 'vitest'

import type { AnalysisSnapshot, GraphResponse } from '../bridge'
import sherlock from '../graph/fixtures/sherlock.graph.json'
import { buildQuizContext, makeQuestions, type QuestionInput, type QuizSet } from './model'
import { pickQuiz } from './quiz'

const snapshot: AnalysisSnapshot = { ...(sherlock as GraphResponse), dbPath: 'x.db', readingSequence: [] }
const at = { tourId: 't1', tourRevision: 2, stopIndex: 1, nodeId: 'file:a.py' }

const set = (id: string, over: Partial<QuizSet> = {}): QuizSet => ({
  schemaVersion: 1,
  id,
  ...at,
  language: 'en',
  snapshotHash: 'other',
  questions: [{ id: `${id}:1`, type: 'true_false', difficulty: 'easy', concept: 'c', question: 'q', answer: 'True', distractors: ['False'], choices: ['True', 'False'], explanation: 'e', evidence: [] }],
  createdMs: 0,
  ...over,
})

describe('pickQuiz', () => {
  it('takes only a quiz written for this revision and stop of the tour', () => {
    expect(pickQuiz([set('a', { tourRevision: 1 })], at, 'h')).toBeNull()
    expect(pickQuiz([set('a', { stopIndex: 0 })], at, 'h')).toBeNull()
    expect(pickQuiz([set('a', { nodeId: 'root' })], at, 'h')).toBeNull()
    expect(pickQuiz([set('a', { questions: [] })], at, 'h')).toBeNull()
    expect(pickQuiz([set('a')], at, 'h')?.id).toBe('a')
  })

  it('prefers this exact snapshot, then the newest', () => {
    expect(pickQuiz([set('old', { createdMs: 1 }), set('new', { createdMs: 2 })], at, 'h')?.id).toBe('new')
    expect(pickQuiz([set('exact', { snapshotHash: 'h' }), set('new', { createdMs: 2 })], at, 'h')?.id).toBe('exact')
  })
})

describe('quiz questions', () => {
  const question: QuestionInput = {
    type: 'multiple_choice',
    difficulty: 'easy',
    concept: 'Layout',
    question: 'Which directory holds the tests?',
    answer: 'tests',
    distractors: ['devel', 'sherlock_project'],
    explanation: 'tests is a top-level directory.',
    evidenceNodeIds: ['root', 'dir:tests'],
  }

  it('shuffles choices the same way every time', () => {
    const a = makeQuestions(snapshot, 's', 'root', [question])[0]!
    const b = makeQuestions(snapshot, 's', 'root', [question])[0]!
    expect(a.choices).toEqual(b.choices)
    expect([...a.choices].sort()).toEqual(['devel', 'sherlock_project', 'tests'])
    expect(a.id).toBe('s:1')
  })

  it('refuses evidence the difficulty does not reach', () => {
    // A file two levels down is beyond an easy question on the repository root.
    expect(() => makeQuestions(snapshot, 's', 'root', [{ ...question, evidenceNodeIds: ['root', 'file:tests/test_ux.py'] }])).toThrow(/outside the easy context/)
    expect(() => makeQuestions(snapshot, 's', 'root', [{ ...question, difficulty: 'hard', evidenceNodeIds: ['root', 'dir:tests', 'file:tests/test_ux.py'] }])).not.toThrow()
  })

  it('keeps the code graph and reading order apart', () => {
    const hard = buildQuizContext(snapshot, 'file:sherlock_project/notify.py', 'hard')
    expect(hard.relations.some((r) => r.kind === 'imports')).toBe(true)
    expect(hard.relations.every((r) => ['contains', 'imports'].includes(r.kind) || r.kind.startsWith('class-'))).toBe(true)
    expect(hard.learning).toEqual({ before: [], after: [] })
  })
})
