// The graph workspace: fills everything below the top bar.
//
// Nothing is drawn yet. The graph viewport will mount inside `.viewport`;
// the workspace states are overlays on top of it, so the layout stays the same
// in every state.

import type { TelescodeErrorCode } from '../bridge'
import { baseName, type WorkspaceState } from '../app/useWorkspace'
import { Button } from '../ui/Button'
import { ErrorState } from '../ui/ErrorState'
import { Spinner } from '../ui/Spinner'
import { CanvasControls } from './CanvasControls'
import { EmptyWorkspace } from './EmptyWorkspace'
import styles from './WorkspaceCanvas.module.css'

interface WorkspaceCanvasProps {
  state: WorkspaceState
  onOpen: (dbPath: string) => void
  onRetry: () => void
  onClose: () => void
  /** Offer the development-only database path entry in the empty state. */
  allowLocalDatabase: boolean
  /** Database path to prefill when returning to the empty state. */
  lastDbPath?: string
}

const ERROR_TITLES: Partial<Record<TelescodeErrorCode, string>> = {
  db_not_found: 'Database not found',
  db_not_a_file: 'Not a database file',
  db_invalid: 'Not a Telescode database',
  sidecar_missing: 'Telescode core not found',
  sidecar_spawn_failed: 'Telescode core failed to start',
  sidecar_failed: 'Analysis failed',
  bridge_unavailable: 'Desktop app required',
  unsupported_schema_version: 'Unsupported database version',
}

export function WorkspaceCanvas({ state, onOpen, onRetry, onClose, allowLocalDatabase, lastDbPath }: WorkspaceCanvasProps) {
  return (
    <main className={styles.canvas} aria-label="Workspace" aria-busy={state.status === 'loading'}>
      <div className={styles.viewport} />

      {state.status === 'empty' && (
        <div className={styles.center}>
          <EmptyWorkspace allowLocalDatabase={allowLocalDatabase} onOpenDatabase={onOpen} lastDbPath={lastDbPath} />
        </div>
      )}

      {state.status === 'loading' && (
        <div className={styles.center}>
          <p className={styles.loading} role="status">
            <Spinner />
            Analyzing {baseName(state.dbPath)}…
          </p>
        </div>
      )}

      {state.status === 'error' && (
        <div className={styles.center}>
          <ErrorState
            title={ERROR_TITLES[state.error.code] ?? 'Could not open the database'}
            message={state.error.message}
            code={state.error.code}
          >
            <Button variant="primary" onClick={onRetry}>
              Try again
            </Button>
            <Button onClick={onClose}>Back</Button>
          </ErrorState>
        </div>
      )}

      {state.status === 'ready' && (
        <p className={styles.status} role="status">
          {state.snapshot.totals.fileCount} files · {state.snapshot.totals.classCount} classes
        </p>
      )}

      <CanvasControls disabled={state.status !== 'ready'} />
    </main>
  )
}
