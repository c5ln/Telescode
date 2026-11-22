// Example repositories play results written in advance: a tour, a quiz for
// every step, and learning material for the suggested goal. Everything else
// runs as usual (download, analysis, the map, grading, the learning session);
// only the agent's writing is skipped.
//
// The data (examples/data/<name>.json) comes from scripts/build-examples.ts.
// It is loaded only when an example is opened.

import { stoppable } from '../agent'
import type { AnalysisSnapshot } from '../bridge'
import type { ExampleRepository } from '../components/examples'
import type { LearningLibrary } from '../learning/api'
import { requirementsGoal } from '../learning/goal'
import { feedbackFor, type QuizResponse, type QuizSet } from '../quiz/model'
import type { QuizLibrary } from '../quiz/quiz'
import type { TourPlan } from '../tour/model'
import { snapshotHash, type SavedTour, type TourLibrary } from '../tour/tutorial'

export interface ExampleData {
  schemaVersion: 1
  owner: string
  name: string
  commit: string
  requirements: string
  tour: { plan: TourPlan }
  /** One per tour step; the snapshot hash is the one the example opens with. */
  quizzes: Omit<QuizSet, 'snapshotHash'>[]
  /** What the agent passed to save_learning_curriculum for `requirements`. */
  curriculum: unknown
}

/** Attempts at bundled questions, by question; kept across wrappers so reopening an example continues the count. */
const attempts = new Map<string, number>()

const files = import.meta.glob<ExampleData>('./data/*.json', { import: 'default' })

export async function loadExample(example: ExampleRepository): Promise<ExampleData | null> {
  const load = files[`./data/${example.name}.json`]
  return load ? load() : null
}

interface Libraries {
  tour: TourLibrary
  quiz: QuizLibrary
  learning: LearningLibrary
}

/**
 * The app's libraries, with the example's prewritten results first. The
 * results are tied to the example's pinned commit; their snapshot hash is
 * the one this analysis produces, which differs per computer only by the
 * database path inside it.
 */
export function withExample(libraries: Libraries, data: ExampleData, snapshot: AnalysisSnapshot): Libraries {
  const hash = snapshotHash(snapshot).then((h) => h ?? '')
  const saved = <T>(list: () => Promise<T[]>) => list().catch((): T[] => [])

  const tour: TourLibrary = {
    ...libraries.tour,
    async list() {
      const h = await hash
      const bundled: SavedTour = { plan: { ...data.tour.plan, snapshotHash: h }, snapshotHash: h, sourceSnapshotHash: h, state: 'draft', updatedMs: 0 }
      return [bundled, ...(await saved(() => libraries.tour.list()))]
    },
  }

  const quiz: QuizLibrary = {
    ...libraries.quiz,
    async list() {
      const h = await hash
      return [...data.quizzes.map((q) => ({ ...q, snapshotHash: h })), ...(await saved(() => libraries.quiz.list()))]
    },
    async respond(answer) {
      const set = data.quizzes.find((q) => q.id === answer.quizSetId)
      const question = set?.questions.find((q) => q.id === answer.quizId)
      if (!set || !question) return libraries.quiz.respond(answer)
      const selected = question.choices[answer.selectedIndex] ?? ''
      const attempt = (attempts.get(question.id) ?? 0) + 1
      attempts.set(question.id, attempt)
      const result = selected === question.answer ? 'correct' : 'incorrect'
      const response: QuizResponse = {
        schemaVersion: 1, id: crypto.randomUUID(), quizId: question.id, quizSetId: set.id, concept: question.concept,
        difficulty: question.difficulty, type: question.type, result, selectedAnswer: selected, attempt,
        tourContext: { tourId: set.tourId, tourRevision: set.tourRevision, stopIndex: set.stopIndex, nodeId: set.nodeId },
        snapshotHash: await hash, relatedNodes: question.evidence.map((e) => e.nodeId),
        responseTimeMs: answer.responseTimeMs, timestamp: new Date().toISOString(),
      }
      return { response, feedback: feedbackFor(question, response) }
    },
  }

  // The prewritten material fits only the suggested goal; any other goal is
  // prepared by the agent as usual.
  // The session's own goal decides, so a session started before this wrapper
  // existed, or with another goal since, is never given the wrong material.
  const suggested = requirementsGoal(data.requirements).then((g) => g.goalId)
  let preparedGoal: string | null = null
  void suggested.then((goalId) => { preparedGoal = goalId })
  const learning: LearningLibrary = {
    ...libraries.learning,
    preparedGoalId: () => preparedGoal,
    // The server never asks a learner a question twice, across sessions too,
    // so each try of the prepared path is its own learner: a second try gets
    // the same questions instead of none.
    async start(input) {
      const goalId = await suggested
      return libraries.learning.start(input.goal.goalId === goalId ? { ...input, learnerId: `example-${crypto.randomUUID()}` } : input)
    },
    prepare(input, onToolCall) {
      // Kept stoppable: the agent's run is known only once the goal is, and a
      // stop asked for before then stops it as soon as it starts.
      let cancel: (() => void) | undefined
      let cancelled = false
      const run = Promise.all([libraries.learning.get(input.sessionId), suggested]).then(([view, goalId]) => {
        if (view.session.goal.goalId === goalId) {
          // The material is saved once. Saving it again would offer no new
          // questions, so a path that has used it up ends there (the panel
          // offers End session) and no agent is ever run for this goal.
          return view.concepts.length ? view : libraries.learning.prepare({ ...input, curriculum: data.curriculum }, onToolCall)
        }
        const prepared = libraries.learning.prepare(input, onToolCall)
        cancel = prepared.cancel
        if (cancelled) cancel?.()
        return prepared
      })
      return stoppable(run, () => {
        cancelled = true
        cancel?.()
      })
    },
  }

  const material = libraries.learning.prepareMaterial
  if (material) {
    // The suggested goal's material all ships with the example, so no agent writes more of it.
    learning.prepareMaterial = (input, onToolCall) => {
      let cancel: (() => void) | undefined
      let cancelled = false
      const run = Promise.all([libraries.learning.get(input.sessionId), suggested]).then(([view, goalId]) => {
        if (view.session.goal.goalId === goalId) return view
        const written = material(input, onToolCall)
        cancel = written.cancel
        if (cancelled) cancel?.()
        return written
      })
      return stoppable(run, () => {
        cancelled = true
        cancel?.()
      })
    }
  }

  return { tour, quiz, learning }
}
