import { useLayoutEffect, useRef, useState, type RefObject } from 'react'
import type { AnalysisSnapshot } from '../bridge'
import type { GraphHandle, MapView } from '../graph/GraphCanvas'
import type { GoalSeed, LearningLibrary } from '../learning/api'
import type { LearningAnswer, LearningResult, LearningView } from '../learning/contracts'
import { snapshotHash } from '../tour/tutorial'
import { defaultGoal, requirementsGoal } from '../learning/goal'

type State = { status: 'off' } | { status: 'loading'; error?: string; pending?: LearningView } | { status: 'ready'; view: LearningView;
  question: LearningView['question']; selected: number | null; result: LearningResult | null; busy: boolean; error: string | null }
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
  useLayoutEffect(() => {
    const invalidate = () => { sequence.current++ }
    context.current = snapshot
    invalidate(); busy.current = false; ending.current = false; request.current = null; savedView.current = null; lastGoal.current = null
    return invalidate
  }, [snapshot])
  const setState = (state: State) => setOwned({ snapshot, state })
  const show = (view: LearningView, move = true) => {
    request.current = null; busy.current = false; ending.current = false
    setState({ status: 'ready', view, question: view.question, selected: null, result: null, busy: false, error: null })
    if (move && view.session.current) graph.current?.spotlight(view.session.current.nodeId)
  }
  const current = (id: number) => id === sequence.current && context.current === snapshot
  const start = async (input: GoalSeed | string = defaultGoal()) => {
    if (!snapshot || !dbPath || busy.current) return
    const id = ++sequence.current; busy.current = true
    savedView.current = graph.current?.saveView() ?? null
    lastGoal.current = input
    setState({ status: 'loading' })
    try {
      const goal = typeof input === 'string' ? await requirementsGoal(input) : input
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
      if (view.session.status === 'preparing') {
        setState({ status: 'loading', pending: view })
        view = await library.prepare({ sessionId: view.session.id, revision: view.session.revision })
      }
      if (current(id)) show(view)
    } catch (e) { if (current(id)) { busy.current = false; setState({ status: 'loading', error: message(e) }) } }
  }
  const close = () => {
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
    const id = sequence.current; busy.current = true
    setState({ ...state, busy: true, error: null })
    try {
      const view = await library.prepare({ sessionId: state.view.session.id, revision: state.view.session.revision })
      if (current(id)) show(view)
    } catch (e) { if (current(id)) { busy.current = false; setState({ ...state, busy: false, error: message(e) }) } }
  }
  return { state, start, close, retry, select, submit, advance, prepareMore, showNode: (nodeId: string) => graph.current?.spotlight(nodeId) }
}
export type Learning = ReturnType<typeof useLearning>
