// Semantic zoom: how much of each node's inside is shown at a given scale.
//
// There are no global levels. Each container "opens" as its children become
// legible on screen: `openness` rises smoothly from 0 (closed: a filled region
// with a centred label) to 1 (open: a faint region, a header label, children
// fully shown). A node's visibility is the product of its ancestors'
// openness, so detail fades in inside its parent and fades back out the same
// way. Everything here is a pure function of the node and the scale.

import type { Box } from './camera'
import type { GraphModel, GraphNode } from './model'

export function smoothstep(x: number, a: number, b: number): number {
  const t = Math.min(1, Math.max(0, (x - a) / (b - a)))
  return t * t * (3 - 2 * t)
}

/**
 * Screen sizes (px) over which containers open. A container opens only when
 * both its children would be big enough to read as shapes and it is itself
 * big enough to frame them (for directories: to carry a header label).
 * Tuned by eye; not product constants.
 */
export const OPEN_RANGE = {
  /** The root opens early: the repository's top-level areas are the map. */
  root: { child: [10, 30] },
  dir: { child: [28, 70], header: [11, 16] },
  file: { child: [48, 100], size: [220, 360] },
  /** Classes open on member row height, i.e. text size. */
  class: { row: [11, 16], size: [160, 260] },
} as const

/**
 * Opening is sequenced within one continuous transition: first the parent's
 * label settles into its header band (`settle`), then its children fade in
 * (`reveal`). The two overlap a little so it never reads as two steps.
 */
export function settle(open: number): number {
  return smoothstep(open, 0, 0.55)
}

export function reveal(open: number): number {
  return smoothstep(open, 0.35, 1)
}

/** Typical child size, in world units: the side of the mean child cell. */
function childScale(node: GraphNode): number {
  const n = node.children.length
  if (n === 0) return 0
  const innerW = node.w - node.pad * 2
  const innerH = node.h - node.header - node.pad * 2
  return Math.sqrt(Math.max(0, innerW * innerH) / n)
}

export function openness(node: GraphNode, k: number): number {
  if (node.children.length === 0) return 0
  const short = Math.min(node.w, node.h) * k
  const range = (x: number, [a, b]: readonly [number, number]) => smoothstep(x, a, b)
  switch (node.kind) {
    case 'root':
      return range(childScale(node) * k, OPEN_RANGE.root.child)
    case 'dir':
      return Math.min(range(childScale(node) * k, OPEN_RANGE.dir.child), range(node.header * k, OPEN_RANGE.dir.header))
    case 'file':
      return Math.min(range(childScale(node) * k, OPEN_RANGE.file.child), range(short, OPEN_RANGE.file.size))
    case 'class':
      return Math.min(range(node.rowHeight * k, OPEN_RANGE.class.row), range(short, OPEN_RANGE.class.size))
    default:
      return 0
  }
}

/** The scale at which a container is fully open (0 for a leaf). */
export function fullyOpenK(node: GraphNode): number {
  if (node.children.length === 0) return 0
  const at = (px: number, size: number) => (size > 0 ? px / size : 0)
  const child = childScale(node)
  const short = Math.min(node.w, node.h)
  switch (node.kind) {
    case 'root':
      return at(OPEN_RANGE.root.child[1], child)
    case 'dir':
      return Math.max(at(OPEN_RANGE.dir.child[1], child), at(OPEN_RANGE.dir.header[1], node.header))
    case 'file':
      return Math.max(at(OPEN_RANGE.file.child[1], child), at(OPEN_RANGE.file.size[1], short))
    case 'class':
      return Math.max(at(OPEN_RANGE.class.row[1], node.rowHeight), at(OPEN_RANGE.class.size[1], short))
    default:
      return 0
  }
}

/**
 * Per-frame cache of openness and visibility, indexed by depth-first order.
 * Call `begin(k)` once per frame; values are computed on first use.
 */
export class LodFrame {
  k = 1
  private readonly model: GraphModel
  private readonly openV: Float64Array
  private readonly visV: Float64Array
  private readonly openAt: Uint32Array
  private readonly visAt: Uint32Array
  /** Endpoint boxes (x, y, w, h per node) and their dominant stand-in. */
  private readonly endV: Float64Array
  private readonly endRep: Int32Array
  private readonly endAt: Uint32Array
  private frame = 0

  constructor(model: GraphModel) {
    this.model = model
    const n = model.nodes.length
    this.openV = new Float64Array(n)
    this.visV = new Float64Array(n)
    this.openAt = new Uint32Array(n)
    this.visAt = new Uint32Array(n)
    this.endV = new Float64Array(n * 4)
    this.endRep = new Int32Array(n)
    this.endAt = new Uint32Array(n)
  }

  begin(k: number) {
    this.k = k
    this.frame++
  }

  open(node: GraphNode): number {
    const i = node.order
    if (this.openAt[i] !== this.frame) {
      this.openV[i] = openness(node, this.k)
      this.openAt[i] = this.frame
    }
    return this.openV[i]
  }

  /** How visible the node is as a distinct object: its ancestors' reveal multiplied. */
  vis(node: GraphNode): number {
    const i = node.order
    if (this.visAt[i] !== this.frame) {
      const p = node.parent
      this.visV[i] = p ? this.vis(p) * reveal(this.open(p)) : 1
      this.visAt[i] = this.frame
    }
    return this.visV[i]
  }

  /**
   * Where an edge attached to `node` should meet the map at this scale: a
   * blend of the node and its ancestors, each weighted by how much it is the
   * visible stand-in (visible but not yet open). The weights sum to 1, so as
   * a region opens the endpoint glides from the region to the node inside it
   * instead of jumping. Returns the stand-in with the largest weight, which
   * edges are grouped by.
   */
  endpoint(node: GraphNode, out: Box): GraphNode {
    // Many edges share an endpoint; compute each node's blend once per frame.
    const i = node.order
    if (this.endAt[i] === this.frame) {
      out.x = this.endV[i * 4]
      out.y = this.endV[i * 4 + 1]
      out.w = this.endV[i * 4 + 2]
      out.h = this.endV[i * 4 + 3]
      return this.model.nodes[this.endRep[i]]
    }
    const dominant = this.blend(node, out)
    this.endV[i * 4] = out.x
    this.endV[i * 4 + 1] = out.y
    this.endV[i * 4 + 2] = out.w
    this.endV[i * 4 + 3] = out.h
    this.endRep[i] = dominant.order
    this.endAt[i] = this.frame
    return dominant
  }

  private blend(node: GraphNode, out: Box): GraphNode {
    out.x = out.y = out.w = out.h = 0
    const chain = chainOf(node, this.model)
    let remaining = 1
    let dominant = chain[0]
    let best = -1
    for (let i = 0; i < chain.length; i++) {
      const a = chain[i]
      const o = i === chain.length - 1 ? 0 : this.open(a)
      const weight = remaining * (1 - o)
      if (weight > 0) {
        out.x += a.x * weight
        out.y += a.y * weight
        out.w += a.w * weight
        out.h += a.h * weight
      }
      if (weight > best) {
        best = weight
        dominant = a
      }
      remaining *= o
      if (remaining <= 0) break
    }
    return dominant
  }
}

const chains = new WeakMap<GraphModel, Map<number, GraphNode[]>>()

/** Root-to-node path including both ends; cached per model. */
function chainOf(node: GraphNode, model: GraphModel): GraphNode[] {
  let byNode = chains.get(model)
  if (!byNode) chains.set(model, (byNode = new Map()))
  let chain = byNode.get(node.order)
  if (!chain) {
    chain = []
    for (let n: GraphNode | null = node; n; n = n.parent) chain.push(n)
    chain.reverse()
    byNode.set(node.order, chain)
  }
  return chain
}
