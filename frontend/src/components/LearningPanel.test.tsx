// @vitest-environment jsdom

import { cleanup, render, screen } from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import { afterEach, describe, expect, it, vi } from 'vitest'

import type { Learning } from '../app/useLearning'
import type { LearningView } from '../learning/contracts'
import { LearningPanel } from './LearningPanel'

afterEach(cleanup)

const view = (goalId: string): LearningView => ({
  session: {
    id: 's', revision: 4, learnerId: 'local', spent: 6, status: 'generation-needed', current: null, history: [], policyVersion: 'p', modelVersion: 'm',
    goal: { goalId, goalRevision: 1, rawQuery: 'Show me sessions.', retrievalQuery: 'Show me sessions.', targetConcepts: [], budget: 12, repositoryId: 'r', sourceSnapshotHash: 'h' },
  },
  knowledge: [], concepts: [], decision: null, question: null, score: 0, reason: '', answers: [],
})

function learning(state: Learning['state'], prepared: string | null): Learning {
  return {
    state, start: vi.fn(), close: vi.fn(), retry: vi.fn(), select: vi.fn(), submit: vi.fn(), advance: vi.fn(), prepareMore: vi.fn(),
    isPrepared: (goalId: string) => goalId === prepared, showNode: vi.fn(),
  } as unknown as Learning
}

const ready = (v: LearningView): Learning['state'] => ({ status: 'ready', view: v, question: null, selected: null, result: null, busy: false, error: null })

describe('LearningPanel', () => {
  it("ends an example's path with End session once its prepared material is used up", async () => {
    const l = learning(ready(view('requirements:example')), 'requirements:example')
    render(<LearningPanel learning={l} />)
    expect(screen.getByText(/every question prepared for this example/)).toBeTruthy()
    expect(screen.getByText('6 steps done')).toBeTruthy()
    expect(screen.queryByRole('button', { name: 'Prepare more material' })).toBeNull()
    await userEvent.click(screen.getByRole('button', { name: 'End session' }))
    expect(l.advance).toHaveBeenCalledWith('stop')
  })

  it('sums up the answers when the path ends, with each goal concept in plain words', () => {
    const v = view('requirements:example')
    v.session.status = 'stopped'
    v.session.goal.targetConcepts = [{ conceptId: 'goal', level: 'easy', weight: 1 }, { conceptId: 'other', level: 'easy', weight: 1 }]
    v.concepts = [{ id: 'goal', name: 'Path of requests.get()', definition: 'd', version: 1 }, { id: 'other', name: 'Adapters', definition: 'd', version: 1 }]
    v.knowledge = [{ conceptId: 'goal', level: 'easy', probability: .95, observations: 3 }, { conceptId: 'other', level: 'easy', probability: .4, observations: 1 }]
    v.answers = [{ conceptId: 'goal', level: 'easy', asked: 3, correct: 3 }, { conceptId: 'other', level: 'easy', asked: 3, correct: 1 }]
    render(<LearningPanel learning={learning(ready(v), 'requirements:example')} />)
    expect(screen.getByText('You answered 4 of 6 questions correctly.')).toBeTruthy()
    const items = screen.getAllByRole('listitem').map((li) => li.textContent)
    expect(items).toEqual(['Path of requests.get(): Understood (3 of 3 answers about it correct)', 'Adapters: Not confirmed yet (1 of 3 answers about it correct)'])
    expect(screen.queryByText(/of up to 12/)).toBeNull()
  })

  it('offers more material, written by the agent, for any other goal', () => {
    render(<LearningPanel learning={learning(ready(view('requirements:other')), 'requirements:example')} />)
    expect(screen.getByRole('button', { name: 'Prepare more material' })).toBeTruthy()
  })

  it("does not blame the agent when an example's prepared path fails to open", async () => {
    const l = learning({ status: 'loading', error: 'MCP server stopped', prepared: true }, 'requirements:example')
    render(<LearningPanel learning={l} />)
    expect(screen.getByText('The learning path did not open')).toBeTruthy()
    expect(screen.queryByRole('button', { name: /Switch to/ })).toBeNull()
    await userEvent.click(screen.getByRole('button', { name: 'Try again' }))
    expect(l.retry).toHaveBeenCalled()
  })
})
