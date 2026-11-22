// The graph workspace: fills everything below the top bar.
//
// The code map mounts inside `.viewport` once a workspace is ready; the other
// states are overlays in the same place, so the layout stays the same in
// every state.

import { useMemo, type RefObject } from 'react'

import { readNodes, type AgentActivity, type AgentProvider } from '../agent'
import type { RepositorySource, TelescodeError } from '../bridge'
import type { Quiz } from '../app/useQuiz'
import type { Tutorial } from '../app/useTutorial'
import type { Learning } from '../app/useLearning'
import { LearningPanel } from './LearningPanel'
import { LoadingProgress } from './LoadingProgress'
import { baseName, sourceLabel, workspaceName, type WorkspaceState } from '../app/useWorkspace'
import type { GraphHandle } from '../graph/GraphCanvas'
import type { GraphNode } from '../graph/model'
import { Button } from '../ui/Button'
import { ErrorState } from '../ui/ErrorState'
import { openExternal } from '../openUrl'
import { describeError, errorDetails, reportUrl, type ErrorAction } from './errorMessages'
import { EXAMPLE_REPOSITORIES, exampleUrl } from './examples'
import { Spinner } from '../ui/Spinner'
import { CanvasControls } from './CanvasControls'
import { CodeMap } from './CodeMap'
import { EmptyWorkspace } from './EmptyWorkspace'
import { TutorialPanel } from './TutorialPanel'
import styles from './WorkspaceCanvas.module.css'

interface WorkspaceCanvasProps {
  state: WorkspaceState
  onOpen: (source: RepositorySource) => void
  onRetry: () => void
  onClose: () => void
  /** Offer the development-only folder entry in the empty state. */
  allowLocalFolder: boolean
  /** Repository to offer again when returning to the empty state. */
  lastSource?: RepositorySource
  graphRef: RefObject<GraphHandle | null>
  onContextChange: (path: GraphNode[]) => void
  /** Shade files by complexity. */
  complexity: boolean
  onComplexityChange: (on: boolean) => void
  tutorial: Tutorial
  quiz: Quiz
  provider?: AgentProvider
  /** Switch to another agent, offered when the agent fails to write something. */
  onSwitchAgent?: (provider: AgentProvider) => void
  /** A goal to suggest when starting a tour, e.g. an example repository's. */
  suggestedRequirements?: string
  learning?: Learning
}

/** What is being opened, by name. */
function loadingName(state: Extract<WorkspaceState, { status: 'loading' }>): string {
  return state.source ? sourceLabel(state.source) : baseName(state.dbPath)
}

/** What the loading state says is happening. */
function loadingText(state: Extract<WorkspaceState, { status: 'loading' }>): string {
  if (!state.source) return `Analyzing ${baseName(state.dbPath)}…`
  const label = sourceLabel(state.source)
  if (state.dbPath) return `Analyzing ${label}…`
  return state.source.kind === 'github' ? `Downloading and scanning ${label}…` : `Scanning ${label}…`
}

export function WorkspaceCanvas({
  state,
  onOpen,
  onRetry,
  onClose,
  allowLocalFolder,
  lastSource,
  graphRef,
  onContextChange,
  complexity,
  onComplexityChange,
  tutorial,
  quiz,
  provider,
  onSwitchAgent,
  suggestedRequirements,
  learning,
}: WorkspaceCanvasProps) {
  // While an agent writes, the map outlines what it has read.
  const activity = [tutorial.state, quiz.state, learning?.state]
    .map((s): AgentActivity | undefined => (s && 'activity' in s ? s.activity : undefined))
    .find(Boolean)
  const reading = useMemo(() => readNodes(activity), [activity])
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
            onComplexityChange={onComplexityChange}
            hintAllowed={tutorial.state.status === 'off' && quiz.state.status === 'off' && (!learning || learning.state.status === 'off')}
            reading={reading}
          />
        )}
      </div>

      {state.status === 'empty' && (
        <div className={styles.center}>
          <EmptyWorkspace allowLocalFolder={allowLocalFolder} onOpen={onOpen} lastSource={lastSource} />
        </div>
      )}

      {state.status === 'loading' && (
        <div className={styles.center}>
          {state.progress ? (
            <LoadingProgress
              name={loadingName(state)}
              downloads={state.source?.kind === 'github'}
              progress={state.progress}
              onCancel={onClose}
            />
          ) : (
            <p className={styles.loading} role="status">
              <Spinner />
              {loadingText(state)}
            </p>
          )}
        </div>
      )}

      {state.status === 'error' && (
        <div className={styles.center}>
          <WorkspaceError error={state.error} source={state.source} onOpen={onOpen} onRetry={onRetry} onClose={onClose} />
        </div>
      )}

      {state.status === 'ready' && (
        learning && learning.state.status !== 'off' ? <LearningPanel learning={learning} provider={provider} onSwitchAgent={onSwitchAgent} /> :
          <TutorialPanel provider={provider} onSwitchAgent={onSwitchAgent} suggestedRequirements={suggestedRequirements} tutorial={tutorial} quiz={quiz} repositoryName={workspaceName(state)} learning={learning} />
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

const ACTION_LABELS: Record<ErrorAction, string> = {
  edit: 'Edit URL',
  example: 'Try an example',
  retry: 'Try again',
  back: 'Back',
  another: 'Try another repository',
  report: 'Report issue',
}

/** Why the repository could not be opened, and the way forward that fits. */
function WorkspaceError({
  error,
  source,
  onOpen,
  onRetry,
  onClose,
}: {
  error: TelescodeError
  source?: RepositorySource
  onOpen: (source: RepositorySource) => void
  onRetry: () => void
  onClose: () => void
}) {
  const { tone, title, cause, next, actions } = describeError(error, source)
  const details = errorDetails(error)
  const run: Record<ErrorAction, () => void> = {
    edit: onClose,
    example: () => onOpen({ kind: 'github', url: exampleUrl(EXAMPLE_REPOSITORIES[0]!) }),
    retry: onRetry,
    back: onClose,
    another: onClose,
    report: () => openExternal(reportUrl(title, details)),
  }
  return (
    <ErrorState title={title} cause={cause} next={next} tone={tone} details={details}>
      {actions.map((action, i) => (
        <Button key={action} variant={i === 0 ? 'primary' : 'secondary'} onClick={run[action]}>
          {action === 'edit' && source?.kind === 'folder' ? 'Edit path' : ACTION_LABELS[action]}
        </Button>
      ))}
    </ErrorState>
  )
}
