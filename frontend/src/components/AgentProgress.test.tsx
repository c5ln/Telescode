// @vitest-environment jsdom

import { cleanup, render, screen, within } from '@testing-library/react'
import { afterEach, describe, expect, it, vi } from 'vitest'

import userEvent from '@testing-library/user-event'

import { addCall, followRun, newRuns, readNodes, recordDuration, startActivity, stoppable, whileRunning, type ToolCall } from '../agent'
import { AgentError } from './AgentError'
import { AgentProgress, usualNote } from './AgentProgress'

afterEach(cleanup)

const call = (tool: string, nodeId: string | null = null): ToolCall => ({ tool, nodeId })

describe('AgentProgress', () => {
  it('follows the tools the agent calls, with the file it is reading', () => {
    let activity = startActivity(0)
    const view = () => render(<AgentProgress work="tour" agent="Claude Code" activity={activity} />)
    const states = () => within(screen.getByRole('list', { name: 'Steps' })).getAllByRole('listitem').map((li) => li.getAttribute('data-state'))

    view()
    expect(screen.getByRole('status').textContent).toBe('Start Claude Code…')
    cleanup()

    for (const c of [call('open_project'), call('get_reading_tour_order'), call('inspect_node', 'file:src/requests/sessions.py'), call('inspect_node', 'class:src/requests/models.py::Response')]) {
      activity = addCall(activity, c, 1000)
    }
    view()
    expect(screen.getByRole('status').textContent).toBe('Read the files…')
    expect(states()).toEqual(['done', 'done', 'done', 'now', 'pending', 'pending'])
    expect(screen.getByText('2 parts of the code read')).toBeTruthy()
    expect(screen.getByText('src/requests/models.py › Response')).toBeTruthy()
    expect(screen.getByText(/can take a few minutes/)).toBeTruthy()
  })

  it('treats a later step as reached even if the agent skipped one', () => {
    const activity = addCall(startActivity(0), call('create_quiz'), 10)
    render(<AgentProgress work="quiz" agent="Codex" activity={activity} />)
    expect(screen.getByRole('status').textContent).toBe('Write the questions…')
  })
})

describe('stopping and timing a run', () => {
  it('offers Cancel only for a run that can be stopped', async () => {
    const cancel = vi.fn()
    render(<AgentProgress work="quiz" agent="Claude Code" activity={startActivity(0)} />)
    expect(screen.queryByRole('button', { name: 'Cancel' })).toBeNull()
    cleanup()
    render(<AgentProgress work="quiz" agent="Claude Code" activity={{ ...startActivity(0), cancel }} />)
    await userEvent.click(screen.getByRole('button', { name: 'Cancel' }))
    expect(cancel).toHaveBeenCalledTimes(1)
  })

  it('says how long the recent runs took here, once there are any', () => {
    localStorage.clear()
    expect(usualNote('quiz')).toBe('Writing a quiz can take a minute or two.')
    recordDuration('quiz', 70_000)
    expect(usualNote('quiz')).toBe('A quiz usually takes about 1:10 here.')
    recordDuration('quiz', 185_000)
    expect(usualNote('quiz')).toBe('A quiz usually takes 1:10 to 3:05 here.')
    expect(usualNote('tour', { min: 40_000, max: 45_000 })).toBe('A tour usually takes about 43 s here. You can keep exploring the map meanwhile.')
    expect(usualNote('next question', { min: 20_000, max: 50_000 })).toBe('New questions usually take 20 s to 50 s here.')
    localStorage.clear()
  })

  it('tells a stop by the user apart from a failure', () => {
    render(<AgentError what="tour" message="Claude Code was stopped before it finished." provider="claude" onRetry={() => {}} />)
    expect(screen.getByText('You stopped Claude Code before it saved the tour.')).toBeTruthy()
  })

  it('keeps a run stoppable while the app tracks it', async () => {
    const cancel = vi.fn()
    const done = vi.fn()
    const tracked = whileRunning(stoppable(Promise.resolve(7), cancel), done)
    tracked.cancel?.()
    expect(cancel).toHaveBeenCalledTimes(1)
    expect(await tracked).toBe(7)
    expect(done).toHaveBeenCalledTimes(1)
  })

  it('outlines each part of the code the agent read once, the latest last', () => {
    let activity = startActivity(0)
    for (const c of [call('open_project'), call('inspect_node', 'file:a.py'), call('inspect_node', 'file:b.py'), call('inspect_node', 'file:a.py')]) activity = addCall(activity, c)
    expect(readNodes(activity)).toEqual(['file:b.py', 'file:a.py'])
    expect(readNodes(undefined)).toEqual([])
  })
})

describe('followRun', () => {
  it('runs once per key for every caller, and forgets the run once it ends', async () => {
    const runs = newRuns()
    let finish!: () => void
    const start = vi.fn(() => new Promise<void>((done) => (finish = done)))
    const first: ToolCall[] = []
    const one = followRun(runs, 'k', start, (c) => first.push(c))
    const two = followRun(runs, 'k', start)
    await vi.waitFor(() => expect(start).toHaveBeenCalled())
    finish()
    await Promise.all([one, two])
    expect(start).toHaveBeenCalledTimes(1)
    expect(runs.size).toBe(0)
    // Every caller of a run can stop it.
    expect(one.cancel).toBeTypeOf('function')
    expect(two.cancel).toBe(one.cancel)
  })
})
