// @vitest-environment jsdom

import { cleanup, render, screen, within } from '@testing-library/react'
import { afterEach, describe, expect, it, vi } from 'vitest'

import { addCall, followRun, newRuns, startActivity, type ToolCall } from '../agent'
import { AgentProgress } from './AgentProgress'

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
  })
})
