// The renderer's interaction logic, run without a canvas (drawing is skipped).

import { describe, expect, it, vi } from 'vitest'

import type { GraphResponse } from '../bridge'
import { toScreenX, toScreenY, toWorld } from './camera'
import sherlock from './fixtures/sherlock.graph.json'
import { layoutGraph } from './layout'
import { buildGraphModel, type GraphNode } from './model'
import { capEdgeGroups, EDGE_BUDGET, GraphRenderer } from './renderer'
import type { GraphTheme } from './theme'

function setup(reducedMotion = true) {
  const model = buildGraphModel(sherlock as GraphResponse, 'sherlock')
  layoutGraph(model)
  const onContextChange = vi.fn()
  const onSelectionChange = vi.fn()
  const r = new GraphRenderer(model, {
    canvas: null,
    theme: {} as GraphTheme,
    reducedMotion,
    callbacks: { onContextChange, onSelectionChange },
  })
  r.resize(1200, 750)
  return { model, r, onContextChange, onSelectionChange }
}

const centre = (r: GraphRenderer, n: GraphNode): [number, number] => [
  toScreenX(r.camera, r.viewport, n.x + n.w / 2),
  toScreenY(r.camera, r.viewport, n.y + n.h / 2),
]
const labels = (path: GraphNode[]) => path.map((n) => n.label)

describe('GraphRenderer', () => {
  it('starts fitted to the whole repository', () => {
    const { model, r } = setup()
    expect(model.root.w * r.camera.k).toBeLessThanOrEqual(1200)
    expect(r.camera.x).toBeCloseTo(model.root.x + model.root.w / 2)
    expect(r.contextPath()).toEqual([])
  })

  it('focuses a node, and the breadcrumb context follows', () => {
    const { model, r, onContextChange } = setup()
    const cls = model.byId.get('class:sherlock_project/notify.py::QueryNotifyPrint')!
    r.focus(cls)
    expect(labels(r.contextPath())).toEqual(['sherlock_project', 'notify.py', 'QueryNotifyPrint'])
    expect(labels(onContextChange.mock.lastCall![0])).toEqual(['sherlock_project', 'notify.py', 'QueryNotifyPrint'])
    // The node now fills the view, centred.
    expect(r.camera.x).toBeCloseTo(cls.x + cls.w / 2)
    expect(Math.max((cls.w * r.camera.k) / 1200, (cls.h * r.camera.k) / 750)).toBeGreaterThan(0.8)
  })

  it('navigates back up to an ancestor', () => {
    const { model, r } = setup()
    r.focus(model.byId.get('class:sherlock_project/notify.py::QueryNotifyPrint')!)
    r.focus(model.byId.get('dir:sherlock_project')!)
    expect(labels(r.contextPath())).toEqual(['sherlock_project'])
    r.focus(model.root)
    expect(r.contextPath()).toEqual([])
  })

  it('follows the camera once the user moves it', () => {
    const { model, r } = setup()
    r.focus(model.byId.get('dir:tests')!)
    expect(labels(r.contextPath())).toEqual(['tests'])
    // Any manual camera move unpins the focused node: the context becomes
    // whatever open node is under the view centre, here inside tests/.
    r.panBy(0, 0)
    expect(labels(r.contextPath()).slice(0, 1)).toEqual(['tests'])
    r.fit(false)
    expect(r.contextPath()).toEqual([])
  })

  it('hits only what is visible: a file at overview, its class once zoomed in', () => {
    const { model, r } = setup()
    const cls = model.byId.get('class:sherlock_project/notify.py::QueryNotifyPrint')!
    expect(r.hitTest(...centre(r, cls))?.id).toBe('file:sherlock_project/notify.py')
    expect(r.hitTest(2, 2)).toBeNull() // outside the map
    r.focus(model.byId.get('file:sherlock_project/notify.py')!)
    // Zoomed in, the class header hits the class and a member row hits the member.
    const header: [number, number] = [centre(r, cls)[0], toScreenY(r.camera, r.viewport, cls.y + cls.pad + cls.header / 2)]
    expect(r.hitTest(...header)?.id).toBe(cls.id)
    expect(r.hitTest(...centre(r, cls.children[0]))?.id).toBe(cls.children[0].id)
  })

  it('selects on click, reports it, and uses it as the breadcrumb context', () => {
    const { model, r, onSelectionChange } = setup()
    const file = model.byId.get('file:tests/test_ux.py')!
    r.click(...centre(r, file))
    expect(r.selected).toBe(file)
    expect(onSelectionChange).toHaveBeenLastCalledWith(file)
    expect(labels(r.contextPath())).toEqual(['tests', 'test_ux.py'])
    r.select(null)
    expect(onSelectionChange).toHaveBeenLastCalledWith(null)
  })

  it('double-click selects and focuses', () => {
    const { model, r } = setup()
    const file = model.byId.get('file:sherlock_project/sites.py')!
    r.doubleClick(...centre(r, file))
    expect(r.selected).toBe(file)
    expect(r.camera.x).toBeCloseTo(file.x + file.w / 2)
  })

  it('keeps the pointer anchored while zooming with the wheel', () => {
    const { r } = setup()
    const [wx, wy] = toWorld(r.camera, r.viewport, 300, 200)
    r.wheel(0, -300, 0, false, 300, 200)
    expect(r.camera.k).toBeGreaterThan(1)
    expect(toScreenX(r.camera, r.viewport, wx)).toBeCloseTo(300)
    expect(toScreenY(r.camera, r.viewport, wy)).toBeCloseTo(200)
  })

  it('pans on two-finger trackpad scroll and zooms on pinch', () => {
    const { r } = setup()
    const before = { ...r.camera }
    r.wheel(12, 7.5, 0, false, 600, 375)
    expect(r.camera.k).toBe(before.k)
    expect(r.camera.x).toBeGreaterThan(before.x)
    r.wheel(0, -20, 0, true, 600, 375)
    expect(r.camera.k).toBeGreaterThan(before.k)
  })

  it('keeps zoom within limits', () => {
    const { r } = setup()
    for (let i = 0; i < 60; i++) r.zoomOut(false)
    expect(r.camera.k).toBeCloseTo(r.minK)
    for (let i = 0; i < 200; i++) r.zoomIn(false)
    expect(r.camera.k).toBeCloseTo(r.maxK)
  })

  it('animates camera moves unless reduced motion is on', () => {
    const smooth = setup(false)
    smooth.r.zoomIn()
    expect(smooth.r.animating).toBe(true)
    const k0 = smooth.r.camera.k
    smooth.r.step(performance.now() + 10_000, 16)
    expect(smooth.r.animating).toBe(false)
    expect(smooth.r.camera.k).toBeCloseTo(k0 * 1.6)

    const reduced = setup(true)
    reduced.r.zoomIn()
    expect(reduced.r.animating).toBe(false)
  })

  it('keeps the view when the window is resized', () => {
    const { model, r } = setup()
    r.focus(model.byId.get('dir:tests')!)
    const before = { ...r.camera }
    r.resize(900, 600)
    expect(r.camera).toEqual(before)
  })
})

describe('capEdgeGroups', () => {
  const group = (score: number, emphasized = false) => ({ score, emphasized })

  it('draws at most the cap, strongest first by the existing score', () => {
    const groups = Array.from({ length: 1000 }, (_, i) => group(i))
    const kept = capEdgeGroups(groups, EDGE_BUDGET)
    expect(kept).toHaveLength(EDGE_BUDGET)
    expect(kept.map((g) => g.score)).toEqual(Array.from({ length: EDGE_BUDGET }, (_, i) => 999 - i))
  })

  it('keeps hovered or selected relationships even when the cap is full', () => {
    const weak = group(0, true)
    const groups = [...Array.from({ length: 50 }, (_, i) => group(100 + i)), weak, group(1, true)]
    const kept = capEdgeGroups(groups, 10)
    expect(kept.filter((g) => !g.emphasized)).toHaveLength(10)
    expect(kept.filter((g) => g.emphasized)).toHaveLength(2)
    expect(kept).toContain(weak)
  })

  it('draws everything below the cap and leaves the input untouched', () => {
    const groups = [group(1), group(3), group(2)]
    expect(capEdgeGroups(groups, 10).map((g) => g.score)).toEqual([3, 2, 1])
    expect(groups.map((g) => g.score)).toEqual([1, 3, 2])
    expect(capEdgeGroups(groups, 0)).toEqual([])
  })
})
