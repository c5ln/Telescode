import { useLayoutEffect, useRef, useState, type RefObject } from 'react'
import { addCall, recordDuration, startActivity, withRun, type AgentActivity, type AgentRun } from '../agent'
import type { AnalysisSnapshot } from '../bridge'
import type { GraphHandle, MapView } from '../graph/GraphCanvas'
import type { GoalSeed, LearningLibrary } from '../learning/api'
import type { LearningAnswer, LearningResult, LearningView } from '../learning/contracts'
import { snapshotHash } from '../tour/tutorial'
import { defaultGoal, requirementsGoal } from '../learning/goal'

type State = { status: 'off' } | { status: 'loading'; error?: string; pending?: LearningView; activity?: AgentActivity; prepared?: boolean } | { status: 'ready'; view: LearningView;
  question: LearningView['question']; selected: number | null; result: LearningResult | null; busy: boolean; error: string | null
  /** The questions ran out and the next ones are being written; what the agent has done so far. */
  waiting?: AgentActivity }
const message = (e: unknown) => e instanceof Error ? e.message : String(e)

export function useLearning(library: LearningLibrary, snapshot: AnalysisSnapshot | null, dbPath: string | null, graph: RefObject<GraphHandle | null>) {
  const [owned, setOwned] = useState<{ snapshot: AnalysisSnapshot | null; state: State }>({ snapshot, state: { status: 'off' } })
  const state: State = owned.snapshot === snapshot ? owned.state : { status: 'off' }
  const context = useRef(snapshot)
  const sequence = useRef(0), busy = useRef(false)
  const request = useRef<LearningAnswer | null>(null)
  const lastGoal = useRef<GoalSeed | string | null>(null)
  const ending = useRef(false)
  const savedView = useRef<MapView | null>(null)
  /** The background run writing what the next step may need, while the learner answers. */
  const material = useRef<{ run: AgentRun<LearningView>; activity: AgentActivity } | null>(null)
  /** While the learner waits for that run: the sequence they wait under and the view they wait on. */
  const waiting = useRef<{ id: number; view: LearningView } | null>(null)
  const stopMaterial = () => { material.current?.run.cancel?.(); material.current = null; waiting.current = null }
  useLayoutEffect(() => {
    const invalidate = () => { sequence.current++ }
    context.current = snapshot
    invalidate(); busy.current = false; ending.current = false; request.current = null; savedView.current = null; lastGoal.current = null
    return () => { invalidate(); material.current?.run.cancel?.(); material.current = null; waiting.current = null }
  }, [snapshot])
  const setState = (state: State) => setOwned({ snapshot, state })
  const show = (view: LearningView, move = true) => {
    request.current = null; busy.current = false; ending.current = false
    setState({ status: 'ready', view, question: view.question, selected: null, result: null, busy: false, error: null })
    if (move && view.session.current) graph.current?.spotlight(view.session.current.nodeId)
    // While a question is answered, what either answer leads to is written; once they run out, the learner waits for it.
    if (view.session.status === 'active' && view.question) writeMaterial(view)
    else if (view.session.status === 'generation-needed' && library.prepareMaterial && !isPrepared(view.session.goal.goalId)) awaitMaterial(view)
  }
  const failed = (view: LearningView, error: string) => {
    busy.current = false
    setState({ status: 'ready', view, question: view.question, selected: null, result: null, busy: false, error })
  }
  /** Starts the background run for `view`'s session, unless one is going or no agent writes for its goal. */
  const writeMaterial = (view: LearningView) => {
    if (!library.prepareMaterial || material.current || isPrepared(view.session.goal.goalId)) return
    let activity = startActivity()
    const run = library.prepareMaterial({ sessionId: view.session.id }, (call) => {
      activity = addCall(activity, call)
      if (material.current?.run === run) material.current.activity = activity
      const wait = waiting.current
      if (wait && current(wait.id)) setState({ status: 'ready', view: wait.view, question: null, selected: null, result: null, busy: false, error: null, waiting: activity })
    })
    activity = withRun(activity, run)
    material.current = { run, activity }
    run.then((next) => {
      if (material.current?.run === run) material.current = null
      const wait = waiting.current
      if (!wait || !current(wait.id)) return
      waiting.current = null
      if (next.session.status === 'active') { recordDuration('next question', Date.now() - activity.startedMs); show(next) }
      else failed(next, 'No new questions could be written for this step. Try again, or end the session.')
    }, (e: unknown) => {
      if (material.current?.run === run) material.current = null
      const wait = waiting.current
      if (!wait || !current(wait.id)) return
      waiting.current = null
      failed(wait.view, message(e))
    })
  }
  /** The learner has nothing left to answer: wait for the background run, starting one if none is going. */
  const awaitMaterial = (view: LearningView) => {
    waiting.current = { id: sequence.current, view }
    writeMaterial(view)
    if (!material.current) { waiting.current = null; return }
    busy.current = false
    setState({ status: 'ready', view, question: null, selected: null, result: null, busy: false, error: null, waiting: material.current.activity })
  }
  const current = (id: number) => id === sequence.current && context.current === snapshot
  /** Whether the goal's material ships with the app, so no agent writes more of it. */
  const isPrepared = (goalId: string) => goalId === library.preparedGoalId?.()
  const start = async (input: GoalSeed | string = defaultGoal()) => {
    if (!snapshot || !dbPath || busy.current) return
    stopMaterial()
    const id = ++sequence.current; busy.current = true
    savedView.current = graph.current?.saveView() ?? null
    lastGoal.current = input
    setState({ status: 'loading' })
    let goalId: string | undefined
    try {
      const goal = typeof input === 'string' ? await requirementsGoal(input) : input
      goalId = goal.goalId
      const hash = await snapshotHash(snapshot)
      if (!current(id)) return
      const key = `telescode.learning.${JSON.stringify([dbPath, hash, goal])}`
      let sessionId: string | null = null
      try { sessionId = localStorage.getItem(key) } catch { /* Optional local resume pointer. */ }
      let view: LearningView | null = null
      if (sessionId) try {
        const saved = await library.get(sessionId)
        if (saved.session.goal.sourceSnapshotHash === hash && ['active', 'preparing', 'generation-needed'].includes(saved.session.status)) view = saved
      } catch { /* Deleted sessions can be started again. */ }
      if (!current(id)) return
      if (!view) view = await library.start({ dbPath, goal })
      if (!current(id)) return
      try { localStorage.setItem(key, view.session.id) } catch { /* State is also persisted by the server. */ }
      // A session whose first preparation failed has no material at all: it is prepared again, not shown as used up.
      if (view.session.status === 'preparing' || (view.session.status === 'generation-needed' && !view.concepts.length)) {
        const pending = view
        let activity = startActivity()
        setState({ status: 'loading', pending, activity })
        const run = library.prepare({ sessionId: view.session.id, revision: view.session.revision }, (call) => {
          if (!current(id)) return
          activity = addCall(activity, call)
          setState({ status: 'loading', pending, activity })
        })
        activity = withRun(activity, run)
        if (current(id)) setState({ status: 'loading', pending, activity })
        view = await run
        // Only an agent's run says how long the next one takes; bundled material saves at once.
        if (activity.calls.length) recordDuration('learning path', Date.now() - activity.startedMs)
      }
      if (current(id)) show(view)
    } catch (e) { if (current(id)) { busy.current = false; setState({ status: 'loading', error: message(e), prepared: goalId !== undefined && isPrepared(goalId) }) } }
  }
  const close = () => {
    stopMaterial()
    sequence.current++; busy.current = false; request.current = null
    if (context.current === snapshot && savedView.current) graph.current?.restoreView(savedView.current)
    savedView.current = null; setState({ status: 'off' })
  }
  const retry = () => {
    if (state.status === 'loading' && state.error && lastGoal.current !== null) void start(lastGoal.current)
  }
  const select = (selected: number) => {
    if (state.status !== 'ready' || busy.current || state.result || request.current) return
    setState({ ...state, selected })
  }
  const submit = async () => {
    if (state.status !== 'ready' || busy.current || state.result || !state.question || state.selected === null || !state.view.session.current) return
    const id = sequence.current; busy.current = true
    request.current ??= { sessionId: state.view.session.id, revision: state.view.session.revision, stopId: state.view.session.current.id,
      submissionId: crypto.randomUUID(), questionId: state.question.id, selectedIndex: state.selected }
    setState({ ...state, busy: true, error: null })
    try {
      const result = await library.respond(request.current)
      if (current(id)) { busy.current = false; setState({ ...state, view: result.view, result, busy: false, error: null }) }
    } catch (e) { if (current(id)) { busy.current = false; setState({ ...state, busy: false, error: message(e) }) } }
  }
  const advance = async (outcome: 'continue' | 'skip' | 'stop') => {
    if (outcome === 'stop') {
      const view = state.status === 'ready' ? state.view : state.status === 'loading' ? state.pending : null
      if (!view || ending.current) return
      ending.current = true
      stopMaterial()
      const id = ++sequence.current
      if (state.status === 'ready') setState({ ...state, busy: true, error: null })
      else setState({ status: 'loading' })
      try {
        // Preparation may have incremented the revision while the CLI was running.
        const latest = await library.get(view.session.id)
        const ended = await library.advance({ sessionId: latest.session.id, revision: latest.session.revision, stopId: latest.session.current?.id ?? '', outcome: 'stop' })
        if (current(id)) show(ended)
      } catch (e) {
        if (current(id)) { ending.current = false; busy.current = false; setState({ status: 'loading', pending: view, error: message(e) }) }
      }
      return
    }
    if (state.status !== 'ready' || busy.current || !state.view.session.current) return
    const id = sequence.current; busy.current = true
    setState({ ...state, busy: true, error: null })
    try {
      const view = await library.advance({ sessionId: state.view.session.id, revision: state.view.session.revision, stopId: state.view.session.current.id, outcome })
      if (current(id)) show(view)
    } catch (e) { if (current(id)) { busy.current = false; setState({ ...state, busy: false, error: message(e) }) } }
  }
  const prepareMore = async () => {
    if (state.status !== 'ready' || busy.current) return
    // With background material, more is the next questions for this step, not a new learning path.
    if (library.prepareMaterial && !isPrepared(state.view.session.goal.goalId)) return awaitMaterial(state.view)
    const id = sequence.current; busy.current = true
    setState({ ...state, busy: true, error: null })
    try {
      const view = await library.prepare({ sessionId: state.view.session.id, revision: state.view.session.revision })
      if (current(id)) show(view)
    } catch (e) { if (current(id)) { busy.current = false; setState({ ...state, busy: false, error: message(e) }) } }
  }
  return { state, start, close, retry, select, submit, advance, prepareMore, isPrepared, showNode: (nodeId: string) => graph.current?.spotlight(nodeId) }
}
export type Learning = ReturnType<typeof useLearning>
