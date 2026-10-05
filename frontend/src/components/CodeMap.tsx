// The ready state: the analysed repository as a zoomable map.
//
// Builds the map once per snapshot (no bridge calls after that) and keeps a
// failure to build or draw it inside the canvas, as a normal error state.

import { useMemo, useState, type RefObject } from 'react'

import type { AnalysisSnapshot } from '../bridge'
import { GraphCanvas, type GraphHandle } from '../graph/GraphCanvas'
import { layoutGraph } from '../graph/layout'
import { buildGraphModel, type GraphModel, type GraphNode } from '../graph/model'
import { Button } from '../ui/Button'
import { EmptyState } from '../ui/EmptyState'
import { ErrorState } from '../ui/ErrorState'
import styles from './WorkspaceCanvas.module.css'

interface CodeMapProps {
  snapshot: AnalysisSnapshot
  repositoryName: string
  onContextChange: (path: GraphNode[]) => void
  onRetry: () => void
  graphRef: RefObject<GraphHandle | null>
  complexity: boolean
}

type Built = { model: GraphModel; error?: undefined } | { model?: undefined; error: unknown }

export function CodeMap({ snapshot, repositoryName, onContextChange, onRetry, graphRef, complexity }: CodeMapProps) {
  const built = useMemo<Built>(() => {
    try {
      const model = buildGraphModel(snapshot, repositoryName)
      layoutGraph(model)
      return { model }
    } catch (error) {
      return { error }
    }
  }, [snapshot, repositoryName])
  const [drawError, setDrawError] = useState<unknown>(null)

  const error = built.error ?? drawError
  if (error) {
    return (
      <div className={styles.center}>
        <ErrorState
          title="Could not draw the code map"
          message={error instanceof Error ? error.message : String(error)}
        >
          <Button variant="primary" onClick={onRetry}>
            Try again
          </Button>
        </ErrorState>
      </div>
    )
  }

  const model = built.model!
  if (model.root.fileCount === 0) {
    return (
      <div className={styles.center}>
        <EmptyState title="Nothing to map" description="The analysis has no source files Telescode can read." />
      </div>
    )
  }

  return (
    <>
      <GraphCanvas
        ref={graphRef}
        model={model}
        onContextChange={onContextChange}
        onError={setDrawError}
        complexity={complexity}
      />
      <p className={styles.status}>
        {model.root.fileCount} files · {model.root.classCount} classes
      </p>
    </>
  )
}
