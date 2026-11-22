// @vitest-environment jsdom

import { act, cleanup, render, screen, within } from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import { afterEach, describe, expect, it } from 'vitest'

import { AppShell } from '../app/AppShell'
import type { OpenProgress, RepositorySource, TelescodeApi } from '../bridge'
import { duration, estimate } from './LoadingProgress'

afterEach(cleanup)

describe('opening a repository', () => {
  it('shows each step as the shell reports it, with what it has learned so far', async () => {
    let report: (p: OpenProgress) => void = () => {}
    const repositories = (_: RepositorySource, onProgress?: (p: OpenProgress) => void) => {
      report = onProgress!
      return new Promise<string>(() => {})
    }
    const api = { analyze: () => new Promise(() => {}) } as unknown as TelescodeApi
    render(<AppShell api={api} repositories={repositories} />)
    await userEvent.click(screen.getByRole('button', { name: 'Open psf/requests' }))

    expect(screen.getByRole('heading', { name: 'psf/requests' })).toBeTruthy()
    expect(screen.getByRole('status').textContent).toBe('Download…')
    expect(screen.getByText('Most repositories open in a few seconds.')).toBeTruthy()

    act(() => report({ stage: 'download', bytes: 3 * 1024 * 1024 }))
    expect(screen.getByText('3.0 MB downloaded')).toBeTruthy()

    act(() => report({ stage: 'read', pythonFiles: 37 }))
    expect(screen.getByRole('status').textContent).toBe('Read the code…')
    expect(screen.getByText('37 Python files, 3.0 MB downloaded')).toBeTruthy()
    expect(screen.getByText('Repositories with 37 Python files usually open in a few seconds.')).toBeTruthy()
    const steps = within(screen.getByRole('list', { name: 'Steps' })).getAllByRole('listitem')
    expect(steps.map((s) => s.getAttribute('data-state'))).toEqual(['done', 'now', 'pending', 'pending'])

    act(() => report({ stage: 'rank' }))
    expect(screen.getByRole('status').textContent).toBe('Rank files…')

    await userEvent.click(screen.getByRole('button', { name: 'Cancel' }))
    expect(screen.getByText('See a Python repository as a map')).toBeTruthy()
    expect(within(document.body).queryByRole('status')).toBeNull()
  })
})

describe('estimate', () => {
  it('reads from the Python file count, once it is known', () => {
    expect(estimate(null)).toBe('Most repositories open in a few seconds.')
    expect(estimate(1)).toBe('Repositories with 1 Python file usually open in a few seconds.')
    expect(estimate(1233)).toBe('Repositories with 1,233 Python files usually open in 5 to 15 seconds.')
    expect(estimate(2943)).toBe('Repositories with 2,943 Python files can take up to a minute.')
  })

  it('writes short waits in seconds and long ones in minutes', () => {
    expect(duration(420)).toBe('0.4 s')
    expect(duration(65_000)).toBe('1:05')
  })
})
