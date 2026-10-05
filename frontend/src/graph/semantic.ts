// Semantic zoom: how much of each node's inside is shown at a given scale.
//
// There are no global levels. Each container "opens" as its children become
// legible on screen: `openness` rises smoothly from 0 (closed: a filled region
// with a centred label) to 1 (open: a faint region, a header label, children
// fully shown). A node's visibility is the product of its ancestors'
// openness, so detail fades in inside its parent and fades back out the same
// way. Everything here is a pure function of the node and the scale.

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

/**
 * The scale at which a container starts to open (`start`: open above it) or
 * is fully open (`end`). 0 for a leaf.
 */
export function openingK(node: GraphNode, end: 'start' | 'end'): number {
  if (node.children.length === 0) return 0
  const e = end === 'start' ? 0 : 1
  const at = (range: readonly [number, number], size: number) => (size > 0 ? range[e] / size : 0)
  const child = childScale(node)
  const short = Math.min(node.w, node.h)
  // Openness is the least of its parts, so a node starts to open, and is
  // fully open, only once every part has.
  switch (node.kind) {
    case 'root':
      return at(OPEN_RANGE.root.child, child)
    case 'dir':
      return Math.max(at(OPEN_RANGE.dir.child, child), at(OPEN_RANGE.dir.header, node.header))
    case 'file':
      return Math.max(at(OPEN_RANGE.file.child, child), at(OPEN_RANGE.file.size, short))
    case 'class':
      return Math.max(at(OPEN_RANGE.class.row, node.rowHeight), at(OPEN_RANGE.class.size, short))
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
  /** Edge stand-ins per node (see `standIns`), reused between frames. */
  private readonly standInV: StandIn[][]
  private readonly standInAt: Uint32Array
  private frame = 0

  constructor(model: GraphModel) {
    this.model = model
    const n = model.nodes.length
    this.openV = new Float64Array(n)
    this.visV = new Float64Array(n)
    this.openAt = new Uint32Array(n)
    this.visAt = new Uint32Array(n)
    this.standInV = new Array(n)
    this.standInAt = new Uint32Array(n)
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
   * Where an edge attached to `node` meets the map at this scale: the nodes
   * on its path from the root that are drawn as themselves, each weighted by
   * how much (its visibility, less the part handed on to its children as it
   * opens). The weights sum to 1. Edges are drawn to each stand-in's own box
   * and faded by its weight, so a line always ends on a box that is really
   * there; as a region opens, its line fades out while the lines to the
   * nodes inside it fade in.
   */
  standIns(node: GraphNode): readonly StandIn[] {
    // Many edges share an endpoint; compute each node's stand-ins once per frame.
    const i = node.order
    let list = this.standInV[i]
    if (this.standInAt[i] === this.frame && list) return list
    if (!list) this.standInV[i] = list = []
    list.length = 0
    const chain = chainOf(node, this.model)
    for (let j = 0; j < chain.length; j++) {
      const a = chain[j]
      const vis = this.vis(a)
      if (vis <= 0) break
      const weight = j === chain.length - 1 ? vis : vis * (1 - reveal(this.open(a)))
      if (weight > 1e-3) list.push({ node: a, weight })
    }
    this.standInAt[i] = this.frame
    return list
  }
}

export interface StandIn {
  node: GraphNode
  weight: number
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
