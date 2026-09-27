// Mounts the code map renderer on a <canvas> and forwards input to it.
//
// React owns only the element and the callbacks. Camera movement, hover and
// animation happen inside GraphRenderer without React renders; React hears
// about navigation context and selection changes, which are rare.

import { useEffect, useImperativeHandle, useLayoutEffect, useRef, useState, type Ref } from 'react'

import styles from './GraphCanvas.module.css'
import type { GraphModel, GraphNode } from './model'
import { GraphRenderer } from './renderer'
import { readTheme } from './theme'

export interface GraphHandle {
  zoomIn(): void
  zoomOut(): void
  fit(): void
  /** Clear the selection and move to a node (the root fits the whole map). */
  navigate(id: string): void
}

interface GraphCanvasProps {
  model: GraphModel
  onContextChange: (path: GraphNode[]) => void
  onError: (error: unknown) => void
  ref?: Ref<GraphHandle>
}

/** Movement (px) before a press becomes a drag instead of a click. */
const DRAG_THRESHOLD = 3
const ARROW_PAN = 80

const KIND_NAMES = { root: 'repository', dir: 'directory', file: 'file', class: 'class', member: 'member' } as const

export function GraphCanvas({ model, onContextChange, onError, ref }: GraphCanvasProps) {
  const canvasRef = useRef<HTMLCanvasElement>(null)
  const rendererRef = useRef<GraphRenderer | null>(null)
  const [announcement, setAnnouncement] = useState('')

  // Latest callbacks without re-creating the renderer when they change.
  const callbacks = useRef({ onContextChange, onError })
  useLayoutEffect(() => {
    callbacks.current = { onContextChange, onError }
  })

  useImperativeHandle(
    ref,
    () => ({
      zoomIn: () => rendererRef.current?.zoomIn(),
      zoomOut: () => rendererRef.current?.zoomOut(),
      fit: () => {
        const r = rendererRef.current
        if (!r) return
        r.select(null)
        r.fit()
      },
      navigate: (id) => {
        const r = rendererRef.current
        const node = r?.model.byId.get(id)
        if (!r || !node) return
        r.select(null)
        r.focus(node)
      },
    }),
    [],
  )

  useEffect(() => {
    const canvas = canvasRef.current
    if (!canvas) return
    const motion = window.matchMedia?.('(prefers-reduced-motion: reduce)')

    let renderer: GraphRenderer
    try {
      renderer = new GraphRenderer(model, {
        canvas,
        theme: readTheme(canvas),
        reducedMotion: motion?.matches ?? false,
        callbacks: {
          onContextChange: (path) => callbacks.current.onContextChange(path),
          onError: (e) => callbacks.current.onError(e),
          onSelectionChange: (node) =>
            setAnnouncement(node ? `Selected ${KIND_NAMES[node.kind]} ${node.label}` : ''),
        },
      })
    } catch (e) {
      callbacks.current.onError(e)
      return
    }
    rendererRef.current = renderer

    // ---- Size ----
    const resize = () => {
      const rect = canvas.getBoundingClientRect()
      renderer.resize(rect.width, rect.height, window.devicePixelRatio || 1)
    }
    resize()
    const observer = typeof ResizeObserver === 'function' ? new ResizeObserver(resize) : null
    observer?.observe(canvas)

    // ---- Pointer ----
    let press: { id: number; x: number; y: number; lastX: number; lastY: number; dragging: boolean } | null = null
    const local = (e: MouseEvent) => {
      const r = canvas.getBoundingClientRect()
      return [e.clientX - r.left, e.clientY - r.top] as const
    }
    const onPointerDown = (e: PointerEvent) => {
      if (e.button !== 0 && e.button !== 1) return
      const [x, y] = local(e)
      press = { id: e.pointerId, x, y, lastX: x, lastY: y, dragging: false }
      canvas.setPointerCapture(e.pointerId)
    }
    const onPointerMove = (e: PointerEvent) => {
      const [x, y] = local(e)
      if (press && press.id === e.pointerId) {
        if (!press.dragging && Math.hypot(x - press.x, y - press.y) > DRAG_THRESHOLD) {
          press.dragging = true
          canvas.dataset.dragging = 'true'
        }
        if (press.dragging) {
          renderer.panBy(x - press.lastX, y - press.lastY)
          press.lastX = x
          press.lastY = y
          return
        }
      }
      renderer.pointerMove(x, y)
    }
    const onPointerUp = (e: PointerEvent) => {
      if (!press || press.id !== e.pointerId) return
      const [x, y] = local(e)
      if (!press.dragging && e.button === 0) renderer.click(x, y)
      press = null
      delete canvas.dataset.dragging
    }
    const onPointerCancel = () => {
      press = null
      delete canvas.dataset.dragging
    }
    const onDoubleClick = (e: MouseEvent) => {
      const [x, y] = local(e)
      renderer.doubleClick(x, y)
    }
    const onWheel = (e: WheelEvent) => {
      e.preventDefault()
      const [x, y] = local(e)
      renderer.wheel(e.deltaX, e.deltaY, e.deltaMode, e.ctrlKey, x, y)
    }

    // ---- Keyboard (when the map has focus) ----
    const onKeyDown = (e: KeyboardEvent) => {
      if (e.ctrlKey || e.metaKey || e.altKey) return
      const handled = (() => {
        switch (e.key) {
          case '+':
          case '=':
            renderer.zoomIn()
            return true
          case '-':
          case '_':
            renderer.zoomOut()
            return true
          case '0':
            renderer.select(null)
            renderer.fit()
            return true
          case 'Escape':
            if (!renderer.selected) return false
            renderer.select(null)
            return true
          case 'Enter':
            if (!renderer.selected) return false
            renderer.focus(renderer.selected)
            return true
          case 'ArrowLeft':
            renderer.panBy(ARROW_PAN, 0)
            return true
          case 'ArrowRight':
            renderer.panBy(-ARROW_PAN, 0)
            return true
          case 'ArrowUp':
            renderer.panBy(0, ARROW_PAN)
            return true
          case 'ArrowDown':
            renderer.panBy(0, -ARROW_PAN)
            return true
        }
        return false
      })()
      if (handled) e.preventDefault()
    }

    const onPointerLeave = () => renderer.pointerLeave()
    const onMotionChange = () => renderer.setReducedMotion(motion?.matches ?? false)
    const onFonts = () => renderer.fontsChanged()

    canvas.addEventListener('pointerdown', onPointerDown)
    canvas.addEventListener('pointermove', onPointerMove)
    canvas.addEventListener('pointerup', onPointerUp)
    canvas.addEventListener('pointercancel', onPointerCancel)
    canvas.addEventListener('pointerleave', onPointerLeave)
    canvas.addEventListener('dblclick', onDoubleClick)
    canvas.addEventListener('wheel', onWheel, { passive: false })
    canvas.addEventListener('keydown', onKeyDown)
    motion?.addEventListener?.('change', onMotionChange)
    document.fonts?.addEventListener?.('loadingdone', onFonts)
    void document.fonts?.ready.then(onFonts)

    // Development only: scripted checks and benchmarks drive the renderer directly.
    if (import.meta.env.DEV) (window as { __telescode?: GraphRenderer }).__telescode = renderer
    if (import.meta.env.DEV && new URLSearchParams(window.location.search).has('bench')) {
      void import('./bench').then(async ({ runBench }) => {
        const result = await runBench(renderer)
        document.body.dataset.bench = result
        console.info('[bench]', result)
      })
    }

    return () => {
      observer?.disconnect()
      canvas.removeEventListener('pointerdown', onPointerDown)
      canvas.removeEventListener('pointermove', onPointerMove)
      canvas.removeEventListener('pointerup', onPointerUp)
      canvas.removeEventListener('pointercancel', onPointerCancel)
      canvas.removeEventListener('pointerleave', onPointerLeave)
      canvas.removeEventListener('dblclick', onDoubleClick)
      canvas.removeEventListener('wheel', onWheel)
      canvas.removeEventListener('keydown', onKeyDown)
      motion?.removeEventListener?.('change', onMotionChange)
      document.fonts?.removeEventListener?.('loadingdone', onFonts)
      renderer.destroy()
      if (rendererRef.current === renderer) rendererRef.current = null
    }
  }, [model])

  return (
    <>
      <canvas
        ref={canvasRef}
        className={styles.canvas}
        tabIndex={0}
        role="application"
        aria-roledescription="code map"
        aria-label="Code map. Scroll to zoom, drag to pan, click to select, double-click to focus. Plus and minus zoom, 0 fits, Enter focuses the selection."
      />
      <p className="visually-hidden" role="status" aria-live="polite">
        {announcement}
      </p>
    </>
  )
}
