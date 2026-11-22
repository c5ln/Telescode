// The ready state: the analysed repository as a zoomable map.
//
// Builds the map once per snapshot (no bridge calls after that) and keeps a
// failure to build or draw it inside the canvas, as a normal error state.

import { useEffect, useMemo, useRef, useState, type RefObject } from 'react'

import type { AnalysisSnapshot } from '../bridge'
import { modelComplexityLevels } from '../graph/complexity'
import { GraphCanvas, type GraphHandle } from '../graph/GraphCanvas'
import { layoutGraph } from '../graph/layout'
import { buildGraphModel, type GraphModel, type GraphNode } from '../graph/model'
import { Button } from '../ui/Button'
import { EmptyState } from '../ui/EmptyState'
import { ErrorState } from '../ui/ErrorState'
import { FileCard, MapHint, MapLegend, useMapHint, type FileFacts } from './MapOverlays'
import styles from './WorkspaceCanvas.module.css'

interface CodeMapProps {
  snapshot: AnalysisSnapshot
  repositoryName: string
  onContextChange: (path: GraphNode[]) => void
  onRetry: () => void
  graphRef: RefObject<GraphHandle | null>
  complexity: boolean
  onComplexityChange: (on: boolean) => void
  /** Whether the first-time hint may show; off while a tour or quiz holds the bottom of the map. */
  hintAllowed?: boolean
}

/** How long the pointer rests on a file before its card shows. */
const CARD_DELAY_MS = 350
const CARD_WIDTH = 264
const CARD_OFFSET = 16

type Built = { model: GraphModel; error?: undefined } | { model?: undefined; error: unknown }

export function CodeMap({
  snapshot,
  repositoryName,
  onContextChange,
  onRetry,
  graphRef,
  complexity,
  onComplexityChange,
  hintAllowed = true,
}: CodeMapProps) {
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
  const hint = useMapHint()
  const card = useFileCard(built.model)

  const error = built.error ?? drawError
  if (error) {
    return (
      <div className={styles.center}>
        <ErrorState
          title="Could not draw the code map"
          cause="The analysis loaded, but the map could not be laid out."
          next="Try again. If it happens again, report it with the details below."
          details={error instanceof Error ? (error.stack ?? error.message) : String(error)}
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
    <div className={styles.map} ref={card.area}>
      <GraphCanvas
        ref={graphRef}
        model={model}
        onContextChange={onContextChange}
        onError={setDrawError}
        complexity={complexity}
        onHover={card.hover}
        onInteract={(kind) => {
          card.hide()
          hint.interacted(kind)
        }}
      />
      {card.shown && <FileCard {...card.shown} />}
      <MapLegend
        complexity={complexity}
        onComplexityChange={onComplexityChange}
        summary={`${model.root.fileCount} files, ${model.root.classCount} classes`}
      />
      {hint.due && hintAllowed && <MapHint onDismiss={hint.dismiss} />}
    </div>
  )
}

/** The card for the file under the pointer, once the pointer has rested on it. */
function useFileCard(model: GraphModel | undefined) {
  const area = useRef<HTMLDivElement>(null)
  const timer = useRef<ReturnType<typeof setTimeout> | null>(null)
  const [shown, setShown] = useState<{ facts: FileFacts; x: number; y: number } | null>(null)

  const facts = useMemo(() => {
    if (!model) return null
    const levels = modelComplexityLevels(model)
    const imports = new Map<GraphNode, number>()
    const usedBy = new Map<GraphNode, number>()
    for (const e of model.edges) {
      if (e.kind !== 'import') continue
      imports.set(e.source, (imports.get(e.source) ?? 0) + 1)
      usedBy.set(e.target, (usedBy.get(e.target) ?? 0) + 1)
    }
    return (node: GraphNode): FileFacts => ({
      node,
      level: levels[node.order] ?? -1,
      imports: imports.get(node) ?? 0,
      usedBy: usedBy.get(node) ?? 0,
    })
  }, [model])

  const cancel = () => {
    if (timer.current) clearTimeout(timer.current)
    timer.current = null
  }
  useEffect(() => cancel, [])

  const hover = (node: GraphNode | null, at: { x: number; y: number }) => {
    cancel()
    setShown(null)
    if (!node || node.kind !== 'file' || !facts) return
    timer.current = setTimeout(() => {
      const bounds = area.current?.getBoundingClientRect()
      const right = bounds ? bounds.width : Infinity
      const x = at.x + CARD_OFFSET + CARD_WIDTH > right ? at.x - CARD_OFFSET - CARD_WIDTH : at.x + CARD_OFFSET
      setShown({ facts: facts(node), x: Math.max(8, x), y: at.y + CARD_OFFSET })
    }, CARD_DELAY_MS)
  }
  const hide = () => {
    cancel()
    setShown(null)
  }
  return { area, shown, hover, hide }
}
