// @vitest-environment jsdom

import { act, cleanup, fireEvent, render, screen, within } from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import { afterEach, describe, expect, it, vi } from 'vitest'

import { TelescodeError, type AnalysisSnapshot, type GraphResponse, type RepositorySource, type TelescodeApi } from '../bridge'
import sherlock from '../graph/fixtures/sherlock.graph.json'
import type { GraphRenderer } from '../graph/renderer'
import type { QuizQuestion, QuizSet } from '../quiz/model'
import type { QuizLibrary } from '../quiz/quiz'
import type { TourStop } from '../tour/model'
import type { SavedTour, TourLibrary } from '../tour/tutorial'
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

/** A repository opener whose downloads stay pending until the test settles them. */
function deferredRepositories() {
  const opened: { resolve: (dbPath: string) => void; reject: (e: unknown) => void }[] = []
  const repositories = vi.fn(() => new Promise<string>((resolve, reject) => opened.push({ resolve, reject })))
  return { repositories, opened }
}

async function openRepository(url: string) {
  await userEvent.type(screen.getByRole('textbox', { name: 'Repository URL' }), url)
  await userEvent.click(screen.getByRole('button', { name: 'Analyze' }))
}

/** A repository opener that scans a folder at once, into `C:/cache/<folder name>.db`. */
const scanFolder = () =>
  vi.fn(async (source: RepositorySource) => {
    if (source.kind !== 'folder') throw new Error('only folders')
    return `C:/cache/${source.path.split('/').pop()}.db`
  })

/** The development-only path: switch the empty state to folder entry, then open. */
async function openFolder(path: string) {
  const devLink = screen.queryByRole('button', { name: 'Open a local folder (dev)' })
  if (devLink) await userEvent.click(devLink)
  await userEvent.type(screen.getByRole('textbox', { name: 'Folder path' }), path)
  await userEvent.click(screen.getByRole('button', { name: 'Open' }))
}

/** A saved tour of the Sherlock sample, short enough to play through in a test. */
function sherlockTour(holdMs: number): SavedTour {
  const stop = (nodeId: string, caption: string): TourStop => ({ nodeId, caption, title: nodeId, evidence: [], transitionMs: 0, holdMs })
  const stops = [
    stop('root', 'Sherlock finds a username across sites.'),
    stop('file:sherlock_project/result.py', 'result.py defines the shape of a result.'),
    stop('class:sherlock_project/result.py::QueryStatus', 'QueryStatus is the verdict for one site.'),
  ]
  const plan = { schemaVersion: 1 as const, id: 't1', revision: 1, title: 'Sherlock onboarding', language: 'en' as const, snapshotHash: 'x', stops }
  return { plan, snapshotHash: 'x', state: 'draft', updatedMs: 1 }
}

const library = (...lists: SavedTour[][]): TourLibrary => {
  let call = 0
  return { list: vi.fn(async () => lists[Math.min(call++, lists.length - 1)]!) }
}

const renderer = () => (window as { __telescode?: GraphRenderer }).__telescode!

const canvasButtons = () =>
  within(screen.getByRole('toolbar', { name: 'Canvas' })).getAllByRole('button') as HTMLButtonElement[]

describe('AppShell', () => {
  it('has breadcrumbs, search, agent selection, tutorial and overflow controls in the top bar, and no sidebar', () => {
    render(<AppShell api={deferredApi().api} allowLocalFolder />)
    const bar = screen.getByRole('banner')

    expect(within(bar).getByRole('navigation', { name: 'Breadcrumb' })).toBeTruthy()
    expect(within(bar).getByRole('search')).toBeTruthy()
    expect(within(bar).getByPlaceholderText('Search files, classes, functions…')).toBeTruthy()
    expect(within(bar).getAllByRole('button').map((b) => b.getAttribute('aria-label'))).toEqual([
      'AI agent',
      'Tutorial',
      'More actions',
    ])
    expect(screen.queryByRole('complementary')).toBeNull()
  })

  it('starts from a repository URL and never asks users for a database', async () => {
    const { api } = deferredApi()
    const { repositories, opened } = deferredRepositories()
    render(<AppShell api={api} repositories={repositories} />)

    expect(screen.getByText('Open a repository')).toBeTruthy()
    const url = screen.getByRole('textbox', { name: 'Repository URL' })
    expect(screen.queryByRole('textbox', { name: 'Folder path' })).toBeNull()
    expect(screen.queryByRole('button', { name: 'Open a local folder (dev)' })).toBeNull()
    expect(screen.queryByText(/database/i)).toBeNull()

    for (const bad of ['not a url', 'https://gitlab.com/c5ln/Telescode', 'https://github.com/c5ln/Telescode/pull/1']) {
      await userEvent.clear(url)
      await userEvent.type(url, bad)
      await userEvent.click(screen.getByRole('button', { name: 'Analyze' }))
      expect(screen.getByRole('status').textContent).toMatch(/^Enter a public GitHub repository URL/)
      expect(url.getAttribute('aria-invalid')).toBe('true')
    }
    expect(repositories).not.toHaveBeenCalled()

    await userEvent.clear(url)
    await userEvent.type(url, 'https://github.com/c5ln/Telescode')
    await userEvent.click(screen.getByRole('button', { name: 'Analyze' }))
    expect(repositories).toHaveBeenCalledWith({ kind: 'github', url: 'https://github.com/c5ln/Telescode' })
    expect(opened).toHaveLength(1)
  })

  it('downloads a repository, then analyzes it like a local database', async () => {
    const { api, analyze, pending } = deferredApi()
    const { repositories, opened } = deferredRepositories()
    render(<AppShell api={api} repositories={repositories} />)
    await openRepository('https://github.com/c5ln/Telescode')

    expect(screen.getByRole('status').textContent).toBe('Downloading and scanning c5ln/Telescode…')
    expect(screen.getByRole('main', { name: 'Workspace' }).getAttribute('aria-busy')).toBe('true')
    expect(screen.getByText('Telescode').getAttribute('aria-current')).toBe('location')
    expect(analyze).not.toHaveBeenCalled()

    opened[0]!.resolve('/cache/repositories/c5ln/telescode/Telescode.db')
    await vi.waitFor(() => expect(analyze).toHaveBeenCalledWith('/cache/repositories/c5ln/telescode/Telescode.db'))
    expect(screen.getByRole('status').textContent).toBe('Analyzing c5ln/Telescode…')

    pending[0]!.resolve(snapshot)
    expect(await screen.findByText('16 files · 11 classes')).toBeTruthy()
    expect(screen.getByText('Telescode').getAttribute('aria-current')).toBe('location')
  })

  it('shows why a repository could not be opened, retries the download, and offers the URL again', async () => {
    const { api, analyze } = deferredApi()
    const { repositories, opened } = deferredRepositories()
    render(<AppShell api={api} repositories={repositories} allowLocalFolder />)
    await openRepository('https://github.com/c5ln/private')

    opened[0]!.reject(new TelescodeError('repository_not_found', 'c5ln/private does not exist or is private.'))
    const alert = await screen.findByRole('alert')
    expect(within(alert).getByText('Repository not found')).toBeTruthy()
    expect(within(alert).getByText('c5ln/private does not exist or is private.')).toBeTruthy()
    expect(screen.queryByText('16 files · 11 classes')).toBeNull()

    await userEvent.click(screen.getByRole('button', { name: 'Try again' }))
    expect(repositories).toHaveBeenCalledTimes(2)
    expect(analyze).not.toHaveBeenCalled()

    opened[1]!.reject(new TelescodeError('repository_empty', 'c5ln/private has no source files Telescode can analyze.'))
    expect(await screen.findByText('No supported source files')).toBeTruthy()

    await userEvent.click(screen.getByRole('button', { name: 'Back' }))
    expect(screen.getByText('Open a repository')).toBeTruthy()
    expect((screen.getByRole('textbox', { name: 'Repository URL' }) as HTMLInputElement).value).toBe(
      'https://github.com/c5ln/private',
    )
  })

  it('drops a download the user has moved on from', async () => {
    const { api, analyze } = deferredApi()
    const { repositories, opened } = deferredRepositories()
    render(<AppShell api={api} repositories={repositories} />)
    await openRepository('https://github.com/c5ln/Telescode')

    await userEvent.click(screen.getByRole('button', { name: 'More actions' }))
    await userEvent.click(screen.getByRole('menuitem', { name: 'Open repository…' }))
    opened[0]!.resolve('/cache/Telescode.db')
    await new Promise((r) => setTimeout(r, 0))

    expect(analyze).not.toHaveBeenCalled()
    expect(screen.getByText('Open a repository')).toBeTruthy()
  })

  it('starts empty, then scans a local folder and goes through loading to ready', async () => {
    const { api, analyze, pending } = deferredApi()
    const { repositories, opened } = deferredRepositories()
    render(<AppShell api={api} repositories={repositories} allowLocalFolder />)

    expect(screen.getByText('Open a repository')).toBeTruthy()
    expect(canvasButtons().every((b) => b.disabled)).toBe(true)

    await openFolder('C:/x/project')
    expect(repositories).toHaveBeenCalledWith({ kind: 'folder', path: 'C:/x/project' })
    expect(screen.getByRole('status').textContent).toBe('Scanning project…')
    expect(screen.getByRole('main', { name: 'Workspace' }).getAttribute('aria-busy')).toBe('true')

    opened[0]!.resolve('C:/cache/project.db')
    await vi.waitFor(() => expect(analyze).toHaveBeenCalledWith('C:/cache/project.db'))
    expect(screen.getByRole('status').textContent).toBe('Analyzing project…')

    pending[0]!.resolve(snapshot)
    expect(await screen.findByText('16 files · 11 classes')).toBeTruthy()
    expect(screen.getByText('project').getAttribute('aria-current')).toBe('location')
    expect(canvasButtons().every((b) => !b.disabled)).toBe(true)
  })

  it('shows a concise bridge error and can retry', async () => {
    const { api, analyze } = deferredApi()
    const { repositories, opened } = deferredRepositories()
    render(<AppShell api={api} repositories={repositories} allowLocalFolder />)
    await openFolder('C:/x/missing')

    opened[0]!.reject(new TelescodeError('folder_not_found', 'Folder not found: C:/x/missing'))
    const alert = await screen.findByRole('alert')
    expect(within(alert).getByText('Folder not found')).toBeTruthy()
    expect(within(alert).getByText('Folder not found: C:/x/missing')).toBeTruthy()
    expect(within(alert).getByText('folder_not_found')).toBeTruthy()

    await userEvent.click(screen.getByRole('button', { name: 'Try again' }))
    expect(repositories).toHaveBeenCalledTimes(2)
    expect(analyze).not.toHaveBeenCalled()
    expect(screen.getByRole('status').textContent).toBe('Scanning missing…')
  })

  it('re-analyzes a scanned folder without scanning it again', async () => {
    const { api, analyze, pending } = deferredApi()
    const repositories = scanFolder()
    render(<AppShell api={api} repositories={repositories} allowLocalFolder />)
    await openFolder('C:/x/project')
    await vi.waitFor(() => expect(analyze).toHaveBeenCalledTimes(1))
    pending[0]!.resolve(snapshot)
    await screen.findByText('16 files · 11 classes')

    await userEvent.click(screen.getByRole('button', { name: 'More actions' }))
    await userEvent.click(screen.getByRole('menuitem', { name: 'Reload analysis' }))
    expect(analyze).toHaveBeenCalledTimes(2)
    expect(analyze).toHaveBeenLastCalledWith('C:/cache/project.db')
    expect(repositories).toHaveBeenCalledTimes(1)
  })

  it('returns to the empty state from the overflow menu, keeping the last folder path', async () => {
    const { api, pending } = deferredApi()
    render(<AppShell api={api} repositories={scanFolder()} allowLocalFolder />)
    await openFolder('C:/x/project')
    await vi.waitFor(() => expect(pending).toHaveLength(1))
    pending[0]!.resolve(snapshot)
    await screen.findByText('16 files · 11 classes')

    await userEvent.click(screen.getByRole('button', { name: 'More actions' }))
    await userEvent.click(screen.getByRole('menuitem', { name: 'Open repository…' }))

    expect(screen.getByText('Open a local folder')).toBeTruthy()
    expect((screen.getByRole('textbox', { name: 'Folder path' }) as HTMLInputElement).value).toBe('C:/x/project')
  })

  it('ignores a slower, superseded request', async () => {
    const { api, pending } = deferredApi()
    render(<AppShell api={api} repositories={scanFolder()} allowLocalFolder />)
    await openFolder('C:/x/old')
    await vi.waitFor(() => expect(pending).toHaveLength(1))

    await userEvent.click(screen.getByRole('button', { name: 'More actions' }))
    await userEvent.click(screen.getByRole('menuitem', { name: 'Open repository…' }))
    await userEvent.clear(screen.getByRole('textbox', { name: 'Folder path' }))
    await openFolder('C:/x/project')
    await vi.waitFor(() => expect(pending).toHaveLength(2))

    pending[1]!.resolve(snapshot)
    await screen.findByText('16 files · 11 classes')
    pending[0]!.reject(new TelescodeError('db_invalid', 'late failure'))
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

  it('colours files by complexity by default, and only while the Complexity toggle is on, without moving anything', async () => {
    render(<AppShell api={deferredApi().api} initialState={{ status: 'ready', dbPath: 'C:/x/project.db', snapshot }} />)
    const renderer = (window as { __telescode?: GraphRenderer }).__telescode!
    renderer.resize(1200, 750)
    const hotspot = renderer.model.nodes.find((n) => n.complexity === 48)!
    const before = { camera: { ...renderer.camera }, box: [hotspot.x, hotspot.y, hotspot.w, hotspot.h] }
    expect(renderer.complexityLevel(hotspot)).toBe(3)

    await userEvent.click(screen.getByRole('button', { name: 'More actions' }))
    const toggle = screen.getByRole('menuitemcheckbox', { name: 'Complexity' })
    expect(toggle.getAttribute('aria-checked')).toBe('true')
    await userEvent.click(toggle)
    expect(toggle.getAttribute('aria-checked')).toBe('false')
    expect(renderer.complexityLevel(hotspot)).toBe(-1)

    await userEvent.click(toggle)
    expect(renderer.complexityLevel(hotspot)).toBe(3)
    expect(renderer.camera).toEqual(before.camera)
    expect([hotspot.x, hotspot.y, hotspot.w, hotspot.h]).toEqual(before.box)
  })

  describe('tutorial mode', () => {
    const ready = { status: 'ready' as const, dbPath: 'C:/x/project.db', snapshot }

    it('is only available once a repository is open', () => {
      render(<AppShell api={deferredApi().api} tours={library([])} />)
      expect((screen.getByRole('button', { name: 'Tutorial' }) as HTMLButtonElement).disabled).toBe(true)
    })

    it('plays the saved tour on the map, step by step, and puts the view back on exit', async () => {
      render(<AppShell api={deferredApi().api} initialState={ready} tours={library([sherlockTour(60_000)])} />)
      const r = renderer()
      r.resize(1200, 750)
      const before = { ...r.camera }

      await userEvent.click(screen.getByRole('button', { name: 'Tutorial' }))
      const panel = await screen.findByRole('region', { name: 'Tutorial' })
      expect(screen.getByRole('button', { name: 'Exit tutorial', pressed: true })).toBeTruthy()
      expect(within(panel).getByText('Sherlock finds a username across sites.')).toBeTruthy()
      expect(within(panel).getByText('1 / 3')).toBeTruthy()
      expect(within(panel).getByText('Tutorial')).toBeTruthy()
      expect(within(panel).queryByText(/Sherlock onboarding/)).toBeNull()
      expect(r.selected).toBeNull()

      // The current step's bar fills over its time, a little quicker than the video, and freezes on pause.
      const fill = () => within(panel).getByTestId('step-progress')
      expect(fill().style.animationDuration).toBe('48000ms')
      expect(fill().style.animationPlayState).toBe('running')
      await userEvent.click(within(panel).getByRole('button', { name: 'Pause' }))
      expect(fill().style.animationPlayState).toBe('paused')
      await userEvent.click(within(panel).getByRole('button', { name: 'Next step' }))
      expect(within(panel).getByText('result.py defines the shape of a result.')).toBeTruthy()
      expect(within(panel).getByText('2 / 3')).toBeTruthy()
      expect(r.selected?.id).toBe('file:sherlock_project/result.py')
      r.step(performance.now() + 10_000, 16)
      expect(r.camera.k).toBeGreaterThan(before.k)

      fireEvent.keyDown(panel, { key: 'ArrowRight' })
      expect(r.selected?.id).toBe('class:sherlock_project/result.py::QueryStatus')
      expect((within(panel).getByRole('button', { name: 'Next step' }) as HTMLButtonElement).disabled).toBe(true)
      // Play on a last step reached by hand plays that step rather than starting over.
      await userEvent.click(within(panel).getByRole('button', { name: 'Play' }))
      expect(within(panel).getByText('3 / 3')).toBeTruthy()
      await userEvent.click(within(panel).getByRole('button', { name: 'Pause' }))
      await userEvent.click(within(panel).getByRole('button', { name: 'Previous step' }))
      expect(within(panel).getByText('2 / 3')).toBeTruthy()

      await userEvent.click(within(panel).getByRole('button', { name: 'Exit tutorial' }))
      expect(screen.queryByRole('region', { name: 'Tutorial' })).toBeNull()
      expect(r.selected).toBeNull()
      r.step(performance.now() + 20_000, 16)
      expect(r.camera.x).toBeCloseTo(before.x)
      expect(r.camera.k).toBeCloseTo(before.k)
      expect(r.model.nodes.length).toBeGreaterThan(0)
    })

    it('advances on its own while playing and stops at the last step', async () => {
      render(<AppShell api={deferredApi().api} initialState={ready} tours={library([sherlockTour(30)])} />)
      await userEvent.click(screen.getByRole('button', { name: 'Tutorial' }))
      const panel = await screen.findByRole('region', { name: 'Tutorial' })
      await within(panel).findByText('3 / 3')
      // The last step plays out too, and its bar ends full.
      expect(within(panel).getByTestId('step-progress').style.animationPlayState).toBe('running')
      await within(panel).findByRole('button', { name: 'Play' })
      expect(within(panel).queryByTestId('step-progress')).toBeNull()
      expect([...panel.querySelectorAll('li')].map((li) => li.dataset.state)).toEqual(['done', 'done', 'done'])
      expect(renderer().selected?.id).toBe('class:sherlock_project/result.py::QueryStatus')

      // Play from the end starts over.
      await userEvent.click(within(panel).getByRole('button', { name: 'Play' }))
      expect(within(panel).getByText('1 / 3')).toBeTruthy()
    })

    it('generates a tutorial when there is none, and offers it once it is saved', async () => {
      let finish!: () => void
      const tours = { ...library([], [sherlockTour(60_000)]), generate: vi.fn(() => new Promise<void>((r) => (finish = r))) }
      render(<AppShell api={deferredApi().api} initialState={ready} tours={tours} />)
      await userEvent.click(screen.getByRole('button', { name: 'Tutorial' }))
      const panel = await screen.findByRole('region', { name: 'Tutorial' })
      await within(panel).findByText('Generating a tutorial for project')
      expect(within(panel).getByRole('status').textContent).toBe('Generating… It appears here as soon as it is ready.')
      expect(tours.generate).toHaveBeenCalledWith('C:/x/project.db')

      await act(async () => finish())
      await within(panel).findByText('Tutorial ready')
      expect(within(panel).getByText('Sherlock onboarding · 3 steps')).toBeTruthy()
      await userEvent.click(within(panel).getByRole('button', { name: 'Start tutorial' }))
      expect(within(panel).getByText('1 / 3')).toBeTruthy()
    })

    it('looks at Claude Code only when ▷ finds no tutorial, never on opening', async () => {
      const agentStatus = vi.fn(async () => ({ installed: true, signedIn: true }))
      const generate = vi.fn(async () => {})
      render(<AppShell api={deferredApi().api} initialState={ready} tours={{ ...library([sherlockTour(60_000)]), agentStatus, generate }} />)
      expect(agentStatus).not.toHaveBeenCalled()
      await userEvent.click(screen.getByRole('button', { name: 'Tutorial' }))
      await within(await screen.findByRole('region', { name: 'Tutorial' })).findByText('1 / 3')
      expect(agentStatus).not.toHaveBeenCalled()
      expect(generate).not.toHaveBeenCalled()
    })

    it('signs in to Claude first when signed out, then generates', async () => {
      let signedIn!: () => void
      const signIn = vi.fn(() => new Promise<void>((r) => (signedIn = r)))
      const generate = vi.fn(() => new Promise<void>(() => {}))
      const tours = { ...library([]), agentStatus: async () => ({ installed: true, signedIn: false }), signIn, generate }
      render(<AppShell api={deferredApi().api} initialState={ready} tours={tours} />)
      await userEvent.click(screen.getByRole('button', { name: 'Tutorial' }))
      const panel = await screen.findByRole('region', { name: 'Tutorial' })
      await within(panel).findByText('Sign in to Claude to generate a tutorial for project')
      expect(within(panel).getByRole('status').textContent).toBe(
        'Finish signing in to Claude in your browser. The tutorial is generated right after.',
      )
      expect(signIn).toHaveBeenCalledTimes(1)
      expect(generate).not.toHaveBeenCalled()

      await act(async () => signedIn())
      await within(panel).findByText('Generating a tutorial for project')
      expect(generate).toHaveBeenCalledWith('C:/x/project.db')
    })

    it('does not sign in when already signed in', async () => {
      const signIn = vi.fn(async () => {})
      const generate = vi.fn(() => new Promise<void>(() => {}))
      const tours = { ...library([]), agentStatus: async () => ({ installed: true, signedIn: true }), signIn, generate }
      render(<AppShell api={deferredApi().api} initialState={ready} tours={tours} />)
      await userEvent.click(screen.getByRole('button', { name: 'Tutorial' }))
      await within(await screen.findByRole('region', { name: 'Tutorial' })).findByText('Generating a tutorial for project')
      expect(signIn).not.toHaveBeenCalled()
      expect(generate).toHaveBeenCalled()
    })

    it('explains how to install Claude Code when it is missing', async () => {
      const generate = vi.fn(async () => {})
      const tours = { ...library([]), agentStatus: async () => ({ installed: false, signedIn: false }), generate }
      render(<AppShell api={deferredApi().api} initialState={ready} tours={tours} />)
      await userEvent.click(screen.getByRole('button', { name: 'Tutorial' }))
      const panel = await screen.findByRole('region', { name: 'Tutorial' })
      await within(panel).findByText('Claude Code is needed to generate tutorials')
      expect(within(panel).getByText('curl -fsSL https://claude.ai/install.sh | bash')).toBeTruthy()
      expect(generate).not.toHaveBeenCalled()
    })

    it('reports a failed sign-in', async () => {
      const tours = {
        ...library([]),
        agentStatus: async () => ({ installed: true, signedIn: false }),
        signIn: () => Promise.reject(new TelescodeError('tour_generation_failed', 'Signing in to Claude did not finish in time.')),
        generate: vi.fn(async () => {}),
      }
      render(<AppShell api={deferredApi().api} initialState={ready} tours={tours} />)
      await userEvent.click(screen.getByRole('button', { name: 'Tutorial' }))
      const panel = await screen.findByRole('region', { name: 'Tutorial' })
      expect(await within(panel).findByText('Cannot generate the tutorial: Signing in to Claude did not finish in time.')).toBeTruthy()
      expect(within(panel).getByText('Sign in to Claude to generate a tutorial for project')).toBeTruthy()
      expect(tours.generate).not.toHaveBeenCalled()
    })

    it('offers a tutorial saved while generation is still running', async () => {
      vi.useFakeTimers({ toFake: ['setInterval', 'clearInterval'] })
      try {
        const tours = { ...library([], [], [sherlockTour(60_000)]), generate: vi.fn(() => new Promise<void>(() => {})) }
        render(<AppShell api={deferredApi().api} initialState={ready} tours={tours} />)
        await userEvent.click(screen.getByRole('button', { name: 'Tutorial' }))
        const panel = await screen.findByRole('region', { name: 'Tutorial' })
        await within(panel).findByText('Generating a tutorial for project')

        await act(async () => vi.advanceTimersByTime(3000))
        expect(within(panel).queryByText('Tutorial ready')).toBeNull()
        await act(async () => vi.advanceTimersByTime(3000))
        await within(panel).findByText('Tutorial ready')
        expect(tours.list).toHaveBeenCalledTimes(3)
      } finally {
        vi.useRealTimers()
      }
    })

    it('reports a failed generation', async () => {
      const failing = (e: Error) => ({ ...library([]), generate: () => Promise.reject(e) })
      render(<AppShell api={deferredApi().api} initialState={ready} tours={failing(new TelescodeError('tour_generation_failed', 'Claude Code was not found.'))} />)
      await userEvent.click(screen.getByRole('button', { name: 'Tutorial' }))
      const panel = await screen.findByRole('region', { name: 'Tutorial' })
      expect(await within(panel).findByText('Cannot generate the tutorial: Claude Code was not found.')).toBeTruthy()
      expect(within(panel).queryByRole('status')).toBeNull()
    })

    it('reports a generation that saved nothing', async () => {
      render(<AppShell api={deferredApi().api} initialState={ready} tours={{ ...library([]), generate: async () => {} }} />)
      await userEvent.click(screen.getByRole('button', { name: 'Tutorial' }))
      const panel = await screen.findByRole('region', { name: 'Tutorial' })
      expect(await within(panel).findByText('Cannot generate the tutorial: Generation finished without saving a tutorial.')).toBeTruthy()
    })

    it('reports when saved tours cannot be read, without generating', async () => {
      const generate = vi.fn(async () => {})
      const tours: TourLibrary = { list: () => Promise.reject(new TelescodeError('tours_unavailable', 'No MCP server.')), generate }
      render(<AppShell api={deferredApi().api} initialState={ready} tours={tours} />)
      await userEvent.click(screen.getByRole('button', { name: 'Tutorial' }))
      const panel = await screen.findByRole('region', { name: 'Tutorial' })
      expect(await within(panel).findByText('Cannot generate the tutorial: No MCP server.')).toBeTruthy()
      expect(generate).not.toHaveBeenCalled()
    })
  })

  describe('quizzes on a tutorial step', () => {
    const ready = { status: 'ready' as const, dbPath: 'C:/x/project.db', snapshot }
    const question = (n: number, over: Partial<QuizQuestion>): QuizQuestion => ({
      id: `q1:${n}`,
      type: 'multiple_choice',
      difficulty: 'easy',
      concept: 'Repository layout',
      question: 'Which directory holds the tests?',
      answer: 'tests',
      distractors: ['devel', 'sherlock_project'],
      choices: ['devel', 'tests', 'sherlock_project'],
      explanation: 'tests is a top-level directory of the repository.',
      evidence: [
        { nodeId: 'root', title: 'Repository' },
        { nodeId: 'dir:tests', title: 'tests' },
      ],
      ...over,
    })
    const quizSet: QuizSet = {
      schemaVersion: 1,
      id: 'q1',
      tourId: 't1',
      tourRevision: 1,
      stopIndex: 0,
      nodeId: 'root',
      language: 'en',
      snapshotHash: 'x',
      createdMs: 1,
      questions: [
        question(1, {}),
        question(2, { type: 'true_false', question: 'devel holds the package.', answer: 'False', distractors: ['True'], choices: ['True', 'False'], concept: 'Packages' }),
        question(3, { difficulty: 'medium', question: 'What does sherlock_project contain?', concept: 'Packages' }),
      ],
      }

    /** Quizzes graded like the server does, keeping every answer. */
    function quizzes(...lists: QuizSet[][]) {
      let call = 0
      const answers: { quizId: string; selectedIndex: number; responseTimeMs: number | null }[] = []
      const quizLibrary = {
        list: vi.fn(async () => lists[Math.min(call++, lists.length - 1)]!),
        respond: vi.fn(async ({ quizSetId, quizId, selectedIndex, responseTimeMs }: Parameters<QuizLibrary['respond']>[0]) => {
          const q = lists.flat().find((s) => s.id === quizSetId)!.questions.find((x) => x.id === quizId)!
          answers.push({ quizId, selectedIndex, responseTimeMs })
          const correct = q.choices[selectedIndex] === q.answer
          const attempt = answers.filter((a) => a.quizId === quizId).length
          return {
            response: {} as never,
            feedback: { correct, answer: q.answer, explanation: q.explanation, evidence: q.evidence, attempt },
          }
        }),
      } satisfies QuizLibrary
      return { library: quizLibrary, answers }
    }

    async function openQuiz() {
      await userEvent.click(screen.getByRole('button', { name: 'Tutorial' }))
      const tutorial = await screen.findByRole('region', { name: 'Tutorial' })
      await userEvent.click(within(tutorial).getByRole('button', { name: 'Quiz on this step' }))
      return screen.findByRole('region', { name: 'Quiz' })
    }

    it('asks the step’s questions, records each answer and explains the result', async () => {
      const { library: quizLibrary, answers } = quizzes([quizSet])
      render(<AppShell api={deferredApi().api} initialState={ready} tours={library([sherlockTour(60_000)])} quizzes={quizLibrary} />)
      const panel = await openQuiz()
      await within(panel).findByText('Which directory holds the tests?')
      expect(within(panel).getByText('Quiz · root')).toBeTruthy()
      expect(within(panel).getByText('Question 1 of 2 · Repository layout')).toBeTruthy()
      const difficulty = (name: string) => within(within(panel).getByRole('group', { name: 'Difficulty' })).getByRole('button', { name })
      expect(difficulty('Easy').getAttribute('aria-pressed')).toBe('true')
      expect((difficulty('Hard') as HTMLButtonElement).disabled).toBe(true)

      const check = within(panel).getByRole('button', { name: 'Check answer' }) as HTMLButtonElement
      expect(check.disabled).toBe(true)
      await userEvent.click(within(panel).getByRole('radio', { name: 'devel' }))
      await userEvent.click(check)
      expect(await within(panel).findByText('Not quite. The answer is “tests”.')).toBeTruthy()
      expect(within(panel).getByText('tests is a top-level directory of the repository.')).toBeTruthy()
      expect(answers).toEqual([{ quizId: 'q1:1', selectedIndex: 0, responseTimeMs: expect.any(Number) }])
      expect(within(panel).getByRole('radio', { name: 'tests' }).matches(':disabled')).toBe(true)

      // The nodes an answer rests on can be looked at on the map.
      await userEvent.click(within(panel).getByRole('button', { name: 'tests' }))
      expect(renderer().selected?.id).toBe('dir:tests')

      // Trying again is a new attempt at the same question.
      await userEvent.click(within(panel).getByRole('button', { name: 'Try again' }))
      await userEvent.click(within(panel).getByRole('radio', { name: 'tests' }))
      await userEvent.click(within(panel).getByRole('button', { name: 'Check answer' }))
      expect(await within(panel).findByText('Correct.')).toBeTruthy()
      expect(within(panel).queryByRole('button', { name: 'Try again' })).toBeNull()
      expect(answers.map((a) => [a.quizId, a.selectedIndex])).toEqual([['q1:1', 0], ['q1:1', 1]])

      await userEvent.click(within(panel).getByRole('button', { name: 'Next question' }))
      expect(within(panel).getByText('devel holds the package.')).toBeTruthy()
      await userEvent.click(difficulty('Medium'))
      expect(within(panel).getByText('What does sherlock_project contain?')).toBeTruthy()
      expect(within(panel).queryByRole('button', { name: 'Next question' })).toBeNull()

      // Back to the tutorial: the same step, paused, with the map on it again.
      await userEvent.click(within(panel).getByRole('button', { name: 'Back to tutorial' }))
      const tutorial = screen.getByRole('region', { name: 'Tutorial' })
      expect(within(tutorial).getByText('1 / 3')).toBeTruthy()
      expect(within(tutorial).getByRole('button', { name: 'Play' })).toBeTruthy()
      // The root step selects nothing, as in the tutorial itself.
      expect(renderer().selected).toBeNull()
    })

    it('writes a quiz when the step has none, then asks it', async () => {
      let finish!: () => void
      const { library: quizLibrary } = quizzes([], [quizSet])
      const generate = vi.fn(() => new Promise<void>((resolve) => (finish = resolve)))
      render(<AppShell api={deferredApi().api} initialState={ready} tours={library([sherlockTour(60_000)])} quizzes={{ ...quizLibrary, generate }} />)
      const panel = await openQuiz()
      await within(panel).findByText('Writing a quiz on this step')
      expect(generate).toHaveBeenCalledWith({ tourId: 't1', tourRevision: 1, stopIndex: 0, nodeId: 'root', dbPath: 'C:/x/project.db' })
      await act(async () => finish())
      expect(await within(panel).findByText('Which directory holds the tests?')).toBeTruthy()
    })

    it('reports an answer that could not be recorded, and lets the user answer again', async () => {
      const { library: quizLibrary } = quizzes([quizSet])
      quizLibrary.respond.mockRejectedValueOnce(new TelescodeError('tours_unavailable', 'The Telescode MCP server stopped.'))
      render(<AppShell api={deferredApi().api} initialState={ready} tours={library([sherlockTour(60_000)])} quizzes={quizLibrary} />)
      const panel = await openQuiz()
      await userEvent.click(await within(panel).findByRole('radio', { name: 'tests' }))
      await userEvent.click(within(panel).getByRole('button', { name: 'Check answer' }))
      expect(await within(panel).findByText('Cannot record the answer: The Telescode MCP server stopped.')).toBeTruthy()
      await userEvent.click(within(panel).getByRole('button', { name: 'Check answer' }))
      expect(await within(panel).findByText('Correct.')).toBeTruthy()
    })

    it('closes with the tutorial', async () => {
      render(<AppShell api={deferredApi().api} initialState={ready} tours={library([sherlockTour(60_000)])} quizzes={quizzes([quizSet]).library} />)
      const panel = await openQuiz()
      await within(panel).findByText('Which directory holds the tests?')
      await userEvent.click(screen.getByRole('button', { name: 'Exit tutorial', pressed: true }))
      expect(screen.queryByRole('region', { name: 'Quiz' })).toBeNull()
      await userEvent.click(screen.getByRole('button', { name: 'Tutorial' }))
      expect(await screen.findByRole('region', { name: 'Tutorial' })).toBeTruthy()
      expect(screen.queryByRole('region', { name: 'Quiz' })).toBeNull()
    })
  })
})
