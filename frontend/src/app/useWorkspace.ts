// The workspace's load lifecycle: which repository is open and what the core
// said about it.
//
// Opening is two coarse bridge calls: the shell scans the repository (a GitHub
// URL, downloaded first, or a local folder) into a database, which is then
// analyzed. Both results are kept in state.

import { useCallback, useRef, useState } from 'react'

import {
  tauriRepositoryOpener,
  telescode,
  TelescodeError,
  type AnalysisSnapshot,
  type RepositoryOpener,
  type RepositorySource,
  type TelescodeApi,
} from '../bridge'
import { parseGitHubRepositoryUrl } from '../components/repositoryUrl'

/**
 * `source` is what the user opened. Until it has been scanned, `dbPath` is
 * empty. A workspace with no `source` was opened from a database directly
 * (development previews).
 */
export type WorkspaceState =
  | { status: 'empty' }
  | { status: 'loading'; dbPath: string; source?: RepositorySource }
  | { status: 'error'; dbPath: string; source?: RepositorySource; error: TelescodeError }
  | { status: 'ready'; dbPath: string; source?: RepositorySource; snapshot: AnalysisSnapshot }

export interface Workspace {
  state: WorkspaceState
  /** Scan the repository at `source`, then show it. */
  open: (source: RepositorySource) => void
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
    async (id: number, dbPath: string, source?: RepositorySource) => {
      setState({ status: 'loading', dbPath, source })
      try {
        const snapshot = await api.analyze(dbPath)
        if (id === requestId.current) setState({ status: 'ready', dbPath, source, snapshot })
      } catch (e) {
        if (id === requestId.current) setState({ status: 'error', dbPath, source, error: toTelescodeError(e) })
      }
    },
    [api],
  )

  const open = useCallback(
    async (source: RepositorySource) => {
      const id = ++requestId.current
      setState({ status: 'loading', dbPath: '', source })
      let dbPath: string
      try {
        dbPath = await repositories(source)
      } catch (e) {
        if (id === requestId.current) setState({ status: 'error', dbPath: '', source, error: toTelescodeError(e) })
        return
      }
      if (id === requestId.current) await analyze(id, dbPath, source)
    },
    [repositories, analyze],
  )

  const reload = useCallback(() => {
    if (state.status === 'empty') return
    // A failure is retried from the scan; a scanned repository is only re-analyzed.
    if (state.source && (state.status === 'error' || !state.dbPath)) void open(state.source)
    else void analyze(++requestId.current, state.dbPath, state.source)
  }, [analyze, open, state])

  const close = useCallback(() => {
    requestId.current++
    setState({ status: 'empty' })
  }, [])

  return { state, open: (source) => void open(source), reload, close }
}

/** A display name for the analysed repository: the database's file name without extension. */
export function repositoryName(dbPath: string): string {
  return baseName(dbPath).replace(/\.(db|sqlite3?)$/i, '')
}

/** How the workspace names what is open: the database's name, or the repository's while it is being scanned. */
export function workspaceName(state: Exclude<WorkspaceState, { status: 'empty' }>): string {
  if (state.dbPath || !state.source) return repositoryName(state.dbPath)
  return sourceName(state.source)
}

/** A repository's short name: `name` from its GitHub URL, or its folder's name. */
export function sourceName(source: RepositorySource): string {
  if (source.kind === 'folder') return baseName(source.path)
  return parseGitHubRepositoryUrl(source.url)?.name ?? source.url
}

/** A repository as the loading state names it: `owner/name`, or the folder's name. */
export function sourceLabel(source: RepositorySource): string {
  const repository = source.kind === 'github' ? parseGitHubRepositoryUrl(source.url) : null
  return repository ? `${repository.owner}/${repository.name}` : sourceName(source)
}

/** Where a repository is: its URL or folder path. */
export function sourceLocation(source: RepositorySource): string {
  return source.kind === 'github' ? source.url : source.path
}

/** The file name at the end of a path, for display. */
export function baseName(path: string): string {
  const parts = path.split(/[\\/]/).filter(Boolean)
  return parts[parts.length - 1] ?? path
}
