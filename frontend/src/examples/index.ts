// Example repositories play results written in advance: a tour, a quiz for
// every step, and learning material for the suggested goal. Everything else
// runs as usual (download, analysis, the map, grading, the learning session);
// only the agent's writing is skipped.
//
// The data (examples/data/<name>.json) comes from scripts/build-examples.ts.
// It is loaded only when an example is opened.

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

  const attempts = new Map<string, number>()
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
  const suggested = requirementsGoal(data.requirements).then((g) => g.goalId)
  let goalId: string | null = null
  const learning: LearningLibrary = {
    ...libraries.learning,
    start(input) {
      goalId = input.goal.goalId
      return libraries.learning.start(input)
    },
    async prepare(input, onToolCall) {
      return libraries.learning.prepare(goalId === (await suggested) ? { ...input, curriculum: data.curriculum } : input, onToolCall)
    },
  }

  return { tour, quiz, learning }
}
