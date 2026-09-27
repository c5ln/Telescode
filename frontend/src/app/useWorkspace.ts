// The workspace's load lifecycle: which database is open and what the core
// said about it. One coarse bridge call per open or reload, kept in state.

import { useCallback, useRef, useState } from 'react'

import { telescode, TelescodeError, type AnalysisSnapshot, type TelescodeApi } from '../bridge'

export type WorkspaceState =
  | { status: 'empty' }
  | { status: 'loading'; dbPath: string }
  | { status: 'error'; dbPath: string; error: TelescodeError }
  | { status: 'ready'; dbPath: string; snapshot: AnalysisSnapshot }

export interface Workspace {
  state: WorkspaceState
  /** Analyze the database at `dbPath` and show it. */
  open: (dbPath: string) => void
  /** Re-run the analysis for the current database. */
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
): Workspace {
  const [state, setState] = useState<WorkspaceState>(initial)
  // Only the latest request may settle the state; earlier ones are dropped.
  const requestId = useRef(0)

  const open = useCallback(
    async (dbPath: string) => {
      const id = ++requestId.current
      setState({ status: 'loading', dbPath })
      try {
        const snapshot = await api.analyze(dbPath)
        if (id === requestId.current) setState({ status: 'ready', dbPath, snapshot })
      } catch (e) {
        if (id === requestId.current) setState({ status: 'error', dbPath, error: toTelescodeError(e) })
      }
    },
    [api],
  )

  const reload = useCallback(() => {
    if (state.status !== 'empty') void open(state.dbPath)
  }, [open, state])

  const close = useCallback(() => {
    requestId.current++
    setState({ status: 'empty' })
  }, [])

  return { state, open: (dbPath) => void open(dbPath), reload, close }
}

/** The file name at the end of a path, for display. */
export function baseName(path: string): string {
  const parts = path.split(/[\\/]/).filter(Boolean)
  return parts[parts.length - 1] ?? path
}
