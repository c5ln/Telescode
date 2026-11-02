// A quiz on the current tutorial step: find the quiz set written for it, or
// have one written, then ask its questions one difficulty at a time and
// record each answer. Recording and grading happen in the MCP server; this
// only keeps what the panel shows.

import { useCallback, useRef, useState, type RefObject } from 'react'

import type { AnalysisSnapshot } from '../bridge'
import type { GraphHandle } from '../graph/GraphCanvas'
import { DIFFICULTIES, type Difficulty, type QuizFeedback, type QuizQuestion, type QuizSet, type TourContext } from '../quiz/model'
import { pickQuiz, type QuizLibrary } from '../quiz/quiz'
import { snapshotHash, type SavedTour } from '../tour/tutorial'
import type { SetupPhase } from './useTutorial'

export type AnswerState =
  | { status: 'open' }
  | { status: 'submitting' }
  | { status: 'answered'; feedback: QuizFeedback }
  | { status: 'error'; message: string }

export type QuizState =
  | { status: 'off' }
  | { status: 'searching'; at: TourContext }
  /** No quiz for the step yet, so one is written, as for tutorials. */
  | { status: 'setup'; at: TourContext; phase: SetupPhase; error: string | null }
  | {
      status: 'ready'
      at: TourContext
      set: QuizSet
      difficulty: Difficulty
      /** Position among the questions of `difficulty`. */
      index: number
      selected: number | null
      answer: AnswerState
    }

export interface Quiz {
  state: QuizState
  /** Quiz the user on one step of a tour. */
  open: (tour: SavedTour, stopIndex: number) => void
  /** Back to the tutorial, with the map on the step again. */
  close: () => void
  setDifficulty: (difficulty: Difficulty) => void
  select: (choice: number) => void
  submit: () => void
  /** Answer the same question again, as a new attempt. */
  retry: () => void
  next: () => void
  /** Show one of the nodes an answer rests on. */
  showNode: (nodeId: string) => void
}

const OFF: QuizState = { status: 'off' }

/** The questions of one difficulty, in the order the set holds them. */
export function questionsOf(set: QuizSet, difficulty: Difficulty): QuizQuestion[] {
  return set.questions.filter((q) => q.difficulty === difficulty)
}

export function useQuiz(
  library: QuizLibrary,
  snapshot: AnalysisSnapshot | null,
  dbPath: string | null,
  graphRef: RefObject<GraphHandle | null>,
): Quiz {
  // State belongs to one snapshot, as the tutorial's does.
  const [owned, setOwned] = useState<{ snapshot: AnalysisSnapshot | null; state: QuizState }>({ snapshot, state: OFF })
  const state = owned.snapshot === snapshot ? owned.state : OFF
  const setState = useCallback((next: QuizState) => setOwned({ snapshot, state: next }), [snapshot])

  const searchId = useRef(0)
  // When the current question was put in front of the user, for response time.
  const shownAt = useRef(0)
  const moved = useRef(false)

  const show = useCallback(
    (next: Extract<QuizState, { status: 'ready' }>) => {
      shownAt.current = performance.now()
      setState(next)
    },
    [setState],
  )

  const ready = useCallback(
    (at: TourContext, set: QuizSet) => {
      const difficulty = DIFFICULTIES.find((d) => questionsOf(set, d).length > 0) ?? 'easy'
      show({ status: 'ready', at, set, difficulty, index: 0, selected: null, answer: { status: 'open' } })
    },
    [show],
  )

  const open = useCallback(
    (tour: SavedTour, stopIndex: number) => {
      const stop = tour.plan.stops[stopIndex]
      if (!snapshot || !stop) return
      const at: TourContext = { tourId: tour.plan.id, tourRevision: tour.plan.revision, stopIndex, nodeId: stop.nodeId }
      const id = ++searchId.current
      const current = () => id === searchId.current
      moved.current = false
      setState({ status: 'searching', at })
      let phase: SetupPhase = 'generating'
      const setup = (next: SetupPhase, error: string | null = null) => {
        phase = next
        setState({ status: 'setup', at, phase, error })
      }
      const find = async () => {
        const [sets, hash] = await Promise.all([library.list(), snapshotHash(snapshot)])
        return pickQuiz(sets, at, hash)
      }
      const make = async () => {
        const found = await find()
        if (!current()) return
        if (found) return ready(at, found)
        if (!library.generate || !dbPath) return setup('generating', 'There is no quiz for this step yet.')
        const agent = (await library.agentStatus?.()) ?? { installed: true, signedIn: true }
        if (!current()) return
        if (!agent.installed) return setup('not-installed')
        if (!agent.signedIn && library.signIn) {
          setup('signing-in')
          await library.signIn()
          if (!current()) return
        }
        setup('generating')
        await library.generate({ ...at, dbPath })
        const saved = await find()
        if (!current()) return
        if (!saved) throw new Error('Generation finished without saving a quiz.')
        ready(at, saved)
      }
      make().catch((e: unknown) => {
        if (current()) setup(phase, message(e))
      })
    },
    [snapshot, dbPath, library, ready, setState],
  )

  const close = useCallback(() => {
    searchId.current++
    if (state.status !== 'off' && moved.current) graphRef.current?.spotlight(state.at.nodeId)
    moved.current = false
    setState(OFF)
  }, [state, graphRef, setState])

  const setDifficulty = useCallback(
    (difficulty: Difficulty) => {
      if (state.status !== 'ready' || state.answer.status === 'submitting') return
      show({ ...state, difficulty, index: 0, selected: null, answer: { status: 'open' } })
    },
    [state, show],
  )

  const select = useCallback(
    (choice: number) => {
      if (state.status !== 'ready' || state.answer.status === 'answered' || state.answer.status === 'submitting') return
      setState({ ...state, selected: choice, answer: { status: 'open' } })
    },
    [state, setState],
  )

  const submit = useCallback(() => {
    if (state.status !== 'ready' || state.selected === null || state.answer.status === 'answered' || state.answer.status === 'submitting') return
    const question = questionsOf(state.set, state.difficulty)[state.index]
    if (!question) return
    const id = searchId.current
    const asked = state
    setState({ ...asked, answer: { status: 'submitting' } })
    library
      .respond({
        quizSetId: asked.set.id,
        quizId: question.id,
        selectedIndex: asked.selected!,
        responseTimeMs: Math.round(performance.now() - shownAt.current),
      })
      .then(
        ({ feedback }) => {
          if (id === searchId.current) setState({ ...asked, answer: { status: 'answered', feedback } })
        },
        (e: unknown) => {
          if (id === searchId.current) setState({ ...asked, answer: { status: 'error', message: message(e) } })
        },
      )
  }, [state, library, setState])

  const retry = useCallback(() => {
    if (state.status !== 'ready') return
    show({ ...state, selected: null, answer: { status: 'open' } })
  }, [state, show])

  const next = useCallback(() => {
    if (state.status !== 'ready') return
    const count = questionsOf(state.set, state.difficulty).length
    show({ ...state, index: (state.index + 1) % count, selected: null, answer: { status: 'open' } })
  }, [state, show])

  const showNode = useCallback(
    (nodeId: string) => {
      moved.current = true
      graphRef.current?.spotlight(nodeId)
    },
    [graphRef],
  )

  return { state, open, close, setDifficulty, select, submit, retry, next, showNode }
}

const message = (e: unknown) => (e instanceof Error ? e.message : String(e))
