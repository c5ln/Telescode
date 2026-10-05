// The graph workspace: fills everything below the top bar.
//
// The code map mounts inside `.viewport` once a workspace is ready; the other
// states are overlays in the same place, so the layout stays the same in
// every state.

import type { RefObject } from 'react'

import type { TelescodeErrorCode } from '../bridge'
import type { Tutorial } from '../app/useTutorial'
import { baseName, workspaceName, type WorkspaceState } from '../app/useWorkspace'
import type { GraphHandle } from '../graph/GraphCanvas'
import type { GraphNode } from '../graph/model'
import { Button } from '../ui/Button'
import { ErrorState } from '../ui/ErrorState'
import { Spinner } from '../ui/Spinner'
import { CanvasControls } from './CanvasControls'
import { CodeMap } from './CodeMap'
import { EmptyWorkspace } from './EmptyWorkspace'
import { parseGitHubRepositoryUrl } from './repositoryUrl'
import { TutorialPanel } from './TutorialPanel'
import styles from './WorkspaceCanvas.module.css'

interface WorkspaceCanvasProps {
  state: WorkspaceState
  onOpen: (dbPath: string) => void
  onOpenRepository: (url: string) => void
  onRetry: () => void
  onClose: () => void
  /** Offer the development-only database path entry in the empty state. */
  allowLocalDatabase: boolean
  /** Database path to prefill when returning to the empty state. */
  lastDbPath?: string
  /** Repository URL to prefill when returning to the empty state. */
  lastUrl?: string
  graphRef: RefObject<GraphHandle | null>
  onContextChange: (path: GraphNode[]) => void
  /** Shade files by complexity. */
  complexity: boolean
  tutorial: Tutorial
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
  invalid_repository_url: 'Not a GitHub repository URL',
  repository_not_found: 'Repository not found',
  repository_unreachable: 'GitHub could not be reached',
  repository_too_large: 'Repository too large',
  repository_empty: 'No supported source files',
  repository_fetch_failed: 'Download failed',
}

/** What the loading state says is happening. */
function loadingText(state: Extract<WorkspaceState, { status: 'loading' }>): string {
  const repository = state.url ? parseGitHubRepositoryUrl(state.url) : null
  const label = repository ? `${repository.owner}/${repository.name}` : state.url
  if (!state.dbPath) return `Downloading and scanning ${label}…`
  return `Analyzing ${label ?? baseName(state.dbPath)}…`
}

export function WorkspaceCanvas({
  state,
  onOpen,
  onOpenRepository,
  onRetry,
  onClose,
  allowLocalDatabase,
  lastDbPath,
  lastUrl,
  graphRef,
  onContextChange,
  complexity,
  tutorial,
}: WorkspaceCanvasProps) {
  return (
    <main className={styles.canvas} aria-label="Workspace" aria-busy={state.status === 'loading'}>
      <div className={styles.viewport}>
        {state.status === 'ready' && (
          <CodeMap
            snapshot={state.snapshot}
            repositoryName={workspaceName(state)}
            onContextChange={onContextChange}
            onRetry={onRetry}
            graphRef={graphRef}
            complexity={complexity}
          />
        )}
      </div>

      {state.status === 'empty' && (
        <div className={styles.center}>
          <EmptyWorkspace
            allowLocalDatabase={allowLocalDatabase}
            onOpenDatabase={onOpen}
            onOpenRepository={onOpenRepository}
            lastDbPath={lastDbPath}
            lastUrl={lastUrl}
          />
        </div>
      )}

      {state.status === 'loading' && (
        <div className={styles.center}>
          <p className={styles.loading} role="status">
            <Spinner />
            {loadingText(state)}
          </p>
        </div>
      )}

      {state.status === 'error' && (
        <div className={styles.center}>
          <ErrorState
            title={ERROR_TITLES[state.error.code] ?? (state.url ? 'Could not open the repository' : 'Could not open the database')}
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
        <TutorialPanel tutorial={tutorial} repositoryName={workspaceName(state)} />
      )}

      <CanvasControls
        disabled={state.status !== 'ready'}
        onZoomIn={() => graphRef.current?.zoomIn()}
        onZoomOut={() => graphRef.current?.zoomOut()}
        onFit={() => graphRef.current?.fit()}
      />
    </main>
  )
}
