// @vitest-environment jsdom
/// <reference types="node" />
import { createHash } from 'node:crypto'
import { act, cleanup, render, screen, within } from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import { afterEach, beforeEach, expect, it, vi } from 'vitest'
import { AppShell } from './AppShell'
import type { AnalysisSnapshot, GraphResponse, TelescodeApi } from '../bridge'
import sherlock from '../graph/fixtures/sherlock.graph.json'
import type { LearningView } from '../learning/contracts'
import type { GraphRenderer } from '../graph/renderer'
import type { SavedTour } from '../tour/tutorial'

const dbPath = '/repo/project.db'
const snapshot: AnalysisSnapshot = { ...(sherlock as GraphResponse), dbPath, readingSequence: [] }
const analysisHash = createHash('sha256').update(JSON.stringify(snapshot)).digest('hex')
const ready = { status: 'ready' as const, dbPath, snapshot }
const nodeA = 'file:sherlock_project/notify.py', nodeB = 'file:sherlock_project/result.py'
const tour: SavedTour = { plan: { schemaVersion: 1, id: 'learning-tour', revision: 1, title: 'Onboarding', language: 'en', snapshotHash: 'source',
  stops: [{ nodeId: nodeA, title: 'Overview', caption: 'Understand how the application works.', evidence: [], transitionMs: 0, holdMs: 60000 }] },
  snapshotHash: 'source', updatedMs: 1, state: 'draft' }
function view(nodeId = nodeA): LearningView {
  return { session: { id: 'session', revision: 2, learnerId: 'local',
    goal: { goalId: 'onboarding', goalRevision: 1, rawQuery: 'Understand notification flow', retrievalQuery: 'notification result update',
      repositoryId: '/repo', sourceSnapshotHash: analysisHash, targetConcepts: [{ conceptId: 'A', level: 'easy', weight: 1 }], budget: 12 },
    spent: 0, status: 'active', current: { id: nodeId === nodeA ? 'stop-A' : 'stop-B', actionId: 'read', nodeId, conceptId: 'A', level: 'easy', kind: 'read', caption: 'Follow the result update.' },
    history: [], policyVersion: 'bounded-v1', modelVersion: 'assumed-model-v1' },
    knowledge: [{ conceptId: 'A', level: 'easy', probability: .3, observations: 0 }],
    concepts: [{ id: 'A', name: 'Notification state', definition: 'Explain the notification outcome flow', version: 1 }],
    decision: { actionId: 'review-B', value: .8, expectedGain: .5, nodes: 3, truncated: false },
    question: { id: 'q1', question: 'Why preserve the result before notifying?', choices: ['To inspect the outcome', 'To restart the request', 'To configure a site'],
      difficulty: 'easy', concept: 'Notification state', evidence: [{ nodeId: nodeA, title: 'notify.py' }] }, score: .3,
    reason: 'Understanding estimates use a provisional model.' }
}
function library() {
  const active = view()
  const preparing = structuredClone(active); preparing.session.status = 'preparing'; preparing.session.current = null; preparing.question = null
  const afterAnswer = structuredClone(active); afterAnswer.session.revision = 3; afterAnswer.question = null
  afterAnswer.knowledge[0]!.observations = 1
  const next = view(nodeB); next.session.revision = 4; next.session.spent = 1; next.question!.id = 'q2'
  return { start: vi.fn(async (..._args: unknown[]) => preparing), prepare: vi.fn(async (..._args: unknown[]) => active), get: vi.fn(async (..._args: unknown[]) => active),
    respond: vi.fn(async (..._args: unknown[]) => ({ view: afterAnswer, responseId: 'response', counted: true,
      feedback: { correct: false, answer: 'To inspect the outcome', explanation: 'The receiver uses the stored result.', evidence: [{ nodeId: nodeA, title: 'notify.py' }], attempt: 1 } })),
    advance: vi.fn(async (..._args: unknown[]) => next) }
}
const renderer = () => (window as { __telescode?: GraphRenderer }).__telescode!
beforeEach(() => localStorage.clear())
afterEach(cleanup)
async function begin(learning: ReturnType<typeof library>, api?: TelescodeApi) {
  const rendered = render(<AppShell api={api} initialState={ready} tours={{ list: async () => [tour] }} learning={learning} />)
  renderer().resize(1200, 750)
  await userEvent.click(screen.getByRole('button', { name: 'Tutorial' }))
  const tutorial = await screen.findByRole('region', { name: 'Tutorial' })
  await within(tutorial).findByText('Understand how the application works.')
  await userEvent.click(within(tutorial).getByRole('button', { name: 'Learn adaptively' }))
  return rendered
}

it('starts with an explicit goal, prepares once and moves code only when the user advances', async () => {
  const learning = library()
  await begin(learning)
  const panel = await screen.findByRole('region', { name: 'Adaptive learning' })
  await within(panel).findByText('Why preserve the result before notifying?')
  expect(learning.start).toHaveBeenCalledTimes(1)
  const input = learning.start.mock.calls[0]![0] as unknown as { dbPath: string; goal: { rawQuery: string; retrievalQuery: string; targetConcepts: unknown[] } }
  expect(input.dbPath).toBe(dbPath)
  expect(input.goal.rawQuery.trim().length).toBeGreaterThan(0)
  expect(input.goal.retrievalQuery.trim().length).toBeGreaterThan(0)
  expect(input.goal.targetConcepts.length).toBeGreaterThan(0)
  expect(learning.prepare).toHaveBeenCalledTimes(1)
  expect(renderer().selected?.id).toBe(nodeA)
  expect(within(panel).queryByText('The receiver uses the stored result.')).toBeNull()
  await userEvent.click(within(panel).getByRole('radio', { name: 'To restart the request' }))
  await userEvent.click(within(panel).getByRole('button', { name: 'Check answer' }))
  await within(panel).findByText('The receiver uses the stored result.')
  expect(renderer().selected?.id).toBe(nodeA)
  expect(learning.advance).not.toHaveBeenCalled()
  await userEvent.click(within(panel).getByRole('button', { name: 'Continue learning' }))
  expect(learning.advance).toHaveBeenCalledWith(expect.objectContaining({ sessionId: 'session', revision: 3, stopId: 'stop-A', outcome: 'continue' }))
  expect(renderer().selected?.id).toBe(nodeB)
})

it('retries transport failure with the identical submission and selected answer', async () => {
  const learning = library()
  learning.respond.mockRejectedValueOnce(new Error('Connection lost'))
  await begin(learning)
  const panel = await screen.findByRole('region', { name: 'Adaptive learning' })
  await within(panel).findByText('Why preserve the result before notifying?')
  await userEvent.click(within(panel).getByRole('radio', { name: 'To restart the request' }))
  await userEvent.click(within(panel).getByRole('button', { name: 'Check answer' }))
  await within(panel).findByText(/Connection lost/)
  await userEvent.click(within(panel).getByRole('button', { name: 'Check answer' }))
  await within(panel).findByText('The receiver uses the stored result.')
  expect(learning.respond.mock.calls).toHaveLength(2)
  expect(learning.respond.mock.calls[1]).toEqual(learning.respond.mock.calls[0])
})

it('drops preparation that finishes after the workspace changes', async () => {
  const learning = library()
  let finish!: (result: LearningView) => void
  learning.prepare.mockImplementationOnce(() => new Promise(resolve => { finish = resolve }))
  const api: TelescodeApi = { analyze: vi.fn(async () => ({ ...snapshot, dbPath: '/other.db' })), graph: vi.fn(), sequence: vi.fn() }
  await begin(learning, api)
  expect(learning.prepare).toHaveBeenCalledTimes(1)
  await userEvent.click(screen.getByRole('button', { name: 'More actions' }))
  await userEvent.click(screen.getByRole('menuitem', { name: 'Reload analysis' }))
  await act(async () => finish(view()))
  expect(screen.queryByRole('region', { name: 'Adaptive learning' })).toBeNull()
  await userEvent.click(screen.getByRole('button', { name: 'Tutorial' }))
  const nextTutorial = await screen.findByRole('region', { name: 'Tutorial' })
  await within(nextTutorial).findByText('Understand how the application works.')
  await userEvent.click(within(nextTutorial).getByRole('button', { name: 'Learn adaptively' }))
  await within(await screen.findByRole('region', { name: 'Adaptive learning' })).findByText('Why preserve the result before notifying?')
  expect(learning.start).toHaveBeenCalledTimes(2)
  expect(learning.prepare).toHaveBeenCalledTimes(2)
})

it('offers bounded supplemental preparation when independent material is exhausted', async () => {
  const learning = library()
  const exhausted = view(); exhausted.session.status = 'generation-needed'; exhausted.session.current = null; exhausted.question = null
  exhausted.session.spent = 2
  learning.prepare.mockResolvedValueOnce(exhausted)
  await begin(learning)
  const panel = await screen.findByRole('region', { name: 'Adaptive learning' })
  await within(panel).findByRole('button', { name: 'Prepare more material' })
  await userEvent.click(within(panel).getByRole('button', { name: 'Prepare more material' }))
  await within(panel).findByText('Why preserve the result before notifying?')
  expect(learning.prepare).toHaveBeenCalledTimes(2)
})

it('labels no-quiz acquisition unconfirmed and wires explicit completion, skip and end', async () => {
  const learning = library()
  const reading = view(); reading.question = null; reading.knowledge[0]!.probability = .72
  learning.prepare.mockResolvedValueOnce(reading)
  await begin(learning)
  const panel = await screen.findByRole('region', { name: 'Adaptive learning' })
  await within(panel).findByText(/unconfirmed/i)
  expect(within(panel).getByText(/0 independent answers/i)).toBeTruthy()
  await userEvent.click(within(panel).getByRole('button', { name: 'Continue learning' }))
  expect(learning.advance).toHaveBeenLastCalledWith(expect.objectContaining({ outcome: 'continue' }))
  await userEvent.click(within(panel).getByRole('button', { name: 'Skip concept' }))
  expect(learning.advance).toHaveBeenLastCalledWith(expect.objectContaining({ outcome: 'skip' }))
  const stopped = view(); stopped.session.status = 'stopped'; stopped.session.current = null; stopped.question = null
  learning.advance.mockResolvedValueOnce(stopped)
  await userEvent.click(within(panel).getByRole('button', { name: 'End session' }))
  expect(learning.advance).toHaveBeenLastCalledWith(expect.objectContaining({ outcome: 'stop' }))
  await within(panel).findByText(/session ended/i)
})

it('closing and reopening resumes the saved active session without resetting its budget', async () => {
  const learning = library()
  const resumed = view(); resumed.session.spent = 2
  learning.get.mockResolvedValueOnce(resumed)
  await begin(learning)
  const panel = await screen.findByRole('region', { name: 'Adaptive learning' })
  await within(panel).findByText('Why preserve the result before notifying?')
  await userEvent.click(within(panel).getByRole('button', { name: 'Back to tutorial' }))
  const tutorial = await screen.findByRole('region', { name: 'Tutorial' })
  await userEvent.click(within(tutorial).getByRole('button', { name: 'Learn adaptively' }))
  await within(await screen.findByRole('region', { name: 'Adaptive learning' })).findByText('Why preserve the result before notifying?')
  expect(learning.start).toHaveBeenCalledTimes(1)
  expect(learning.get).toHaveBeenCalledWith('session')
  expect(within(screen.getByRole('region', { name: 'Adaptive learning' })).getByText('2 / 12 steps used')).toBeTruthy()
})

it('can end during preparation and ignores the worker completion afterward', async () => {
  const learning = library()
  let finish!: (result: LearningView) => void
  learning.prepare.mockImplementationOnce(() => new Promise(resolve => { finish = resolve }))
  const stopped = view(); stopped.session.status = 'stopped'; stopped.session.current = null; stopped.question = null
  const newerPending = view(); newerPending.session.status = 'preparing'; newerPending.session.current = null; newerPending.session.revision = 3
  learning.get.mockResolvedValueOnce(newerPending)
  learning.advance.mockResolvedValueOnce(stopped)
  await begin(learning)
  const panel = await screen.findByRole('region', { name: 'Adaptive learning' })
  await userEvent.click(await within(panel).findByRole('button', { name: 'End session' }))
  expect(learning.advance).toHaveBeenCalledWith(expect.objectContaining({ sessionId: 'session', revision: 3, outcome: 'stop' }))
  await within(panel).findByText('Session ended.')
  await act(async () => finish(view()))
  expect(within(panel).queryByText('Why preserve the result before notifying?')).toBeNull()
})
