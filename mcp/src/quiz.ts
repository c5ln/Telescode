import { randomUUID } from 'node:crypto'
import { readFileSync, statSync } from 'node:fs'
import { appendFile, mkdir, readFile, readdir, rename, writeFile } from 'node:fs/promises'
import { resolve, sep } from 'node:path'
import type { Project, TourStore } from './store.ts'
import { feedbackFor, makeQuestions, type QuestionInput, type SourceReader, type QuizFeedback, type QuizResponse, type QuizSet, type TourContext } from '../../frontend/src/quiz/model.ts'

/** Files larger than this are left out of excerpts: they are rarely hand-written code. */
const MAX_SOURCE_BYTES = 1024 * 1024

/** Reads repo-relative files under `root`, refusing any path that leaves it. */
export function sourceReader(root: string | undefined): SourceReader | undefined {
  if (!root) return undefined
  const base = resolve(root)
  return fileId => {
    const path = resolve(base, fileId)
    if (!path.startsWith(base + sep)) return null
    try {
      if (statSync(path).size > MAX_SOURCE_BYTES) return null
      return readFileSync(path, 'utf8')
    } catch { return null }
  }
}

/** Responses are appended, never rewritten: the log is the record a later tour adapts from. */
export const RESPONSES_FILE = 'quiz-responses.jsonl'

/**
 * Quiz sets and the answers to them, beside the tours in the output
 * directory. Read from disk on every call, as tours are, because the agent
 * that writes a quiz runs its own server process.
 */
export class QuizStore {
  constructor(readonly outputRoot: string, readonly tours: TourStore) {}

  private path(id: string) {
    if (!/^[\w-]+$/.test(id)) throw new Error('Invalid quiz ID')
    return resolve(this.outputRoot, `${id}.quiz.json`)
  }

  async create(project: Project, tour: TourContext, language: 'ko' | 'en', questions: QuestionInput[]): Promise<QuizSet> {
    // The step must be one of a saved tour's, so answers can be traced back to it.
    const saved = (await this.tours.list()).find(t => t.plan.id === tour.tourId)
    if (!saved) throw new Error(`Tour not found: ${tour.tourId}`)
    if (saved.plan.revision !== tour.tourRevision) throw new Error(`Tour ${tour.tourId} is at revision ${saved.plan.revision}`)
    if (saved.plan.stops[tour.stopIndex]?.nodeId !== tour.nodeId) throw new Error(`Stop ${tour.stopIndex} of tour ${tour.tourId} is not ${tour.nodeId}`)
    // The tour may predate the current analysis, as the app still plays it then; the
    // questions are checked against the open snapshot, whose hash the quiz records.
    const id = randomUUID()
    const set: QuizSet = {
      schemaVersion: 1, id, ...tour, language, snapshotHash: project.hash,
      questions: makeQuestions(project.snapshot, id, tour.nodeId, questions),
      createdMs: Date.now(),
    }
    await mkdir(this.outputRoot, { recursive: true })
    const path = this.path(id)
    const temporary = `${path}.${randomUUID()}.tmp`
    await writeFile(temporary, JSON.stringify(set, null, 2))
    await rename(temporary, path)
    return set
  }

  async get(id: string): Promise<QuizSet> {
    try { return JSON.parse(await readFile(this.path(id), 'utf8')) as QuizSet } catch { throw new Error(`Quiz not found: ${id}`) }
  }

  async list(): Promise<QuizSet[]> {
    const names = await readdir(this.outputRoot).catch(() => [] as string[])
    const sets = await Promise.all(names.filter(n => n.endsWith('.quiz.json')).map(async name => {
      try {
        const set = JSON.parse(await readFile(resolve(this.outputRoot, name), 'utf8')) as QuizSet
        return set.schemaVersion === 1 && Array.isArray(set.questions) ? [set] : []
      } catch { return [] }
    }))
    return sets.flat()
  }

  async responses(quizSetId?: string): Promise<QuizResponse[]> {
    const raw = await readFile(resolve(this.outputRoot, RESPONSES_FILE), 'utf8').catch(() => '')
    return raw.split('\n').flatMap(line => {
      if (!line.trim()) return []
      try {
        const r = JSON.parse(line) as QuizResponse
        return quizSetId === undefined || r.quizSetId === quizSetId ? [r] : []
      } catch { return [] }
    })
  }

  /** Grades an answer, records it and returns the feedback to show. */
  async respond(quizSetId: string, quizId: string, selectedIndex: number, responseTimeMs: number | null): Promise<{ response: QuizResponse; feedback: QuizFeedback }> {
    const set = await this.get(quizSetId)
    const question = set.questions.find(q => q.id === quizId)
    if (!question) throw new Error(`Question not found: ${quizId}`)
    const selected = question.choices[selectedIndex]
    if (!Number.isInteger(selectedIndex) || selected === undefined) throw new Error('Selected choice is out of range')
    const earlier = (await this.responses(quizSetId)).filter(r => r.quizId === quizId).length
    const response: QuizResponse = {
      schemaVersion: 1,
      id: randomUUID(),
      quizId,
      quizSetId,
      concept: question.concept,
      difficulty: question.difficulty,
      type: question.type,
      result: selected === question.answer ? 'correct' : 'incorrect',
      selectedAnswer: selected,
      attempt: earlier + 1,
      tourContext: { tourId: set.tourId, tourRevision: set.tourRevision, stopIndex: set.stopIndex, nodeId: set.nodeId },
      snapshotHash: set.snapshotHash,
      relatedNodes: question.evidence.map(e => e.nodeId),
      responseTimeMs: responseTimeMs != null && Number.isFinite(responseTimeMs) && responseTimeMs >= 0 ? Math.round(responseTimeMs) : null,
      timestamp: new Date().toISOString(),
    }
    await mkdir(this.outputRoot, { recursive: true })
    await appendFile(resolve(this.outputRoot, RESPONSES_FILE), JSON.stringify(response) + '\n')
    return { response, feedback: feedbackFor(question, response) }
  }
}
