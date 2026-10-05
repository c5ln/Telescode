// The workspace's load lifecycle: which database is open and what the core
// said about it. One coarse bridge call per open or reload, kept in state.
//
// A repository URL adds one step in front: the shell downloads and scans the
// repository into a database, which is then analyzed like any other.

import { useCallback, useRef, useState } from 'react'

import {
  tauriRepositoryOpener,
  telescode,
  TelescodeError,
  type AnalysisSnapshot,
  type RepositoryOpener,
  type TelescodeApi,
} from '../bridge'
import { parseGitHubRepositoryUrl } from '../components/repositoryUrl'

/**
 * `url` is set when the workspace came from a repository URL. Until that
 * repository has been downloaded and scanned, `dbPath` is empty.
 */
export type WorkspaceState =
  | { status: 'empty' }
  | { status: 'loading'; dbPath: string; url?: string }
  | { status: 'error'; dbPath: string; url?: string; error: TelescodeError }
  | { status: 'ready'; dbPath: string; url?: string; snapshot: AnalysisSnapshot }

export interface Workspace {
  state: WorkspaceState
  /** Analyze the database at `dbPath` and show it. */
  open: (dbPath: string) => void
  /** Download and scan the repository at `url`, then show it. */
  openRepository: (url: string) => void
  /** Re-run the analysis for the current database, or retry whatever failed. */
  reload: () => void
  /** Return to the empty state. */
  close: () => void
}

function toTelescodeError(e: unknown): TelescodeError {
  return e instanceof TelescodeError
    ? e
    : new TelescodeError('unknown', e instanceof Error ? e.message : String(e))
}

export function useWorkspace(
  api: TelescodeApi = telescode,
  initial: WorkspaceState = { status: 'empty' },
  repositories: RepositoryOpener = tauriRepositoryOpener,
): Workspace {
  const [state, setState] = useState<WorkspaceState>(initial)
  // Only the latest request may settle the state; earlier ones are dropped.
  const requestId = useRef(0)

  const analyze = useCallback(
    async (id: number, dbPath: string, url?: string) => {
      setState({ status: 'loading', dbPath, url })
      try {
        const snapshot = await api.analyze(dbPath)
        if (id === requestId.current) setState({ status: 'ready', dbPath, url, snapshot })
      } catch (e) {
        if (id === requestId.current) setState({ status: 'error', dbPath, url, error: toTelescodeError(e) })
      }
    },
    [api],
  )

  const open = useCallback((dbPath: string, url?: string) => analyze(++requestId.current, dbPath, url), [analyze])

  const openRepository = useCallback(
    async (url: string) => {
      const id = ++requestId.current
      setState({ status: 'loading', dbPath: '', url })
      let dbPath: string
      try {
        dbPath = await repositories(url)
      } catch (e) {
        if (id === requestId.current) setState({ status: 'error', dbPath: '', url, error: toTelescodeError(e) })
        return
      }
      if (id === requestId.current) await analyze(id, dbPath, url)
    },
    [repositories, analyze],
  )

  const reload = useCallback(() => {
    if (state.status === 'empty') return
    // A failed repository is fetched again; a scanned one is only re-analyzed.
    if (state.url && (state.status === 'error' || !state.dbPath)) void openRepository(state.url)
    else void open(state.dbPath, state.url)
  }, [open, openRepository, state])

  const close = useCallback(() => {
    requestId.current++
    setState({ status: 'empty' })
  }, [])

  return {
    state,
    open: (dbPath) => void open(dbPath),
    openRepository: (url) => void openRepository(url),
    reload,
    close,
  }
}

/** A display name for the analysed repository: the database's file name without extension. */
export function repositoryName(dbPath: string): string {
  return baseName(dbPath).replace(/\.(db|sqlite3?)$/i, '')
}

/** How the workspace names what is open: the database's name, or the repository's while it is being fetched. */
export function workspaceName(state: Exclude<WorkspaceState, { status: 'empty' }>): string {
  if (state.dbPath || !state.url) return repositoryName(state.dbPath)
  return parseGitHubRepositoryUrl(state.url)?.name ?? state.url
}

/** The file name at the end of a path, for display. */
export function baseName(path: string): string {
  const parts = path.split(/[\\/]/).filter(Boolean)
  return parts[parts.length - 1] ?? path
}
