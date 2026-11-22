// @vitest-environment jsdom

import { cleanup, render, screen } from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import { afterEach, describe, expect, it, vi } from 'vitest'

import { TourStartPanel } from './TourStartPanel'

afterEach(cleanup)

describe('TourStartPanel', () => {
  it("says an edited example goal is written by the agent, and not for the example's own goal or a blank one", async () => {
    render(<TourStartPanel suggested="Show me sessions." onStart={vi.fn()} onClose={vi.fn()} />)
    const note = () => screen.queryByText(/written by your AI agent/)
    const input = screen.getByRole('textbox')
    expect(note()).toBeNull()
    await userEvent.type(input, ' And cookies.')
    expect(note()).toBeTruthy()
    await userEvent.clear(input)
    expect(note()).toBeNull()
  })

  it('says nothing about the agent for a repository without a suggested goal', async () => {
    render(<TourStartPanel onStart={vi.fn()} onClose={vi.fn()} />)
    await userEvent.type(screen.getByRole('textbox'), 'Show me the CLI.')
    expect(screen.queryByText(/written by your AI agent/)).toBeNull()
  })
})
