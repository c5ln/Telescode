// @vitest-environment jsdom

import { cleanup, render, screen, within } from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import { afterEach, describe, expect, it, vi } from 'vitest'

import { TelescodeError, type AnalysisSnapshot, type TelescodeApi } from '../bridge'
import { AppShell } from './AppShell'

afterEach(cleanup)

const snapshot: AnalysisSnapshot = {
  schemaVersion: 1,
  dbPath: 'C:/x/project.db',
  totals: {
    fileCount: 16,
    classCount: 11,
    classEdgeCount: 1,
    fileNodeCount: 16,
    fileEdgeCount: 18,
    funcNodeCount: 198,
    funcEdgeCount: 219,
    sequenceCount: 74,
  },
  files: [],
  fileGraph: { nodes: [], edges: [] },
  classes: [],
  classEdges: [],
  readingSequence: [],
}

/** An API whose analyze() calls stay pending until the test settles them. */
function deferredApi() {
  const pending: { resolve: (s: AnalysisSnapshot) => void; reject: (e: unknown) => void }[] = []
  const analyze = vi.fn(
    () => new Promise<AnalysisSnapshot>((resolve, reject) => pending.push({ resolve, reject })),
  )
  const api: TelescodeApi = { analyze, graph: vi.fn(), sequence: vi.fn() }
  return { api, analyze, pending }
}

async function openDatabase(path: string) {
  await userEvent.type(screen.getByRole('textbox', { name: 'Database path' }), path)
  await userEvent.click(screen.getByRole('button', { name: 'Open' }))
}

const canvasButtons = () =>
  within(screen.getByRole('toolbar', { name: 'Canvas' })).getAllByRole('button') as HTMLButtonElement[]

describe('AppShell', () => {
  it('has only breadcrumbs, search and an overflow menu in the top bar, and no sidebar', () => {
    render(<AppShell api={deferredApi().api} />)
    const bar = screen.getByRole('banner')

    expect(within(bar).getByRole('navigation', { name: 'Breadcrumb' })).toBeTruthy()
    expect(within(bar).getByRole('search')).toBeTruthy()
    expect(within(bar).getByPlaceholderText('Search files, classes, functions…')).toBeTruthy()
    expect(within(bar).getAllByRole('button').map((b) => b.getAttribute('aria-label'))).toEqual(['More actions'])
    expect(screen.queryByRole('complementary')).toBeNull()
  })

  it('starts empty, then goes through loading to ready', async () => {
    const { api, analyze, pending } = deferredApi()
    render(<AppShell api={api} />)

    expect(screen.getByText('No database open')).toBeTruthy()
    expect(canvasButtons().every((b) => b.disabled)).toBe(true)

    await openDatabase('C:/x/project.db')
    expect(analyze).toHaveBeenCalledWith('C:/x/project.db')
    expect(screen.getByRole('status').textContent).toBe('Analyzing project.db…')
    expect(screen.getByRole('main', { name: 'Workspace' }).getAttribute('aria-busy')).toBe('true')

    pending[0].resolve(snapshot)
    expect(await screen.findByText('16 files · 11 classes')).toBeTruthy()
    expect(screen.getByText('project.db').getAttribute('aria-current')).toBe('location')
    expect(canvasButtons().every((b) => !b.disabled)).toBe(true)
  })

  it('shows a concise bridge error and can retry', async () => {
    const { api, analyze, pending } = deferredApi()
    render(<AppShell api={api} />)
    await openDatabase('C:/x/missing.db')

    pending[0].reject(new TelescodeError('db_not_found', 'No database at C:/x/missing.db'))
    const alert = await screen.findByRole('alert')
    expect(within(alert).getByText('Database not found')).toBeTruthy()
    expect(within(alert).getByText('No database at C:/x/missing.db')).toBeTruthy()
    expect(within(alert).getByText('db_not_found')).toBeTruthy()

    await userEvent.click(screen.getByRole('button', { name: 'Try again' }))
    expect(analyze).toHaveBeenCalledTimes(2)
    expect(screen.getByRole('status').textContent).toBe('Analyzing missing.db…')
  })

  it('returns to the empty state from the overflow menu, keeping the last path', async () => {
    const { api, pending } = deferredApi()
    render(<AppShell api={api} />)
    await openDatabase('C:/x/project.db')
    pending[0].resolve(snapshot)
    await screen.findByText('16 files · 11 classes')

    await userEvent.click(screen.getByRole('button', { name: 'More actions' }))
    await userEvent.click(screen.getByRole('menuitem', { name: 'Open database…' }))

    expect(screen.getByText('No database open')).toBeTruthy()
    expect((screen.getByRole('textbox', { name: 'Database path' }) as HTMLInputElement).value).toBe('C:/x/project.db')
  })

  it('ignores a slower, superseded request', async () => {
    const { api, pending } = deferredApi()
    render(<AppShell api={api} />)
    await openDatabase('C:/x/old.db')

    await userEvent.click(screen.getByRole('button', { name: 'More actions' }))
    await userEvent.click(screen.getByRole('menuitem', { name: 'Open database…' }))
    await userEvent.clear(screen.getByRole('textbox', { name: 'Database path' }))
    await openDatabase('C:/x/project.db')

    pending[1].resolve(snapshot)
    await screen.findByText('16 files · 11 classes')
    pending[0].reject(new TelescodeError('db_invalid', 'late failure'))
    await new Promise((r) => setTimeout(r, 0))
    expect(screen.queryByRole('alert')).toBeNull()
  })

  it('navigates back up the breadcrumb path', async () => {
    render(
      <AppShell
        api={deferredApi().api}
        initialState={{ status: 'ready', dbPath: 'C:/x/project.db', snapshot }}
        initialPath={['src', 'core', 'dependency.cpp'].map((label) => ({ id: label, label }))}
      />,
    )
    const nav = screen.getByRole('navigation', { name: 'Breadcrumb' })
    expect(nav.textContent).toBe('project.dbsrccoredependency.cpp')

    await userEvent.click(within(nav).getByRole('button', { name: 'src' }))
    expect(nav.textContent).toBe('project.dbsrc')
    expect(within(nav).getByText('src').getAttribute('aria-current')).toBe('location')
  })
})
