// Development-only frame benchmark: add `?bench` to a dev URL showing a map.
//
// Drives the camera from the whole repository down into its most
// class-heavy file and back out, then pans, one step per animation frame,
// and writes draw-time and frame-interval statistics to the page.

import { anchoredAt } from './camera'
import type { GraphNode } from './model'
import type { GraphRenderer } from './renderer'

const nextFrame = () => new Promise<number>((resolve) => requestAnimationFrame(resolve))

function stats(values: number[]) {
  const sorted = [...values].sort((a, b) => a - b)
  const at = (q: number) => sorted[Math.min(sorted.length - 1, Math.floor(q * sorted.length))]
  const mean = values.reduce((a, b) => a + b, 0) / values.length
  return `mean ${mean.toFixed(2)} p95 ${at(0.95).toFixed(2)} max ${at(1).toFixed(2)}`
}

export async function runBench(renderer: GraphRenderer): Promise<string> {
  // Descend by class count to a busy file.
  let target: GraphNode = renderer.model.root
  while (target.kind !== 'file' && target.children.length) {
    target = target.children.reduce((a, b) => (b.classCount > a.classCount ? b : a))
  }
  const wx = target.x + target.w / 2
  const wy = target.y + target.h / 2
  const { width, height } = renderer.viewport
  renderer.fit(false)
  await nextFrame()

  const draw: number[] = []
  const interval: number[] = []
  let last = await nextFrame()
  const sample = async () => {
    renderer.invalidate()
    const now = await nextFrame()
    await nextFrame() // the renderer's own frame runs in this one
    draw.push(renderer.lastFrameMs)
    interval.push(now - last)
    last = now
  }

  const k0 = renderer.camera.k
  const k1 = Math.min(renderer.maxK, (Math.min(width, height) / Math.max(target.w, target.h)) * 3)
  const steps = 180
  const zoomTo = (t: number) => {
    const k = k0 * Math.pow(k1 / k0, t)
    const [sx, sy] = [width / 2, height / 2]
    // Travel toward the target while zooming, as a user would.
    const cx = renderer.model.root.x + renderer.model.root.w / 2
    const cy = renderer.model.root.y + renderer.model.root.h / 2
    renderer.camera = anchoredAt(renderer.viewport, k, cx + (wx - cx) * Math.min(1, t * 1.5), cy + (wy - cy) * Math.min(1, t * 1.5), sx, sy)
  }
  for (let i = 0; i <= steps; i++) {
    zoomTo(i / steps)
    await sample()
  }
  for (let i = steps; i >= 0; i--) {
    zoomTo(i / steps)
    await sample()
  }
  zoomTo(0.55)
  for (let i = 0; i < 60; i++) {
    renderer.panBy(12, 4)
    await sample()
  }

  return `frames ${draw.length} | draw ms ${stats(draw)} | interval ms ${stats(interval)} | nodes ${renderer.model.nodes.length} edges ${renderer.model.edges.length}`
}
