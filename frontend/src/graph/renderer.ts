// Draws the code map on one <canvas> and owns everything that changes per
// frame: camera, animation, hover, selection, and the navigation context the
// breadcrumbs show. React never re-renders during interaction; it only hears
// about context and selection changes.
//
// Frames are drawn on demand (input, animation, resize), never in an idle
// loop. Works without a 2D context too (tests), minus the drawing.

import {
  anchoredAt,
  clamp,
  easeInOut,
  fitBox,
  flight,
  flightDuration,
  toScreenX,
  toScreenY,
  toWorld,
  zoomAround,
  type Box,
  type Camera,
  type Flight,
  type Viewport,
} from './camera'
import { WORLD } from './layout'
import { ancestors, contains, type GraphEdge, type GraphModel, type GraphNode } from './model'
import { fullyOpenK, LodFrame, settle, smoothstep, type StandIn } from './semantic'
import { TextMeasurer } from './text'
import type { GraphTheme } from './theme'

export interface RendererCallbacks {
  /** The path from the root (exclusive) to the current navigation context. */
  onContextChange?: (path: GraphNode[]) => void
  onSelectionChange?: (node: GraphNode | null) => void
  onError?: (error: unknown) => void
}

export interface RendererOptions {
  canvas: HTMLCanvasElement | null
  theme: GraphTheme
  reducedMotion: boolean
  callbacks: RendererCallbacks
  /** Camera to start from, e.g. when the data is reloaded. */
  camera?: Camera
}

/** Margin around a focused node, as a fraction of its size. */
const FOCUS_MARGIN = 0.08
/** Mouse-wheel zoom speed per pixel of scroll. */
const WHEEL_ZOOM = 0.0022
/** Smoothing time constant for wheel zoom, ms. Short: direct, not floaty. */
const WHEEL_TAU = 55
/** Most edge groups drawn at once, not counting hovered/selected relationships. */
export const EDGE_BUDGET = 320
/** Most emphasized relationships drawn for a hovered node, on top of EDGE_BUDGET. */
export const HOVER_EDGE_CAP = 40
/** Most emphasized relationships drawn for the selection (a deliberate choice, so more). */
export const SELECTION_EDGE_CAP = 120
/** Opacity of the outline on what the selection is connected to, relative to the selection's. */
const RELATED_STRENGTH = 0.45
/** Openness over which a closed node's caption ("12 files") fades out: as its children fade in. */
const CAPTION_FADE = [0.35, 0.6] as const
/** Button zoom step. */
const STEP = 1.6
/** Zoom range, relative to the scale that fits the whole repository. */
const MIN_ZOOM = 0.6
/** Zooming in is always allowed this far, whatever is in view. */
const MAX_ZOOM_FLOOR = 2
/**
 * Zooming in stops once what is being zoomed into is fully open and reads at
 * full size: its smallest member row this tall on screen, px (member text is
 * full size from about 20px)...
 */
const MAX_ROW_PX = 24
/** ...and its smallest box this large, px (labels are full size from about this). */
const MAX_LEAF_PX = 120

const raf: (cb: FrameRequestCallback) => number =
  typeof requestAnimationFrame === 'function' ? requestAnimationFrame : (cb) => setTimeout(() => cb(performance.now()), 16) as unknown as number
const caf: (id: number) => void = typeof cancelAnimationFrame === 'function' ? cancelAnimationFrame : clearTimeout

interface WheelZoom {
  targetK: number
  sx: number
  sy: number
  wx: number
  wy: number
}

interface EdgeGroup {
  /** The boxes the group's lines run between, exactly as drawn. */
  s: GraphNode
  t: GraphNode
  count: number
  /** How much these stand-ins are what is drawn (see LodFrame.standIns), and for class relationships, their visibility. */
  alpha: number
  emphasized: boolean
}

interface EdgeCandidate {
  x0: number
  y0: number
  cx: number
  cy: number
  x1: number
  y1: number
  len: number
  alpha: number
  /** Opacity of an emphasized line: its stand-ins' weight alone. */
  weight: number
  count: number
  emphasized: boolean
  score: number
}

interface FlightAnim {
  flight: Flight
  start: number
  duration: number
}

export class GraphRenderer {
  readonly model: GraphModel
  camera: Camera = { x: WORLD.width / 2, y: WORLD.height / 2, k: 1 }
  viewport: Viewport = { width: 0, height: 0 }
  hovered: GraphNode | null = null
  selected: GraphNode | null = null
  /** Set by explicit navigation (focus); cleared when the user moves the camera. */
  pinned: GraphNode | null = null

  private readonly ctx: CanvasRenderingContext2D | null
  private readonly canvas: HTMLCanvasElement | null
  private readonly callbacks: RendererCallbacks
  private readonly lod: LodFrame
  private readonly text: TextMeasurer | null
  private theme: GraphTheme
  private reducedMotion: boolean
  private dpr = 1
  private frameId = 0
  private lastTime = 0
  private wheelZoom: WheelZoom | null = null
  private flightAnim: FlightAnim | null = null
  private pointer: { x: number; y: number } | null = null
  private contextKey = ''
  /** Nodes related to the focus (see `relationFocus`) and to the selection, by order. */
  private related = new Set<number>()
  private selectionRelated = new Set<number>()
  /** The hovered node is dimmed against the selection, so it does not take the focus. */
  private hoverMuted = false
  private hasCamera = false
  private failed = false
  /** Per node (by order), the scale at which everything in its subtree is open and full size. */
  private readonly fullK: Float64Array
  private readonly importEdges: GraphEdge[]
  /** Class edges by the file of their source class: only visible files' are looked at. */
  private readonly classEdgesByFile = new Map<GraphNode, GraphEdge[]>()
  /** Duration of the last drawn frame, ms (for measurement). */
  lastFrameMs = 0

  constructor(model: GraphModel, options: RendererOptions) {
    this.model = model
    this.canvas = options.canvas
    this.ctx = options.canvas?.getContext('2d') ?? null
    this.theme = options.theme
    this.reducedMotion = options.reducedMotion
    this.callbacks = options.callbacks
    this.lod = new LodFrame(model)
    this.text = this.ctx ? new TextMeasurer(this.ctx, options.theme.font) : null
    if (options.camera) {
      this.camera = { ...options.camera }
      this.hasCamera = true
    }

    // Children come after their parent in depth-first order, so a backwards
    // pass sees each subtree before the node that holds it.
    const count = model.nodes.length
    this.fullK = new Float64Array(count)
    for (let i = count - 1; i >= 0; i--) {
      const n = model.nodes[i]
      if (n.kind === 'member') continue // covered by its class's row height
      let k = fullyOpenK(n)
      if (n.kind === 'class' && n.children.length) k = Math.max(k, MAX_ROW_PX / n.rowHeight)
      if (n.children.length === 0 && n.w > 0 && n.h > 0) k = Math.max(k, MAX_LEAF_PX / Math.min(n.w, n.h))
      this.fullK[i] = Math.max(this.fullK[i], k)
      if (n.parent) this.fullK[n.parent.order] = Math.max(this.fullK[n.parent.order], this.fullK[i])
    }

    this.importEdges = model.edges.filter((e) => e.kind === 'import')
    for (const e of model.edges) {
      if (e.kind !== 'class' || !e.source.parent) continue
      const list = this.classEdgesByFile.get(e.source.parent)
      if (list) list.push(e)
      else this.classEdgesByFile.set(e.source.parent, [e])
    }
  }

  // ---- Setup ---------------------------------------------------------------

  resize(width: number, height: number, dpr = 1) {
    const first = !this.hasCamera && width > 0 && height > 0
    this.viewport = { width, height }
    this.dpr = dpr
    if (this.canvas) {
      this.canvas.width = Math.max(1, Math.round(width * dpr))
      this.canvas.height = Math.max(1, Math.round(height * dpr))
    }
    if (first) {
      this.camera = this.fitCamera()
      this.hasCamera = true
    }
    this.camera.k = clamp(this.camera.k, this.minK, this.maxK)
    this.invalidate()
  }

  setTheme(theme: GraphTheme) {
    this.theme = theme
    this.text?.reset()
    this.invalidate()
  }

  setReducedMotion(reduced: boolean) {
    this.reducedMotion = reduced
  }

  /** Re-measure text, e.g. after web fonts load. */
  fontsChanged() {
    this.text?.reset()
    this.invalidate()
  }

  destroy() {
    if (this.frameId) caf(this.frameId)
    this.frameId = 0
  }

  // ---- Zoom limits -----------------------------------------------------------

  /** A little past the whole repository, never so far that it shrinks to a speck. */
  get minK(): number {
    return this.fitKRaw() * MIN_ZOOM
  }

  /** The furthest zoom anywhere: enough for the smallest things in the repository. */
  get maxK(): number {
    return this.maxKFor(this.model.root)
  }

  /**
   * Close enough to see a node's contents fully open and at full size, and
   * no closer: past that only the boxes grow, and small text in a huge box
   * reads worse. A small class stops early; a large one lets you in far
   * enough for its rows.
   */
  maxKFor(node: GraphNode): number {
    const o = (node.kind === 'member' ? node.parent! : node).order
    return Math.max(this.fitKRaw() * MAX_ZOOM_FLOOR, this.fullK[o])
  }

  /**
   * The zoom-in limit at a screen point, from what is there. Never below the
   * current zoom (or the zoom already headed for), so moving onto a smaller
   * node does not pull the camera back out; it only stops it going further.
   */
  private maxKAt(sx: number, sy: number, k = this.camera.k): number {
    const node = this.hitTest(sx, sy)
    const limit = node ? this.maxKFor(node) : this.maxK
    return Math.max(limit, Math.min(k, this.maxK))
  }

  private fitKRaw(): number {
    const { width, height } = this.viewport
    if (width <= 0 || height <= 0) return 1
    return Math.min(width / (WORLD.width * 1.06), height / (WORLD.height * 1.06))
  }

  private fitCamera(): Camera {
    const r = this.model.root
    return fitBox(this.viewport, r, 0.03, 1e-9, Infinity)
  }

  // ---- Camera commands -------------------------------------------------------

  fit(animate = true) {
    this.pinned = null
    this.flyTo(this.fitCamera(), animate)
  }

  zoomIn(animate = true) {
    this.zoomBy(STEP, animate)
  }

  zoomOut(animate = true) {
    this.zoomBy(1 / STEP, animate)
  }

  zoomBy(factor: number, animate = true) {
    this.pinned = null
    const { width, height } = this.viewport
    const from = this.targetCamera()
    const max = this.maxKAt(width / 2, height / 2, from.k)
    const target = zoomAround(from, this.viewport, factor, width / 2, height / 2, this.minK, max)
    this.flyTo(target, animate)
  }

  /** Move the camera to show a node whole, and make it the navigation context. */
  focus(node: GraphNode, animate = true) {
    this.pinned = node
    const camera = fitBox(this.viewport, node, FOCUS_MARGIN, this.minK, this.maxKFor(node))
    this.flyTo(camera, animate)
    this.emitContext()
  }

  panBy(dx: number, dy: number) {
    this.cancelAnimations()
    this.pinned = null
    this.camera = { ...this.camera, x: this.camera.x - dx / this.camera.k, y: this.camera.y - dy / this.camera.k }
    this.invalidate()
  }

  /** Wheel or trackpad input at screen point (sx, sy). */
  wheel(deltaX: number, deltaY: number, deltaMode: number, ctrlKey: boolean, sx: number, sy: number) {
    const px = deltaMode === 1 ? 16 : deltaMode === 2 ? this.viewport.height : 1
    const dx = deltaX * px
    const dy = deltaY * px
    this.pinned = null
    this.flightAnim = null

    if (ctrlKey) {
      // Trackpad pinch (browsers report it as ctrl + wheel): direct, no smoothing.
      this.wheelZoom = null
      this.camera = zoomAround(this.camera, this.viewport, Math.exp(-dy * 0.01), sx, sy, this.minK, this.maxKAt(sx, sy))
      this.invalidate()
      return
    }
    // Two-finger trackpad scroll pans; a mouse wheel (large, vertical-only steps) zooms.
    const trackpad = deltaMode === 0 && (deltaX !== 0 || Math.abs(dy) < 40 || !Number.isInteger(dy))
    if (trackpad) {
      this.panBy(-dx, -dy)
      return
    }

    const factor = Math.exp(-dy * WHEEL_ZOOM)
    if (this.reducedMotion) {
      this.camera = zoomAround(this.camera, this.viewport, factor, sx, sy, this.minK, this.maxKAt(sx, sy))
      this.invalidate()
      return
    }
    const z = this.wheelZoom
    if (z && Math.abs(z.sx - sx) < 2 && Math.abs(z.sy - sy) < 2) {
      z.targetK = clamp(z.targetK * factor, this.minK, this.maxKAt(sx, sy, z.targetK))
    } else {
      const [wx, wy] = toWorld(this.camera, this.viewport, sx, sy)
      this.wheelZoom = { targetK: clamp(this.camera.k * factor, this.minK, this.maxKAt(sx, sy)), sx, sy, wx, wy }
    }
    this.invalidate()
  }

  private targetCamera(): Camera {
    if (this.flightAnim) return this.flightAnim.flight.at(1)
    if (this.wheelZoom) {
      const z = this.wheelZoom
      return anchoredAt(this.viewport, z.targetK, z.wx, z.wy, z.sx, z.sy)
    }
    return this.camera
  }

  private flyTo(target: Camera, animate: boolean) {
    this.wheelZoom = null
    if (!animate || this.reducedMotion || this.viewport.width <= 0) {
      this.flightAnim = null
      this.camera = target
    } else {
      const f = flight(this.camera, target, this.viewport)
      this.flightAnim = { flight: f, start: performance.now(), duration: flightDuration(f) }
    }
    this.invalidate()
  }

  private cancelAnimations() {
    this.flightAnim = null
    this.wheelZoom = null
  }

  get animating(): boolean {
    return this.flightAnim !== null || this.wheelZoom !== null
  }

  // ---- Pointer ---------------------------------------------------------------

  pointerMove(sx: number, sy: number) {
    this.pointer = { x: sx, y: sy }
    this.setHovered(this.hoverTest(sx, sy))
    this.invalidate()
  }

  pointerLeave() {
    this.pointer = null
    this.setHovered(null)
  }

  click(sx: number, sy: number) {
    this.select(this.hitTest(sx, sy))
  }

  doubleClick(sx: number, sy: number) {
    const node = this.hitTest(sx, sy)
    if (!node) return
    this.select(node)
    this.focus(node)
  }

  select(node: GraphNode | null) {
    if (node === this.model.root) node = null
    if (node === this.selected) return
    this.selected = node
    this.selectionRelated = relatedTo(this.model, node)
    this.hoverMuted = this.dimmedBySelection(this.hovered)
    this.updateRelated()
    this.callbacks.onSelectionChange?.(node)
    this.emitContext()
    this.invalidate()
  }

  private setHovered(node: GraphNode | null) {
    if (node === this.model.root) node = null
    if (node === this.hovered) return
    this.hovered = node
    this.hoverMuted = this.dimmedBySelection(node)
    this.updateRelated()
    this.invalidate()
  }

  /**
   * What the pointer hovers: like hitTest, but the empty space inside an open
   * container (between its children) hovers nothing, so passing over the gaps
   * does not light up the whole container's relationships. Its header band
   * still hovers it.
   */
  hoverTest(sx: number, sy: number): GraphNode | null {
    const node = this.hitTest(sx, sy)
    if (!node || node.children.length === 0 || this.lod.open(node) < 0.35) return node
    const [, wy] = toWorld(this.camera, this.viewport, sx, sy)
    return wy < node.y + node.pad + node.header ? node : null
  }

  /** The deepest node at a screen point that is visible enough to point at. */
  hitTest(sx: number, sy: number): GraphNode | null {
    this.lod.begin(this.camera.k)
    const [wx, wy] = toWorld(this.camera, this.viewport, sx, sy)
    const root = this.model.root
    if (!inside(root, wx, wy)) return null
    let node = root
    for (;;) {
      if (this.lod.open(node) < 0.35) return node
      let next: GraphNode | null = null
      for (const c of node.children) {
        if (inside(c, wx, wy) && Math.min(c.w, c.h) * this.camera.k >= 3) {
          next = c
          break
        }
      }
      if (!next) return node
      node = next
    }
  }

  /**
   * What relationships are shown for: the hovered node, unless it is dimmed
   * against the selection (its connections show only once it is selected),
   * else the selection.
   */
  get relationFocus(): GraphNode | null {
    return this.hovered && !this.hoverMuted ? this.hovered : this.selected
  }

  private updateRelated() {
    const focus = this.relationFocus
    this.related = focus === this.selected ? this.selectionRelated : relatedTo(this.model, focus)
  }

  /** Whether a node reads as dimmed while only the selection is in focus. */
  private dimmedBySelection(n: GraphNode | null): boolean {
    const s = this.selected
    if (!n || !s || n === s) return false
    // Directories are never faded themselves; one reads as dimmed when nothing in it is lit.
    if (n.kind === 'dir') return dims(s) && !contains(n, s) && !someIn(this.selectionRelated, n)
    return dimFor(n, s, this.selectionRelated) < 1
  }

  // ---- Navigation context ------------------------------------------------------

  /**
   * What the breadcrumbs show: the selection if there is one, else the node
   * last navigated to, else the deepest node the camera is inside of.
   */
  contextPath(): GraphNode[] {
    const node = this.selected ?? this.pinned ?? this.cameraContext()
    return node === this.model.root ? [] : [...ancestors(node).slice(1), node]
  }

  /** The deepest node containing the viewport centre that the view has "entered". */
  cameraContext(): GraphNode {
    this.lod.begin(this.camera.k)
    const { x, y, k } = this.camera
    const short = Math.min(this.viewport.width, this.viewport.height)
    let node = this.model.root
    for (;;) {
      let next: GraphNode | null = null
      for (const c of node.children) {
        if (!inside(c, x, y)) continue
        const entered = this.lod.open(c) >= 0.5 || (c.children.length === 0 && Math.min(c.w, c.h) * k >= short * 0.5)
        if (entered) next = c
        break
      }
      if (!next) return node
      node = next
    }
  }

  private emitContext() {
    const path = this.contextPath()
    const key = path.map((n) => n.id).join('\n')
    if (key === this.contextKey) return
    this.contextKey = key
    this.callbacks.onContextChange?.(path)
  }

  // ---- Frame loop --------------------------------------------------------------

  invalidate() {
    if (this.failed || this.frameId) return
    this.frameId = raf((t) => this.frame(t))
  }

  private frame(now: number) {
    this.frameId = 0
    const dt = this.lastTime ? Math.min(64, now - this.lastTime) : 16
    this.lastTime = now
    try {
      this.step(now, dt)
      const start = performance.now()
      this.draw()
      this.lastFrameMs = performance.now() - start
      this.emitContext()
    } catch (error) {
      this.failed = true
      this.callbacks.onError?.(error)
      return
    }
    if (this.animating) this.invalidate()
    else this.lastTime = 0
  }

  /** Advance animations; public so tests can drive time. */
  step(now: number, dt: number) {
    const f = this.flightAnim
    if (f) {
      const t = Math.min(1, (now - f.start) / f.duration)
      this.camera = f.flight.at(easeInOut(t))
      if (t >= 1) this.flightAnim = null
    }
    const z = this.wheelZoom
    if (z) {
      const a = 1 - Math.exp(-dt / WHEEL_TAU)
      let k = this.camera.k * Math.pow(z.targetK / this.camera.k, a)
      const done = Math.abs(Math.log(z.targetK / k)) < 0.002
      if (done) k = z.targetK
      this.camera = anchoredAt(this.viewport, k, z.wx, z.wy, z.sx, z.sy)
      if (done) this.wheelZoom = null
    }
    if (this.pointer && this.animating) this.setHovered(this.hoverTest(this.pointer.x, this.pointer.y))
  }

  // ---- Drawing -----------------------------------------------------------------

  private draw() {
    const ctx = this.ctx
    const text = this.text
    if (!ctx || !text) return
    const { width, height } = this.viewport
    ctx.setTransform(this.dpr, 0, 0, this.dpr, 0, 0)
    ctx.clearRect(0, 0, width, height)
    this.lod.begin(this.camera.k)

    const visible = this.collectVisible()
    ctx.lineWidth = 1
    for (const n of visible) this.drawBox(ctx, n)
    this.drawEdges(ctx, visible)
    const labeled = new Set<GraphNode>()
    for (const n of visible) if (this.drawLabel(ctx, text, n)) labeled.add(n)
    this.drawHighlights(ctx)
    this.drawHoverTip(ctx, text, labeled)
  }

  /** Nodes to draw, parents before children, skipping off-screen and invisible subtrees. */
  private collectVisible(): GraphNode[] {
    const out: GraphNode[] = []
    const { width, height } = this.viewport
    const stack = [this.model.root]
    while (stack.length) {
      const n = stack.pop()!
      if (this.lod.vis(n) < 0.01) continue
      const s = this.screenBox(n)
      if (s.x > width || s.y > height || s.x + s.w < 0 || s.y + s.h < 0) continue
      if (s.w < 1.5 || s.h < 1.5) continue
      out.push(n)
      if (this.lod.open(n) > 0.01) for (let i = n.children.length - 1; i >= 0; i--) stack.push(n.children[i])
    }
    return out
  }

  private screenBox(n: Box): Box {
    const { k } = this.camera
    return { x: toScreenX(this.camera, this.viewport, n.x), y: toScreenY(this.camera, this.viewport, n.y), w: n.w * k, h: n.h * k }
  }

  private dim(n: GraphNode): number {
    return dimFor(n, this.relationFocus, this.related)
  }

  private drawBox(ctx: CanvasRenderingContext2D, n: GraphNode) {
    const t = this.theme
    const vis = this.lod.vis(n) * this.dim(n)
    const open = this.lod.open(n)
    const b = snap(this.screenBox(n))

    switch (n.kind) {
      case 'root':
        // The map's ground; only visible as a region once zoomed far out.
        if (open < 1) {
          ctx.globalAlpha = 1 - open
          fillRect(ctx, b, 2, t.region)
          strokeRect(ctx, b, 2, t.regionStroke)
        }
        break
      case 'dir':
        ctx.globalAlpha = vis
        fillRect(ctx, b, 2, t.regionOpen)
        if (open < 1) {
          ctx.globalAlpha = vis * (1 - open)
          fillRect(ctx, b, 2, t.region)
        }
        ctx.globalAlpha = vis
        strokeRect(ctx, b, 2, t.regionStroke)
        break
      case 'file':
        ctx.globalAlpha = vis
        fillRect(ctx, b, 3, t.node)
        strokeRect(ctx, b, 3, t.nodeStroke)
        break
      case 'class':
        ctx.globalAlpha = vis
        fillRect(ctx, b, 2, t.symbol)
        strokeRect(ctx, b, 2, t.symbolStroke)
        if (open > 0.01) {
          // Rule under the class name, like a class diagram.
          const y = Math.round(b.y + (n.pad + n.header) * this.camera.k) + 0.5
          ctx.globalAlpha = vis * open
          ctx.strokeStyle = t.symbolStroke
          ctx.lineWidth = 1
          ctx.beginPath()
          ctx.moveTo(b.x, y)
          ctx.lineTo(b.x + b.w, y)
          ctx.stroke()
        }
        break
      case 'member':
        if (n === this.hovered || n === this.selected) {
          ctx.globalAlpha = vis
          fillRect(ctx, b, 2, n === this.selected ? t.selectedFill : t.hoverFill)
        }
        break
    }
    ctx.globalAlpha = 1
  }

  /**
   * Edges are grouped by the pair of nodes currently standing in for their
   * endpoints, so zoomed out, the many imports between two areas read as one
   * line between two regions; zooming in, the group splits as the regions
   * open and each line glides to the files it connects.
   */
  private drawEdges(ctx: CanvasRenderingContext2D, visible: GraphNode[]) {
    const t = this.theme
    const focus = this.relationFocus
    const { width, height } = this.viewport
    const n = this.model.nodes.length
    const groups = new Map<number, EdgeGroup>()
    const add = (e: GraphEdge, s: GraphNode, t: GraphNode, alpha: number) => {
      // Both ends inside one stand-in: internal to a region at this scale.
      if (contains(s, t) || contains(t, s)) return
      const key = s.order * n + t.order
      let g = groups.get(key)
      if (!g) groups.set(key, (g = { s, t, count: 0, alpha: 0, emphasized: false }))
      g.count++
      g.alpha = Math.max(g.alpha, alpha)
      if (focus && !g.emphasized && (contains(focus, e.source) || contains(focus, e.target))) g.emphasized = true
    }

    // Imports everywhere; class relationships only from files open on screen.
    const edges: GraphEdge[] = this.importEdges.slice()
    for (const n of visible) {
      if (n.kind !== 'file' || this.lod.open(n) < 0.05) continue
      const list = this.classEdgesByFile.get(n)
      if (list) for (const e of list) edges.push(e)
    }

    for (const e of edges) {
      if (e.kind === 'import') {
        // An import is drawn between every pair of stand-ins for its ends,
        // faded by how much each pair is what is on screen.
        for (const s of this.lod.standIns(e.source))
          for (const t of this.lod.standIns(e.target)) {
            const weight = s.weight * t.weight
            if (weight >= 0.05) add(e, s.node, t.node, weight)
          }
      } else {
        // Class relationships belong to the symbol level only.
        const alpha = Math.min(this.lod.vis(e.source), this.lod.vis(e.target))
        if (alpha >= 0.05) add(e, e.source, e.target, alpha)
      }
    }

    // Paths batched by (emphasis, opacity step, width) so a frame makes only a
    // handful of stroke calls however many edges there are.
    const LEVELS = 6
    const paths = new Map<number, { line: Path2D; heads: Path2D }>()
    const pathFor = (emphasized: boolean, level: number, wide: boolean) => {
      const key = (emphasized ? 1000 : 0) + level * 2 + (wide ? 1 : 0)
      let p = paths.get(key)
      if (!p) paths.set(key, (p = { line: new Path2D(), heads: new Path2D() }))
      return p
    }
    let drawn = 0

    const candidates: EdgeCandidate[] = []
    const { k } = this.camera
    const ox = width / 2 - this.camera.x * k
    const oy = height / 2 - this.camera.y * k
    for (const g of groups.values()) {
      // Screen boxes, computed in place (this loop runs for thousands of groups).
      const sw = g.s.w * k
      const sh = g.s.h * k
      const dw = g.t.w * k
      const dh = g.t.h * k
      // Fade out edges whose ends are too small to see.
      const legible = smoothstep(Math.min(sw, sh, dw, dh), 10, 32)
      const strength = 0.45 + 0.55 * Math.min(1, Math.log2(g.count + 1) / 4)
      const alpha = g.alpha * legible * strength
      if (alpha < 0.05 && !g.emphasized) continue

      const scx = g.s.x * k + ox + sw / 2
      const scy = g.s.y * k + oy + sh / 2
      const dcx = g.t.x * k + ox + dw / 2
      const dcy = g.t.y * k + oy + dh / 2
      // Only relationships anchored in view: a line between two off-screen
      // places would just cross the screen. One end off-screen: fainter.
      const sOn = overlapsView(scx, scy, sw, sh, width, height)
      const dOn = overlapsView(dcx, dcy, dw, dh, width, height)
      if (!sOn && !dOn && !g.emphasized) continue
      const anchored = sOn && dOn ? 1 : 0.5
      const vx = dcx - scx
      const vy = dcy - scy
      if (vx * vx + vy * vy < 64) continue
      // One end inside the other's box: nothing meaningful to draw.
      if (Math.abs(vx) < sw / 2 && Math.abs(vy) < sh / 2) continue
      if (Math.abs(vx) < dw / 2 && Math.abs(vy) < dh / 2) continue
      // Leave each box where the centre-to-centre line crosses its border.
      const ts = Math.min(vx ? sw / 2 / Math.abs(vx) : Infinity, vy ? sh / 2 / Math.abs(vy) : Infinity)
      const td = Math.min(vx ? dw / 2 / Math.abs(vx) : Infinity, vy ? dh / 2 / Math.abs(vy) : Infinity)
      const x0 = scx + vx * ts
      const y0 = scy + vy * ts
      const x1 = dcx - vx * td
      const y1 = dcy - vy * td
      const lx = x1 - x0
      const ly = y1 - y0
      const len = Math.sqrt(lx * lx + ly * ly)
      if (len < 6) continue
      // Gentle curve that always bends the same way relative to direction,
      // so edges in opposite directions do not overlap.
      const bend = Math.min(len * 0.12, 60)
      const cx = (x0 + x1) / 2 + (ly / len) * bend
      const cy = (y0 + y1) / 2 - (lx / len) * bend
      if (Math.max(x0, x1, cx) < 0 || Math.max(y0, y1, cy) < 0 || Math.min(x0, x1, cx) > width || Math.min(y0, y1, cy) > height) continue

      candidates.push({
        x0, y0, cx, cy, x1, y1, len,
        alpha: alpha * anchored,
        weight: g.alpha,
        count: g.count,
        emphasized: g.emphasized,
        // A line that is fading out between levels gives way to the one fading in.
        score: g.count * legible * anchored * g.alpha,
      })
    }

    const emphasisCap = focus === this.selected ? SELECTION_EDGE_CAP : HOVER_EDGE_CAP
    for (const c of capEdgeGroups(candidates, EDGE_BUDGET, emphasisCap)) {
      const alpha = c.alpha
      if (!c.emphasized) drawn++
      const level = Math.max(1, Math.round(Math.min(1, c.emphasized ? c.weight : alpha) * LEVELS))
      const p = pathFor(c.emphasized, level, c.count >= 6)
      p.line.moveTo(c.x0, c.y0)
      p.line.quadraticCurveTo(c.cx, c.cy, c.x1, c.y1)
      if (c.len > 48) arrowHead(p.heads, c.cx, c.cy, c.x1, c.y1, c.emphasized ? 6 : 5)
    }

    // Fainter when crowded, and fainter still while something is in focus.
    const crowd = drawn > 200 ? 0.8 : 1
    const selectedColor = focus === this.selected ? t.selected : t.edgeStrong
    for (const [key, p] of paths) {
      const emphasized = key >= 1000
      const level = Math.floor((key % 1000) / 2)
      const wide = key % 2 === 1
      ctx.globalAlpha = (level / LEVELS) * (emphasized ? 1 : crowd * (focus ? 0.4 : 1))
      ctx.strokeStyle = ctx.fillStyle = emphasized ? selectedColor : t.edge
      ctx.lineWidth = (wide ? 1.6 : 1) * (emphasized ? 1.25 : 1)
      ctx.stroke(p.line)
      ctx.fill(p.heads)
    }
    ctx.globalAlpha = 1
    ctx.lineWidth = 1
  }

  /** Returns whether a label was drawn. */
  private drawLabel(ctx: CanvasRenderingContext2D, text: TextMeasurer, n: GraphNode): boolean {
    const t = this.theme
    const { k } = this.camera
    const b = this.screenBox(n)
    const alpha = this.lod.vis(n) * this.dim(n) * smoothstep(Math.min(b.w, b.h * 2.5), 22, 40)
    if (alpha < 0.02) return false

    if (n.kind === 'member') return this.drawMember(ctx, text, n, b, alpha)

    const open = this.lod.open(n)
    // The label travels to the header before the children arrive.
    const moved = settle(open)
    const weight = n.kind === 'file' ? 500 : 600
    const closedSize =
      n.kind === 'file'
        ? clamp(Math.min(b.w * 0.085, b.h * 0.2), 11, 14)
        : n.kind === 'class'
          ? clamp(Math.min(b.w * 0.09, b.h * 0.24), 11, 13)
          : clamp(Math.min(b.w * 0.1, b.h * 0.2), 11, 20)
    const pad = n.pad * k
    const header = n.header * k
    // Open label sits in the header band, sized to fit it.
    const openSize = clamp(Math.min(12.5, header * 0.62), 0, 12.5)
    const size = TextMeasurer.quantize(lerp(closedSize, openSize, moved))
    if (size < 10) return false
    if (n.kind === 'root' && open > 0.5) return false

    const closedMax = b.w * 0.88
    const openMax = b.w - pad * 2
    const label = text.fit(n.label, size, weight, lerp(closedMax, openMax, moved))
    if (!label) return false
    const width = text.width(label, size, weight)

    const secondary = n.kind === 'class' ? `${n.children.length} members` : n.kind === 'file' ? n.detail ?? '' : `${n.fileCount} files`
    const showSecondary = open < 0.99 && b.h >= size * 3.4 && b.w >= 72
    const lineGap = showSecondary ? size * 0.7 : 0

    const closedX = b.x + b.w / 2 - width / 2
    const closedY = b.y + b.h / 2 - lineGap
    // Classes: header row inside the padding. Directories and files: the band above their padding.
    const openX = b.x + pad + 1
    const openY = n.kind === 'class' ? b.y + pad + header / 2 : b.y + (header + pad) / 2
    const x = lerp(closedX, openX, moved)
    const y = lerp(closedY, openY, moved)

    ctx.globalAlpha = alpha
    text.use(size, weight)
    ctx.textBaseline = 'middle'
    ctx.fillStyle = n.kind === 'dir' && moved > 0.5 ? t.labelSecondary : t.label
    ctx.fillText(label, x, y)

    // The caption belongs to the closed look. It travels under the label as
    // the label moves to the header, and leaves as the children arrive, so
    // a half-open node always shows either its count or its contents.
    const captionAlpha = 1 - smoothstep(open, CAPTION_FADE[0], CAPTION_FADE[1])
    if (showSecondary && secondary && captionAlpha > 0.01) {
      const s2 = TextMeasurer.quantize(Math.max(10.5, size * 0.72))
      const sub = text.fit(secondary, s2, 400, lerp(closedMax, openMax, moved))
      if (sub) {
        const subWidth = text.width(sub, s2, 400)
        ctx.globalAlpha = alpha * captionAlpha
        text.use(s2, 400)
        ctx.fillStyle = t.labelMuted
        // Centred under the label when closed, left-aligned with it when open.
        ctx.fillText(sub, x + lerp((width - subWidth) / 2, 0, moved), y + size * 1.35)
      }
    }
    ctx.globalAlpha = 1
    return true
  }

  private drawMember(ctx: CanvasRenderingContext2D, text: TextMeasurer, n: GraphNode, b: Box, alpha: number): boolean {
    const t = this.theme
    const size = TextMeasurer.quantize(clamp(b.h * 0.64, 0, 13))
    if (size < 10) return false
    const cy = b.y + b.h / 2
    const glyph = size * 0.36
    const gx = b.x + glyph

    ctx.globalAlpha = alpha
    ctx.lineWidth = 1
    ctx.strokeStyle = t.labelMuted
    ctx.fillStyle = t.labelMuted
    if (n.memberKind === 'method') {
      ctx.beginPath()
      ctx.arc(gx, cy, glyph / 2, 0, Math.PI * 2)
      ctx.stroke()
    } else {
      ctx.fillRect(gx - glyph / 2, cy - glyph / 2, glyph, glyph)
    }

    const x = gx + glyph + size * 0.55
    const max = b.x + b.w - x
    const name = text.fit(n.label, size, 450, max)
    if (!name) return false
    text.use(size, 450)
    ctx.textBaseline = 'middle'
    ctx.fillStyle = t.label
    ctx.fillText(name, x, cy)

    if (n.memberKind === 'method' && name === n.label) {
      const w = text.width(name, size, 450)
      const params = text.fit(`(${n.detail ?? ''})`, size, 400, max - w)
      if (params) {
        text.use(size, 400)
        ctx.fillStyle = t.labelMuted
        ctx.fillText(params, x + w, cy)
      }
    }
    ctx.globalAlpha = 1
    return true
  }

  /**
   * The boxes to mark as connected to the selection, more quietly than the
   * selection itself: for each related node, the box its arrow meets at this
   * scale. Only while the selection's relationships are the ones shown.
   */
  relatedHighlights(): GraphNode[] {
    const sel = this.selected
    if (!sel || this.relationFocus !== sel) return []
    this.lod.begin(this.camera.k)
    const lit = new Set<GraphNode>()
    for (const o of this.related) {
      let best: StandIn | null = null
      for (const s of this.lod.standIns(this.model.nodes[o])) if (!best || s.weight > best.weight) best = s
      const n = best?.node
      if (n && n.kind !== 'root' && !contains(n, sel) && !contains(sel, n)) lit.add(n)
    }
    return [...lit]
  }

  private drawHighlights(ctx: CanvasRenderingContext2D) {
    const t = this.theme
    const outline = (n: GraphNode, color: string, width: number, strength = 1) => {
      if (n.kind === 'member' || this.lod.vis(n) < 0.05) return
      const b = snap(this.screenBox(n))
      if (b.w < 2 || b.h < 2) return
      ctx.globalAlpha = Math.min(1, this.lod.vis(n) * 1.5) * strength
      ctx.lineWidth = width
      strokeRect(ctx, inset(b, -(width - 1) / 2), n.kind === 'file' ? 3 : 2, color)
    }
    for (const n of this.relatedHighlights()) outline(n, t.selected, 1.5, RELATED_STRENGTH)
    if (this.hovered && this.hovered !== this.selected) outline(this.hovered, t.hover, 1)
    if (this.selected) outline(this.selected, t.selected, 2)
    ctx.globalAlpha = 1
  }

  /** A small label at the pointer when the hovered node is too small to show its own. */
  private drawHoverTip(ctx: CanvasRenderingContext2D, text: TextMeasurer, labeled: Set<GraphNode>) {
    const n = this.hovered
    const p = this.pointer
    if (!n || !p || labeled.has(n) || this.animating) return
    const size = 12
    const label = n.kind === 'member' && n.memberKind === 'method' ? `${n.label}()` : n.label
    const w = text.width(label, size, 500)
    const padX = 7
    const bw = w + padX * 2
    const bh = 22
    let x = p.x + 12
    let y = p.y + 16
    if (x + bw > this.viewport.width - 4) x = p.x - 12 - bw
    if (y + bh > this.viewport.height - 4) y = p.y - 12 - bh
    ctx.globalAlpha = 1
    fillRect(ctx, { x, y, w: bw, h: bh }, 3, this.theme.tipBackground)
    text.use(size, 500)
    ctx.textBaseline = 'middle'
    ctx.fillStyle = this.theme.tipText
    ctx.fillText(label, x + padX, y + bh / 2)
  }
}

/**
 * The edge groups to draw, strongest first by the existing score: at most
 * `emphasisCap` hovered/selected (emphasized) groups plus at most `cap`
 * others. Both are hard caps, so neither a dense repository nor a
 * high-degree node can cover the canvas with lines. Emphasized groups past
 * their cap are dropped, not drawn as ordinary edges.
 */
export function capEdgeGroups<T extends { emphasized: boolean; score: number }>(
  groups: T[],
  cap: number,
  emphasisCap: number,
): T[] {
  const ordered = [...groups].sort((a, b) => (b.emphasized ? 1 : 0) - (a.emphasized ? 1 : 0) || b.score - a.score)
  const out: T[] = []
  let emphasized = 0
  let others = 0
  for (const g of ordered) {
    if (g.emphasized) {
      if (emphasized >= emphasisCap) continue
      emphasized++
    } else {
      if (others >= cap) break
      others++
    }
    out.push(g)
  }
  return out
}

// ---- Focus helpers -------------------------------------------------------------

/** Nodes connected by an edge to a node's subtree, by order. */
function relatedTo(model: GraphModel, focus: GraphNode | null): Set<number> {
  const related = new Set<number>()
  if (!focus) return related
  for (const e of model.edges) {
    if (contains(focus, e.source)) related.add(e.target.order)
    else if (contains(focus, e.target)) related.add(e.source.order)
  }
  return related
}

/** Whether focusing this node fades anything. */
function dims(focus: GraphNode): boolean {
  return focus.kind !== 'root' && focus.kind !== 'dir'
}

/** Whether any of these nodes (by order) lies in a node's subtree. */
function someIn(orders: Set<number>, node: GraphNode): boolean {
  for (const o of orders) if (o >= node.order && o < node.end) return true
  return false
}

/** De-emphasis for files and symbols unrelated to the focus. */
function dimFor(n: GraphNode, focus: GraphNode | null, related: Set<number>): number {
  if (!focus || !dims(focus)) return 1
  if (n.kind === 'root' || n.kind === 'dir') return 1
  const subject = n.kind === 'member' ? n.parent! : n
  if (contains(focus, subject) || contains(subject, focus)) return 1
  if (related.has(subject.order)) return 1
  if (subject.kind === 'class' && subject.parent && related.has(subject.parent.order)) return 1
  return 0.5
}

// ---- Geometry helpers ----------------------------------------------------------

function inside(b: Box, x: number, y: number): boolean {
  return x >= b.x && y >= b.y && x <= b.x + b.w && y <= b.y + b.h
}

/** Whether a box given by its screen centre and size overlaps the viewport. */
function overlapsView(cx: number, cy: number, w: number, h: number, width: number, height: number): boolean {
  return cx + w / 2 >= 0 && cy + h / 2 >= 0 && cx - w / 2 <= width && cy - h / 2 <= height
}

function lerp(a: number, b: number, t: number): number {
  return a + (b - a) * t
}

function arrowHead(path: Path2D, fromX: number, fromY: number, x: number, y: number, size: number) {
  const a = Math.atan2(y - fromY, x - fromX)
  path.moveTo(x, y)
  path.lineTo(x - size * Math.cos(a - 0.45), y - size * Math.sin(a - 0.45))
  path.lineTo(x - size * Math.cos(a + 0.45), y - size * Math.sin(a + 0.45))
  path.closePath()
}

/** Align to the pixel grid so 1px strokes stay crisp. */
function snap(b: Box): Box {
  const x = Math.round(b.x) + 0.5
  const y = Math.round(b.y) + 0.5
  return { x, y, w: Math.max(0, Math.round(b.x + b.w) - Math.round(b.x) - 1), h: Math.max(0, Math.round(b.y + b.h) - Math.round(b.y) - 1) }
}

function inset(b: Box, d: number): Box {
  return { x: b.x + d, y: b.y + d, w: b.w - d * 2, h: b.h - d * 2 }
}

function roundRect(ctx: CanvasRenderingContext2D, b: Box, r: number) {
  const rr = Math.min(r, b.w / 2, b.h / 2)
  ctx.beginPath()
  ctx.roundRect(b.x, b.y, b.w, b.h, rr)
}

function fillRect(ctx: CanvasRenderingContext2D, b: Box, r: number, color: string) {
  ctx.fillStyle = color
  roundRect(ctx, b, r)
  ctx.fill()
}

function strokeRect(ctx: CanvasRenderingContext2D, b: Box, r: number, color: string) {
  ctx.strokeStyle = color
  roundRect(ctx, b, r)
  ctx.stroke()
}
