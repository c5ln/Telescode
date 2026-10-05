// The code map's tree: repository → directories → files → classes → members.
//
// Built from the core's GraphResponse without reinterpreting it. Containment
// comes from what the core already reports: directories from each file's
// repo-relative `fileId` path, classes from `fileId`, members from each class's
// own `fields` and `methods`. Edges are the core's file and class edges as-is.
//
// The schema has no list of module-level functions or of call edges, so the
// symbol level is classes and their members only.

import type { GraphResponse } from '../bridge'

export type NodeKind = 'root' | 'dir' | 'file' | 'class' | 'member'

export interface GraphNode {
  id: string
  kind: NodeKind
  label: string
  /** Secondary text: member params, file line count, etc. */
  detail?: string
  memberKind?: 'method' | 'field'
  parent: GraphNode | null
  children: GraphNode[]
  depth: number
  /** Position in depth-first order; the subtree is [order, end). */
  order: number
  end: number
  /** Relative size used by the layout. */
  weight: number
  fileCount: number
  classCount: number
  /** Files only: the core's maxCyclomaticComplexity. */
  complexity?: number

  // Layout, in world units (see layout.ts).
  x: number
  y: number
  w: number
  h: number
  /** Height of the label band shown once the node is open. */
  header: number
  /** Inner padding around the children. */
  pad: number
  /** Classes only: height of one member row. */
  rowHeight: number
}

export type EdgeKind = 'import' | 'class'

export interface GraphEdge {
  source: GraphNode
  target: GraphNode
  kind: EdgeKind
}

export interface GraphModel {
  root: GraphNode
  /** Every node, in depth-first order (nodes[i].order === i). */
  nodes: GraphNode[]
  byId: Map<string, GraphNode>
  edges: GraphEdge[]
}

function makeNode(id: string, kind: NodeKind, label: string, parent: GraphNode | null): GraphNode {
  const node: GraphNode = {
    id,
    kind,
    label,
    parent,
    children: [],
    depth: parent ? parent.depth + 1 : 0,
    order: 0,
    end: 0,
    weight: 0,
    fileCount: 0,
    classCount: 0,
    x: 0,
    y: 0,
    w: 0,
    h: 0,
    header: 0,
    pad: 0,
    rowHeight: 0,
  }
  parent?.children.push(node)
  return node
}

const byLabel = (a: GraphNode, b: GraphNode) =>
  a.label < b.label ? -1 : a.label > b.label ? 1 : a.id < b.id ? -1 : a.id > b.id ? 1 : 0

/** Directories first, then files, each by name; members keep source order. */
function sortChildren(node: GraphNode) {
  if (node.kind === 'class') return
  node.children.sort((a, b) => (a.kind === b.kind ? byLabel(a, b) : a.kind === 'dir' ? -1 : b.kind === 'dir' ? 1 : byLabel(a, b)))
}

/**
 * A directory whose only child is another directory merges with it, so a
 * path like src/core reads as one region instead of two nested ones.
 */
function collapseChains(node: GraphNode) {
  for (const child of node.children) collapseChains(child)
  if (node.kind !== 'dir') return
  while (node.children.length === 1 && node.children[0].kind === 'dir') {
    const only = node.children[0]
    node.id = only.id
    node.label = `${node.label}/${only.label}`
    node.children = only.children
    for (const c of node.children) c.parent = node
  }
}

function finish(node: GraphNode, depth: number, nodes: GraphNode[]) {
  node.depth = depth
  node.order = nodes.length
  nodes.push(node)
  sortChildren(node)
  for (const child of node.children) finish(child, depth + 1, nodes)
  node.end = nodes.length
  if (node.kind === 'file') {
    node.fileCount = 1
    node.classCount = node.children.length
  } else if (node.kind === 'dir' || node.kind === 'root') {
    node.fileCount = node.children.reduce((n, c) => n + c.fileCount, 0)
    node.classCount = node.children.reduce((n, c) => n + c.classCount, 0)
  }
}

export function buildGraphModel(graph: GraphResponse, rootLabel: string): GraphModel {
  const root = makeNode('root', 'root', rootLabel, null)
  const dirs = new Map<string, GraphNode>()
  const files = new Map<string, GraphNode>()

  const dirFor = (segments: string[]): GraphNode => {
    let parent = root
    for (let i = 0; i < segments.length; i++) {
      const path = segments.slice(0, i + 1).join('/')
      let dir = dirs.get(path)
      if (!dir) {
        dir = makeNode(`dir:${path}`, 'dir', segments[i], parent)
        dirs.set(path, dir)
      }
      parent = dir
    }
    return parent
  }

  for (const file of graph.files) {
    const segments = file.fileId.split(/[\\/]/).filter(Boolean)
    const name = segments.pop() ?? file.fileName
    const node = makeNode(`file:${file.fileId}`, 'file', name, dirFor(segments))
    const loc = file.metrics.rawLoc
    node.detail = `${loc} ${loc === 1 ? 'line' : 'lines'}`
    node.complexity = file.metrics.maxCyclomaticComplexity
    // Area grows with size, but sub-linearly so large files do not swamp the map.
    node.weight = Math.sqrt(Math.max(loc, 1)) + 4
    files.set(file.fileId, node)
  }

  const classes = new Map<string, GraphNode>()
  for (const cls of graph.classes) {
    const file = files.get(cls.fileId)
    if (!file) continue
    const node = makeNode(`class:${cls.classId}`, 'class', cls.className, file)
    const seen = new Map<string, number>()
    const member = (name: string, memberKind: 'method' | 'field', detail?: string) => {
      // Python properties and their setters share a name; keep ids unique.
      const n = seen.get(`${memberKind}:${name}`) ?? 0
      seen.set(`${memberKind}:${name}`, n + 1)
      const id = `member:${cls.classId}::${memberKind === 'field' ? '.' : ''}${name}${n ? `#${n}` : ''}`
      const m = makeNode(id, 'member', name, node)
      m.memberKind = memberKind
      m.detail = detail
      m.weight = 1
    }
    for (const f of cls.fields) member(f.name, 'field')
    for (const m of cls.methods) member(m.name, 'method', m.params)
    node.weight = 3 + node.children.length
    classes.set(cls.classId, node)
  }

  collapseChains(root)

  const nodes: GraphNode[] = []
  finish(root, 0, nodes)
  for (let i = nodes.length - 1; i >= 0; i--) {
    const n = nodes[i]
    if (n.kind === 'dir' || n.kind === 'root') n.weight = n.children.reduce((s, c) => s + c.weight, 0)
  }

  const edges: GraphEdge[] = []
  for (const e of graph.fileGraph.edges) {
    const source = files.get(e.source)
    const target = files.get(e.target)
    if (source && target && source !== target) edges.push({ source, target, kind: 'import' })
  }
  for (const e of graph.classEdges) {
    const source = classes.get(e.source)
    const target = classes.get(e.target)
    if (source && target && source !== target) edges.push({ source, target, kind: 'class' })
  }

  return { root, nodes, byId: new Map(nodes.map((n) => [n.id, n])), edges }
}

/** The node's ancestors from the root down, excluding the node itself. */
export function ancestors(node: GraphNode): GraphNode[] {
  const out: GraphNode[] = []
  for (let p = node.parent; p; p = p.parent) out.push(p)
  return out.reverse()
}

export function contains(ancestor: GraphNode, node: GraphNode): boolean {
  return node.order >= ancestor.order && node.order < ancestor.end
}
