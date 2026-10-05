// Tutorial mode's lifecycle: find the saved tour for the open repository and
// play it on the map, or, when there is none yet, have one generated and
// wait for it. The map itself is only steered through the graph handle, so leaving the
// tutorial puts back the view the user had.

import { useCallback, useEffect, useRef, useState, type RefObject } from 'react'

import type { AnalysisSnapshot } from '../bridge'
import type { GraphHandle, MapView } from '../graph/GraphCanvas'
import { buildGraphModel } from '../graph/model'
import { pickTutorial, snapshotHash, stepDurationMs, type SavedTour, type TourLibrary } from '../tour/tutorial'

export type TutorialState =
  | { status: 'off' }
  /** Looking for a saved tour after the user asked for the tutorial. */
  | { status: 'searching' }
  /**
   * No tour yet, so one is made: Claude Code is checked, signed in if needed
   * (`signing-in`), then writes it (`generating`). `ready` once it is saved,
   * `error` if a step failed.
   */
  | { status: 'setup'; phase: SetupPhase; ready: SavedTour | null; error: string | null }
  /** `finished` once playback has run through the last step. */
  | { status: 'playing'; tour: SavedTour; index: number; playing: boolean; finished: boolean }

export type SetupPhase = 'generating' | 'signing-in' | 'not-installed'

export interface Tutorial {
  state: TutorialState
  /** Play the saved tour, or generate one when there is none. */
  start: () => void
  /** Play a tour found during the generation flow. */
  play: (tour: SavedTour) => void
  goTo: (index: number) => void
  togglePlaying: () => void
  /** Leave the tutorial and return to the view from before it. */
  exit: () => void
}

/** How often the generation flow checks whether the tour has been saved. */
export const POLL_MS = 3000

const OFF: TutorialState = { status: 'off' }

export function useTutorial(
  library: TourLibrary,
  snapshot: AnalysisSnapshot | null,
  dbPath: string | null,
  graphRef: RefObject<GraphHandle | null>,
): Tutorial {
  // State belongs to one snapshot: opening or reloading a repository ends it.
  const [owned, setOwned] = useState<{ snapshot: AnalysisSnapshot | null; state: TutorialState }>({
    snapshot,
    state: OFF,
  })
  const state = owned.snapshot === snapshot ? owned.state : OFF
  const setState = useCallback((next: TutorialState) => setOwned({ snapshot, state: next }), [snapshot])

  const savedView = useRef<MapView | null>(null)
  // Time left on a paused step, so resuming continues it instead of starting it over.
  const remaining = useRef<{ index: number; ms: number } | null>(null)
  // Answers to an earlier search are dropped once the tutorial moves on.
  const searchId = useRef(0)

  const find = useCallback(async (): Promise<SavedTour | null> => {
    if (!snapshot) return null
    const [tours, hash] = await Promise.all([library.list(), snapshotHash(snapshot)])
    const model = buildGraphModel(snapshot, '')
    const onMap = (id: string) => {
      const kind = model.byId.get(id)?.kind
      return kind !== undefined && kind !== 'member'
    }
    return pickTutorial(tours, onMap, hash)
  }, [library, snapshot])

  const play = useCallback(
    (tour: SavedTour) => {
      searchId.current++
      savedView.current = graphRef.current?.saveView() ?? null
      remaining.current = null
      setState({ status: 'playing', tour, index: 0, playing: true, finished: false })
    },
    [graphRef, setState],
  )

  const start = useCallback(() => {
    if (!snapshot) return
    const id = ++searchId.current
    setState({ status: 'searching' })
    const current = () => id === searchId.current
    // The step under way, which a failure is reported against.
    let phase: SetupPhase = 'generating'
    const setup = (next: SetupPhase, error: string | null = null) => {
      phase = next
      setState({ status: 'setup', phase, ready: null, error })
    }
    const make = async () => {
      const tour = await find()
      if (!current()) return
      if (tour) return play(tour)
      if (!library.generate || !dbPath) return setup('generating')
      // Claude Code is only looked at now, when a tutorial has to be written.
      const agent = (await library.agentStatus?.()) ?? { installed: true, signedIn: true }
      if (!current()) return
      if (!agent.installed) return setup('not-installed')
      if (!agent.signedIn && library.signIn) {
        setup('signing-in')
        await library.signIn()
        if (!current()) return
      }
      setup('generating')
      await library.generate(dbPath)
      // Once generation is done the tour is saved; offer it (polling may already have).
      const saved = await find()
      if (!current()) return
      if (!saved) throw new Error('Generation finished without saving a tutorial.')
      setState({ status: 'setup', phase: 'generating', ready: saved, error: null })
    }
    make().catch((e: unknown) => {
      if (current()) setup(phase, message(e))
    })
  }, [snapshot, dbPath, library, find, play, setState])

  const exit = useCallback(() => {
    searchId.current++
    if (state.status === 'playing' && savedView.current) graphRef.current?.restoreView(savedView.current)
    savedView.current = null
    setState(OFF)
  }, [state.status, graphRef, setState])

  const goTo = useCallback(
    (index: number) => {
      if (state.status !== 'playing') return
      const last = state.tour.plan.stops.length - 1
      remaining.current = null
      // Moving on from the last step finishes the tour: playback stops rather than looping.
      if (index > last) setState({ ...state, playing: false, finished: true })
      else setState({ ...state, index: Math.max(0, index), finished: false })
    },
    [state, setState],
  )

  const togglePlaying = useCallback(() => {
    if (state.status !== 'playing') return
    // Play after the end starts over.
    if (state.finished) {
      remaining.current = null
      setState({ ...state, index: 0, playing: true, finished: false })
    }
    else setState({ ...state, playing: !state.playing })
  }, [state, setState])

  // Move the map to the current step.
  const index = state.status === 'playing' ? state.index : -1
  const step = state.status === 'playing' ? state.tour.plan.stops[index] : undefined
  useEffect(() => {
    if (step) graphRef.current?.spotlight(step.nodeId)
  }, [step, graphRef])

  // Automatic playback advances after the step's time in the tour.
  const autoplay = state.status === 'playing' && state.playing
  useEffect(() => {
    if (!autoplay || !step) return
    const ms = remaining.current?.index === index ? remaining.current.ms : stepDurationMs(step)
    const started = performance.now()
    const timer = setTimeout(() => goTo(index + 1), ms)
    return () => {
      clearTimeout(timer)
      remaining.current = { index, ms: Math.max(0, ms - (performance.now() - started)) }
    }
  }, [autoplay, step, index, goTo])

  // The generation flow: check for the tour until it appears or generation fails.
  const waiting = state.status === 'setup' && state.phase === 'generating' && !state.ready && !state.error
  useEffect(() => {
    if (!waiting) return
    const id = searchId.current
    const timer = setInterval(() => {
      find().then(
        (tour) => {
          if (id === searchId.current && tour) setState({ status: 'setup', phase: 'generating', ready: tour, error: null })
        },
        (e: unknown) => {
          if (id === searchId.current) setState({ status: 'setup', phase: 'generating', ready: null, error: message(e) })
        },
      )
    }, POLL_MS)
    return () => clearInterval(timer)
  }, [waiting, find, setState])

  return { state, start, play, goTo, togglePlaying, exit }
}

const message = (e: unknown) => (e instanceof Error ? e.message : String(e))
