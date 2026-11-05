// @vitest-environment jsdom

import { cleanup, render, screen, within } from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'

import type { AnalysisSnapshot, GraphResponse } from '../bridge'
import sherlock from '../graph/fixtures/sherlock.graph.json'
import type { LearningLibrary } from '../learning/api'
import type { LearningView } from '../learning/contracts'
import type { SavedTour } from '../tour/tutorial'
import { AppShell } from './AppShell'

const dbPath = '/repo/project.db'
const snapshot: AnalysisSnapshot = { ...(sherlock as GraphResponse), dbPath, readingSequence: [] }
const tour: SavedTour = {
  plan: {
    schemaVersion: 1, id: 'onboarding', revision: 1, title: 'Sherlock onboarding', language: 'en', snapshotHash: 'source',
    stops: [{
      nodeId: 'file:sherlock_project/notify.py', title: 'Overview',
      caption: 'Understand how Sherlock reports its results.', evidence: [], transitionMs: 0, holdMs: 60_000,
    }],
  },
  snapshotHash: 'source', state: 'draft', updatedMs: 1,
}

beforeEach(() => localStorage.clear())
afterEach(cleanup)

function setup() {
  // Leave generation pending so these tests observe only the start-screen handoff.
  const pending = new Promise<LearningView>(() => { /* Keep preparation in flight until unmount. */ })
  const learning = {
    start: vi.fn<LearningLibrary['start']>(() => pending),
    get: vi.fn<LearningLibrary['get']>(() => pending),
    prepare: vi.fn<LearningLibrary['prepare']>(() => pending),
    respond: vi.fn<LearningLibrary['respond']>(),
    advance: vi.fn<LearningLibrary['advance']>(() => pending),
  } satisfies LearningLibrary
  const tours = {
    list: vi.fn(async (): Promise<SavedTour[]> => [tour]),
    generate: vi.fn((_dbPath: string) => new Promise<void>(() => { /* Keep generation in flight until unmount. */ })),
    agentStatus: vi.fn(async () => ({ installed: true, signedIn: true })),
  }
  render(<AppShell initialState={{ status: 'ready', dbPath, snapshot }} tours={tours} learning={learning} />)
  const user = userEvent.setup()
  const open = async () => {
    await user.click(screen.getByRole('button', { name: 'Tutorial' }))
    return within(await screen.findByRole('region', { name: 'Start tutorial' }))
  }
  return { user, open, tours, learning }
}

describe('Tour start screen', () => {
  it('opens an optional requirements field before starting a tour, even when onboarding is saved', async () => {
    const { open, tours, learning } = setup()
    const panel = await open()
    const field = panel.getByRole('textbox', { name: /requirements/i }) as HTMLInputElement | HTMLTextAreaElement

    expect(field.value).toBe('')
    expect(field.required).toBe(false)
    expect((panel.getByRole('button', { name: 'Start tour' }) as HTMLButtonElement).disabled).toBe(false)
    expect(panel.getByText(/onboarding/i)).toBeTruthy()
    expect(screen.queryByText(tour.plan.stops[0]!.caption)).toBeNull()
    expect(tours.generate).not.toHaveBeenCalled()
    expect(learning.start).not.toHaveBeenCalled()
  })

  it.each(['', '   '])('starts saved onboarding when requirements are %j', async requirements => {
    const { user, open, tours, learning } = setup()
    const panel = await open()
    if (requirements) await user.type(panel.getByRole('textbox', { name: /requirements/i }), requirements)

    await user.click(panel.getByRole('button', { name: 'Start tour' }))

    expect(await screen.findByText(tour.plan.stops[0]!.caption)).toBeTruthy()
    expect(screen.queryByRole('region', { name: 'Start tutorial' })).toBeNull()
    expect(tours.generate).not.toHaveBeenCalled()
    expect(learning.start).not.toHaveBeenCalled()
  })

  it('submits entered requirements to adaptive learning instead of playing saved onboarding', async () => {
    const { user, open, tours, learning } = setup()
    const panel = await open()
    const requirements = '알림 결과를 저장하고 전달하는 흐름을 설명해줘.'
    await user.type(panel.getByRole('textbox', { name: /requirements/i }), requirements)
    expect(learning.start).not.toHaveBeenCalled()

    await user.click(panel.getByRole('button', { name: 'Start tour' }))

    await vi.waitFor(() => expect(learning.start).toHaveBeenCalledWith(expect.objectContaining({
      goal: expect.objectContaining({ rawQuery: requirements }),
    })))
    expect(screen.queryByText(tour.plan.stops[0]!.caption)).toBeNull()
    expect(screen.queryByRole('region', { name: 'Start tutorial' })).toBeNull()
    expect(tours.generate).not.toHaveBeenCalled()
  })

  it('allows keyboard activation of the start action', async () => {
    const { user, open, learning } = setup()
    const panel = await open()

    panel.getByRole('button', { name: 'Start tour' }).focus()
    await user.keyboard('{Enter}')

    expect(await screen.findByText(tour.plan.stops[0]!.caption)).toBeTruthy()
    expect(learning.start).not.toHaveBeenCalled()
  })

  it('dismisses with Escape from the field without starting a tour', async () => {
    const { user, open, tours, learning } = setup()
    const panel = await open()
    await user.type(panel.getByRole('textbox', { name: /requirements/i }), 'Explain authentication')

    await user.keyboard('{Escape}')

    expect(screen.queryByRole('region', { name: 'Start tutorial' })).toBeNull()
    expect(screen.queryByText(tour.plan.stops[0]!.caption)).toBeNull()
    expect(tours.generate).not.toHaveBeenCalled()
    expect(learning.start).not.toHaveBeenCalled()
  })

  it('allows the toolbar control to close the start screen before submission', async () => {
    const { user, open, tours, learning } = setup()
    await open()

    await user.click(within(screen.getByRole('banner')).getByRole('button', { name: 'Exit tutorial' }))

    expect(screen.queryByRole('region', { name: 'Start tutorial' })).toBeNull()
    expect(screen.getByRole('button', { name: 'Tutorial' })).toBeTruthy()
    expect(screen.queryByText(tour.plan.stops[0]!.caption)).toBeNull()
    expect(tours.generate).not.toHaveBeenCalled()
    expect(learning.start).not.toHaveBeenCalled()
  })
})
