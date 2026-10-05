// @vitest-environment jsdom

import { act, cleanup, render, screen, within } from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import { afterEach, describe, expect, it, vi } from 'vitest'

import { TelescodeError, type AnalysisSnapshot, type GraphResponse, type TelescodeApi } from '../bridge'
import sherlock from '../graph/fixtures/sherlock.graph.json'
import type { GraphRenderer } from '../graph/renderer'
import { AppShell } from './AppShell'

afterEach(cleanup)

// Real core output for the Sherlock sample (16 files, 11 classes).
const snapshot: AnalysisSnapshot = { ...(sherlock as GraphResponse), dbPath: 'C:/x/project.db', readingSequence: [] }

/** An API whose analyze() calls stay pending until the test settles them. */
function deferredApi() {
  const pending: { resolve: (s: AnalysisSnapshot) => void; reject: (e: unknown) => void }[] = []
  const analyze = vi.fn(
    () => new Promise<AnalysisSnapshot>((resolve, reject) => pending.push({ resolve, reject })),
  )
  const api: TelescodeApi = { analyze, graph: vi.fn(), sequence: vi.fn() }
  return { api, analyze, pending }
}

/** The development-only path: switch the empty state to database entry, then open. */
async function openDatabase(path: string) {
  const devLink = screen.queryByRole('button', { name: 'Open a local database (dev)' })
  if (devLink) await userEvent.click(devLink)
  await userEvent.type(screen.getByRole('textbox', { name: 'Database path' }), path)
  await userEvent.click(screen.getByRole('button', { name: 'Open' }))
}

const canvasButtons = () =>
  within(screen.getByRole('toolbar', { name: 'Canvas' })).getAllByRole('button') as HTMLButtonElement[]

describe('AppShell', () => {
  it('has only breadcrumbs, search and an overflow menu in the top bar, and no sidebar', () => {
    render(<AppShell api={deferredApi().api} allowLocalDatabase />)
    const bar = screen.getByRole('banner')

    expect(within(bar).getByRole('navigation', { name: 'Breadcrumb' })).toBeTruthy()
    expect(within(bar).getByRole('search')).toBeTruthy()
    expect(within(bar).getByPlaceholderText('Search files, classes, functions…')).toBeTruthy()
    expect(within(bar).getAllByRole('button').map((b) => b.getAttribute('aria-label'))).toEqual(['More actions'])
    expect(screen.queryByRole('complementary')).toBeNull()
  })

  it('starts from a repository URL and never asks users for a database', async () => {
    const { api, analyze } = deferredApi()
    render(<AppShell api={api} />)

    expect(screen.getByText('Open a repository')).toBeTruthy()
    const url = screen.getByRole('textbox', { name: 'Repository URL' })
    expect(screen.queryByRole('textbox', { name: 'Database path' })).toBeNull()
    expect(screen.queryByRole('button', { name: 'Open a local database (dev)' })).toBeNull()
    expect(screen.queryByText(/database/i)).toBeNull()

    await userEvent.type(url, 'not a url')
    await userEvent.click(screen.getByRole('button', { name: 'Analyze' }))
    expect(screen.getByRole('status').textContent).toMatch(/^Enter a Git repository URL/)
    expect(url.getAttribute('aria-invalid')).toBe('true')

    await userEvent.clear(url)
    await userEvent.type(url, 'https://github.com/c5ln/Telescode')
    await userEvent.click(screen.getByRole('button', { name: 'Analyze' }))
    expect(screen.getByRole('status').textContent).toBe('Repository scanning is not available yet.')
    expect(analyze).not.toHaveBeenCalled()
  })

  it('starts empty, then goes through loading to ready', async () => {
    const { api, analyze, pending } = deferredApi()
    render(<AppShell api={api} allowLocalDatabase />)

    expect(screen.getByText('Open a repository')).toBeTruthy()
    expect(canvasButtons().every((b) => b.disabled)).toBe(true)

    await openDatabase('C:/x/project.db')
    expect(analyze).toHaveBeenCalledWith('C:/x/project.db')
    expect(screen.getByRole('status').textContent).toBe('Analyzing project.db…')
    expect(screen.getByRole('main', { name: 'Workspace' }).getAttribute('aria-busy')).toBe('true')

    pending[0].resolve(snapshot)
    expect(await screen.findByText('16 files · 11 classes')).toBeTruthy()
    expect(screen.getByText('project').getAttribute('aria-current')).toBe('location')
    expect(canvasButtons().every((b) => !b.disabled)).toBe(true)
  })

  it('shows a concise bridge error and can retry', async () => {
    const { api, analyze, pending } = deferredApi()
    render(<AppShell api={api} allowLocalDatabase />)
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

  it('returns to the empty state from the overflow menu, keeping the last database path', async () => {
    const { api, pending } = deferredApi()
    render(<AppShell api={api} allowLocalDatabase />)
    await openDatabase('C:/x/project.db')
    pending[0].resolve(snapshot)
    await screen.findByText('16 files · 11 classes')

    await userEvent.click(screen.getByRole('button', { name: 'More actions' }))
    await userEvent.click(screen.getByRole('menuitem', { name: 'Open repository…' }))

    expect(screen.getByText('Open a local database')).toBeTruthy()
    expect((screen.getByRole('textbox', { name: 'Database path' }) as HTMLInputElement).value).toBe('C:/x/project.db')
  })

  it('ignores a slower, superseded request', async () => {
    const { api, pending } = deferredApi()
    render(<AppShell api={api} allowLocalDatabase />)
    await openDatabase('C:/x/old.db')

    await userEvent.click(screen.getByRole('button', { name: 'More actions' }))
    await userEvent.click(screen.getByRole('menuitem', { name: 'Open repository…' }))
    await userEvent.clear(screen.getByRole('textbox', { name: 'Database path' }))
    await openDatabase('C:/x/project.db')

    pending[1].resolve(snapshot)
    await screen.findByText('16 files · 11 classes')
    pending[0].reject(new TelescodeError('db_invalid', 'late failure'))
    await new Promise((r) => setTimeout(r, 0))
    expect(screen.queryByRole('alert')).toBeNull()
  })

  it('shows the code map when ready, and its breadcrumbs follow navigation in the map', async () => {
    render(<AppShell api={deferredApi().api} initialState={{ status: 'ready', dbPath: 'C:/x/project.db', snapshot }} />)
    expect(screen.getByRole('application', { name: /^Code map/ })).toBeTruthy()
    expect(screen.getByText('16 files · 11 classes')).toBeTruthy()

    const nav = screen.getByRole('navigation', { name: 'Breadcrumb' })
    expect(nav.textContent).toBe('project')

    // Development builds expose the renderer; jsdom has no canvas to click on.
    const renderer = (window as { __telescode?: GraphRenderer }).__telescode!
    act(() => renderer.focus(renderer.model.byId.get('class:sherlock_project/notify.py::QueryNotify')!))
    expect(nav.textContent).toBe('projectsherlock_projectnotify.pyQueryNotify')

    await userEvent.click(within(nav).getByRole('button', { name: 'sherlock_project' }))
    expect(nav.textContent).toBe('projectsherlock_project')
    expect(within(nav).getByText('sherlock_project').getAttribute('aria-current')).toBe('location')

    await userEvent.click(within(nav).getByRole('button', { name: 'project' }))
    expect(nav.textContent).toBe('project')
  })

  it('wires the canvas controls to the map once ready', async () => {
    render(<AppShell api={deferredApi().api} initialState={{ status: 'ready', dbPath: 'C:/x/project.db', snapshot }} />)
    const renderer = (window as { __telescode?: GraphRenderer }).__telescode!
    renderer.resize(1200, 750)
    const k = renderer.camera.k
    await userEvent.click(screen.getByRole('button', { name: 'Zoom in' }))
    renderer.step(performance.now() + 10_000, 16)
    expect(renderer.camera.k).toBeGreaterThan(k)
    await userEvent.click(screen.getByRole('button', { name: 'Fit to view' }))
    renderer.step(performance.now() + 20_000, 16)
    expect(renderer.camera.k).toBeCloseTo(k)
  })

  it('colours files by complexity only while the Complexity toggle is on, without moving anything', async () => {
    render(<AppShell api={deferredApi().api} initialState={{ status: 'ready', dbPath: 'C:/x/project.db', snapshot }} />)
    const renderer = (window as { __telescode?: GraphRenderer }).__telescode!
    renderer.resize(1200, 750)
    const hotspot = renderer.model.nodes.find((n) => n.complexity === 48)!
    const before = { camera: { ...renderer.camera }, box: [hotspot.x, hotspot.y, hotspot.w, hotspot.h] }
    expect(renderer.complexityLevel(hotspot)).toBe(-1)

    await userEvent.click(screen.getByRole('button', { name: 'More actions' }))
    const toggle = screen.getByRole('menuitemcheckbox', { name: 'Complexity' })
    expect(toggle.getAttribute('aria-checked')).toBe('false')
    await userEvent.click(toggle)
    expect(toggle.getAttribute('aria-checked')).toBe('true')
    expect(renderer.complexityLevel(hotspot)).toBe(3)

    await userEvent.click(toggle)
    expect(renderer.complexityLevel(hotspot)).toBe(-1)
    expect(renderer.camera).toEqual(before.camera)
    expect([hotspot.x, hotspot.y, hotspot.w, hotspot.h]).toEqual(before.box)
  })
})
