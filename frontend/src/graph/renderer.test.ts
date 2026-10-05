// The renderer's interaction logic, run without a canvas (drawing is skipped).

import { describe, expect, it, vi } from 'vitest'

import type { GraphResponse } from '../bridge'
import { toScreenX, toScreenY, toWorld } from './camera'
import sherlock from './fixtures/sherlock.graph.json'
import { layoutGraph } from './layout'
import { buildGraphModel, contains, type GraphNode } from './model'
import { openness } from './semantic'
import { capEdgeGroups, EDGE_BUDGET, GraphRenderer, HOVER_EDGE_CAP, SELECTION_EDGE_CAP, shownAtRest } from './renderer'
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
    // The node is centred, and fills the view unless that would zoom past
    // the point where its contents read at full size.
    expect(r.camera.x).toBeCloseTo(cls.x + cls.w / 2)
    const fill = Math.max((cls.w * r.camera.k) / 1200, (cls.h * r.camera.k) / 750)
    if (fill < 0.8) expect(r.camera.k).toBeCloseTo(r.maxKFor(cls))
    expect(cls.rowHeight * r.camera.k).toBeGreaterThanOrEqual(20)
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

  it('hovering a dimmed node keeps the focus on the selection until it is selected', () => {
    const { model, r } = setup()
    const selected = model.byId.get('file:tests/test_ux.py')!
    const linked = (n: GraphNode) =>
      model.edges.some((e) => (contains(n, e.source) && contains(selected, e.target)) || (contains(n, e.target) && contains(selected, e.source)))
    const files = model.nodes.filter((n) => n.kind === 'file' && n !== selected)
    const unrelated = files.find((n) => !linked(n))!
    const related = files.find(linked)!

    r.select(selected)
    r.pointerMove(...centre(r, unrelated))
    expect(r.hovered).toBe(unrelated)
    expect(r.relationFocus).toBe(selected)

    // A node that is lit by the selection still shows its own relationships on hover.
    r.pointerMove(...centre(r, related))
    expect(r.relationFocus).toBe(related)

    // Without a selection nothing is dimmed, so hover shows relationships as before.
    r.select(null)
    r.pointerMove(...centre(r, unrelated))
    expect(r.relationFocus).toBe(unrelated)

    // Selecting the dimmed node reveals its relationships.
    r.select(selected)
    r.click(...centre(r, unrelated))
    expect(r.relationFocus).toBe(unrelated)
  })

  it('hovers nothing in the gaps of an open container, but still its header', () => {
    const { model, r } = setup()
    const dir = model.byId.get('dir:sherlock_project')!
    const screen = (wx: number, wy: number): [number, number] => [toScreenX(r.camera, r.viewport, wx), toScreenY(r.camera, r.viewport, wy)]
    // Inside the directory's bottom padding, below all of its files.
    const gap = screen(dir.x + dir.w / 2, dir.y + dir.h - dir.pad / 2)
    expect(r.hitTest(...gap)).toBe(dir)
    r.pointerMove(...gap)
    expect(r.hovered).toBeNull()
    expect(r.relationFocus).toBeNull()

    r.pointerMove(...screen(dir.x + dir.w / 2, dir.y + dir.header / 2))
    expect(r.hovered).toBe(dir)

    // Clicking a gap still selects the container.
    r.click(...gap)
    expect(r.selected).toBe(dir)
  })

  it('marks what the selection is connected to, on the boxes its arrows meet', () => {
    const { model, r } = setup()
    const selected = model.byId.get('file:tests/test_ux.py')!
    r.select(selected)
    const lit = r.relatedHighlights()
    expect(lit.length).toBeGreaterThan(0)
    const linked = model.edges.filter((e) => contains(selected, e.source) || contains(selected, e.target)).map((e) => (contains(selected, e.source) ? e.target : e.source))
    for (const n of lit) {
      expect(contains(n, selected) || contains(selected, n)).toBe(false)
      // Each marked box is a related node or the region standing in for one.
      expect(linked.some((l) => contains(n, l))).toBe(true)
    }
    r.select(null)
    expect(r.relatedHighlights()).toEqual([])
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
    const k = r.camera.k
    expect(k).toBeLessThanOrEqual(r.maxK + 1e-9)
    r.zoomIn(false)
    expect(r.camera.k).toBe(k)
  })

  it('keeps the zoom range useful: the map stays sizeable, member rows readable but not huge', () => {
    const { model, r } = setup()
    r.fit(false)
    const fit = r.camera.k
    expect(r.minK).toBeGreaterThanOrEqual(fit * 0.5)
    const rows = model.nodes.filter((n) => n.kind === 'class' && n.children.length).map((n) => n.rowHeight)
    const minRow = Math.min(...rows)
    expect(minRow * r.maxK).toBeGreaterThanOrEqual(20)
    expect(minRow * r.maxK).toBeLessThan(40)
  })

  it('stops zooming into a small class sooner than into a large one', () => {
    const { model, r } = setup()
    const classes = model.nodes.filter((n) => n.kind === 'class' && n.children.length)
    const roomy = classes.reduce((a, b) => (b.rowHeight > a.rowHeight ? b : a))
    const dense = classes.reduce((a, b) => (b.rowHeight < a.rowHeight ? b : a))
    expect(r.maxKFor(roomy)).toBeLessThan(r.maxKFor(dense))
    // Every container can still be zoomed to fully open.
    for (const n of model.nodes) if (n.kind !== 'member' && n.children.length) expect(openness(n, r.maxKFor(n))).toBeGreaterThan(0.999)
    // Wheel in on the roomy class until it stops: its rows read at full size, not oversized.
    r.focus(roomy, false)
    for (let i = 0; i < 100; i++) r.wheel(0, -120, 0, false, ...centre(r, roomy.children[0]))
    const row = roomy.rowHeight * r.camera.k
    expect(row).toBeGreaterThanOrEqual(20)
    expect(row).toBeLessThan(Math.max(30, roomy.rowHeight * r.maxKFor(model.root) * 0.5))
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

describe('shownAtRest', () => {
  it('draws lines between directories at rest, and lines to files or symbols only on hover or selection', () => {
    const model = buildGraphModel(sherlock as GraphResponse, 'sherlock')
    const dir = model.byId.get('dir:sherlock_project')!
    const tests = model.byId.get('dir:tests')!
    const file = model.byId.get('file:tests/test_ux.py')!
    const cls = model.byId.get('class:sherlock_project/notify.py::QueryNotifyPrint')!
    expect(shownAtRest(dir, tests)).toBe(true)
    expect(shownAtRest(dir, file)).toBe(false)
    expect(shownAtRest(file, dir)).toBe(false)
    expect(shownAtRest(cls, file)).toBe(false)
  })
})

describe('capEdgeGroups', () => {
  const group = (score: number, emphasized = false) => ({ score, emphasized })

  it('draws at most the cap, strongest first by the existing score', () => {
    const groups = Array.from({ length: 1000 }, (_, i) => group(i))
    const kept = capEdgeGroups(groups, EDGE_BUDGET, HOVER_EDGE_CAP)
    expect(kept).toHaveLength(EDGE_BUDGET)
    expect(kept.map((g) => g.score)).toEqual(Array.from({ length: EDGE_BUDGET }, (_, i) => 999 - i))
  })

  it('draws hovered/selected relationships even when the main cap is full', () => {
    const weak = group(0, true)
    const groups = [...Array.from({ length: 50 }, (_, i) => group(100 + i)), weak, group(1, true)]
    const kept = capEdgeGroups(groups, 10, HOVER_EDGE_CAP)
    expect(kept.filter((g) => !g.emphasized)).toHaveLength(10)
    expect(kept.filter((g) => g.emphasized)).toHaveLength(2)
    expect(kept).toContain(weak)
  })

  it("caps a high-degree node's relationships separately, keeping the strongest", () => {
    const hub = Array.from({ length: 500 }, (_, i) => group(i, true))
    const others = Array.from({ length: 500 }, (_, i) => group(i))
    const kept = capEdgeGroups([...hub, ...others], EDGE_BUDGET, HOVER_EDGE_CAP)
    const emphasized = kept.filter((g) => g.emphasized)
    expect(emphasized).toHaveLength(HOVER_EDGE_CAP)
    expect(emphasized.map((g) => g.score)).toEqual(Array.from({ length: HOVER_EDGE_CAP }, (_, i) => 499 - i))
    // Dropped relationships are not drawn as ordinary edges instead.
    expect(kept.filter((g) => !g.emphasized)).toHaveLength(EDGE_BUDGET)
    expect(capEdgeGroups(hub, EDGE_BUDGET, SELECTION_EDGE_CAP)).toHaveLength(SELECTION_EDGE_CAP)
    expect(SELECTION_EDGE_CAP).toBeGreaterThan(HOVER_EDGE_CAP)
  })

  it('draws everything below the caps and leaves the input untouched', () => {
    const groups = [group(1), group(3), group(2)]
    expect(capEdgeGroups(groups, 10, 10).map((g) => g.score)).toEqual([3, 2, 1])
    expect(groups.map((g) => g.score)).toEqual([1, 3, 2])
    expect(capEdgeGroups(groups, 0, 0)).toEqual([])
  })
})
