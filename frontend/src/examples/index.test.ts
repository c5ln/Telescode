// @vitest-environment jsdom

import { describe, expect, it, vi } from 'vitest'

import type { AnalysisSnapshot } from '../bridge'
import type { LearningLibrary } from '../learning/api'
import { requirementsGoal } from '../learning/goal'
import type { QuizLibrary } from '../quiz/quiz'
import { snapshotHash, type TourLibrary } from '../tour/tutorial'
import { withExample, type ExampleData } from '.'

const snapshot = { schemaVersion: 1, dbPath: '/cache/requests.db', files: [] } as unknown as AnalysisSnapshot
const question = {
  id: 'q1', type: 'multiple_choice' as const, difficulty: 'easy' as const, concept: 'Sessions',
  question: 'What keeps cookies between calls?', answer: 'A Session', distractors: ['A Response'],
  choices: ['A Response', 'A Session'], explanation: 'Sessions persist cookies.', evidence: [{ nodeId: 'file:src/requests/sessions.py', title: 'sessions.py' }],
}
const data: ExampleData = {
  schemaVersion: 1, owner: 'psf', name: 'requests', commit: 'c'.repeat(40), requirements: 'Show me sessions.',
  tour: { plan: { schemaVersion: 1, id: 'tour-1', revision: 1, title: 'Requests', language: 'en', snapshotHash: 'old', stops: [] } },
  quizzes: [{ schemaVersion: 1, id: 'set-1', tourId: 'tour-1', tourRevision: 1, stopIndex: 0, nodeId: 'root', language: 'en', questions: [question], createdMs: 0 }],
  curriculum: { version: 1 },
}

function libraries() {
  const tour: TourLibrary = { list: vi.fn(() => Promise.reject(new Error('no desktop app'))) }
  const quiz: QuizLibrary = { list: vi.fn(async () => []), respond: vi.fn() }
  const goals: Record<string, string> = {}
  const learning = {
    start: vi.fn(async () => ({})),
    get: vi.fn(async (sessionId: string) => ({ session: { goal: { goalId: goals[sessionId] } } })),
    prepare: vi.fn(async () => ({})),
    goals,
  } as unknown as LearningLibrary & { goals: Record<string, string> }
  return { tour, quiz, learning }
}

describe('withExample', () => {
  it('offers the prewritten tour and quizzes for this analysis', async () => {
    const example = withExample(libraries(), data, snapshot)
    const hash = await snapshotHash(snapshot)

    const [tour] = await example.tour.list()
    expect(tour).toMatchObject({ plan: { id: 'tour-1', snapshotHash: hash }, snapshotHash: hash, sourceSnapshotHash: hash })
    expect((await example.quiz.list())[0]).toMatchObject({ id: 'set-1', snapshotHash: hash })
  })

  it('grades a prewritten quiz in the app and counts attempts', async () => {
    const base = libraries()
    const example = withExample(base, data, snapshot)
    const wrong = await example.quiz.respond({ quizSetId: 'set-1', quizId: 'q1', selectedIndex: 0, responseTimeMs: 900 })
    expect(wrong.feedback).toMatchObject({ correct: false, answer: 'A Session', attempt: 1 })
    const right = await example.quiz.respond({ quizSetId: 'set-1', quizId: 'q1', selectedIndex: 1, responseTimeMs: 500 })
    expect(right.feedback).toMatchObject({ correct: true, explanation: 'Sessions persist cookies.', attempt: 2 })
    expect(base.quiz.respond).not.toHaveBeenCalled()

    // Opening the example again continues the count.
    const again = await withExample(libraries(), data, snapshot).quiz.respond({ quizSetId: 'set-1', quizId: 'q1', selectedIndex: 1, responseTimeMs: 400 })
    expect(again.feedback.attempt).toBe(3)
  })

  it("hands over the prewritten material only for a session with the suggested goal", async () => {
    const base = libraries()
    base.learning.goals.s1 = (await requirementsGoal('Show me sessions.')).goalId
    base.learning.goals.s2 = (await requirementsGoal('Something else entirely.')).goalId
    // The session's own goal decides, not the goal started last or before this wrapper existed.
    const example = withExample(base, data, snapshot)

    await example.learning.prepare({ sessionId: 's2', revision: 1 })
    expect(base.learning.prepare).toHaveBeenLastCalledWith({ sessionId: 's2', revision: 1 }, undefined)

    await example.learning.prepare({ sessionId: 's1', revision: 1 })
    expect(base.learning.prepare).toHaveBeenLastCalledWith({ sessionId: 's1', revision: 1, curriculum: { version: 1 } }, undefined)
  })
})
