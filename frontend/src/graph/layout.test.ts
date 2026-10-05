import { describe, expect, it } from 'vitest'

import type { GraphResponse } from '../bridge'
import sherlock from './fixtures/sherlock.graph.json'
import { layoutGraph, squarify, WORLD } from './layout'
import { buildGraphModel, type GraphModel, type GraphNode } from './model'

const graph = sherlock as GraphResponse

function build(input: GraphResponse = graph): GraphModel {
  const model = buildGraphModel(input, 'sherlock')
  layoutGraph(model)
  return model
}

const EPS = 1e-6
const within = (inner: GraphNode, outer: GraphNode) =>
  inner.x >= outer.x - EPS &&
  inner.y >= outer.y - EPS &&
  inner.x + inner.w <= outer.x + outer.w + EPS &&
  inner.y + inner.h <= outer.y + outer.h + EPS
const overlap = (a: GraphNode, b: GraphNode) =>
  Math.min(a.x + a.w, b.x + b.w) - Math.max(a.x, b.x) > EPS && Math.min(a.y + a.h, b.y + b.h) - Math.max(a.y, b.y) > EPS

describe('squarify', () => {
  it('fills the rect exactly, with areas proportional to the values', () => {
    const rect = { x: 10, y: 20, w: 300, h: 200 }
    const values = [6, 6, 4, 3, 2, 2, 1]
    const cells = squarify(values, rect)
    const total = values.reduce((a, b) => a + b, 0)
    cells.forEach((c, i) => expect(c.w * c.h).toBeCloseTo((values[i] / total) * rect.w * rect.h, 6))
    expect(cells.reduce((a, c) => a + c.w * c.h, 0)).toBeCloseTo(rect.w * rect.h, 6)
  })

  it('keeps cells close to square', () => {
    const cells = squarify([5, 4, 3, 3, 2, 2, 1, 1], { x: 0, y: 0, w: 400, h: 300 })
    for (const c of cells) expect(Math.max(c.w / c.h, c.h / c.w)).toBeLessThan(3)
  })
})

describe('layoutGraph', () => {
  const model = build()

  it('places the root on the world box', () => {
    expect(model.root).toMatchObject({ x: 0, y: 0, w: WORLD.width, h: WORLD.height })
  })

  it('keeps every node inside its parent, with room for the header', () => {
    for (const n of model.nodes) {
      expect(n.w).toBeGreaterThan(0)
      expect(n.h).toBeGreaterThan(0)
      if (!n.parent) continue
      expect(within(n, n.parent)).toBe(true)
      if (n.kind !== 'member') expect(n.y).toBeGreaterThanOrEqual(n.parent.y + n.parent.header - EPS)
    }
  })

  it('never overlaps siblings', () => {
    for (const n of model.nodes) {
      const c = n.children
      for (let i = 0; i < c.length; i++) for (let j = i + 1; j < c.length; j++) expect(overlap(c[i], c[j])).toBe(false)
    }
  })

  it('leaves clear space between neighbouring top-level areas', () => {
    const c = model.root.children
    const short = Math.min(WORLD.width, WORLD.height)
    for (let i = 0; i < c.length; i++)
      for (let j = i + 1; j < c.length; j++) {
        const [a, b] = [c[i], c[j]]
        const apart = Math.max(b.x - (a.x + a.w), a.x - (b.x + b.w), b.y - (a.y + a.h), a.y - (b.y + b.h))
        expect(apart).toBeGreaterThanOrEqual(short * 0.02 - EPS)
      }
  })

  it('leaves space between a class header rule and its first member', () => {
    for (const cls of model.nodes) {
      if (cls.kind !== 'class' || !cls.children.length) continue
      const rule = cls.y + cls.pad + cls.header
      expect(cls.children[0].y - rule).toBeGreaterThanOrEqual(cls.rowHeight * 0.3)
    }
  })

  it('is deterministic, whatever order the core lists files in', () => {
    const again = build({ ...graph, files: [...graph.files].reverse(), classes: [...graph.classes].reverse() })
    for (const n of model.nodes) {
      const m = again.byId.get(n.id)!
      expect([m.x, m.y, m.w, m.h]).toEqual([n.x, n.y, n.w, n.h])
    }
  })

  it('flows large classes into columns so rows stay readable', () => {
    const [file] = graph.files
    const big = build({
      ...graph,
      files: [file],
      classes: [
        {
          classId: `${file.fileId}::Big`,
          nodeId: 0,
          fileId: file.fileId,
          className: 'Big',
          package: '',
          fields: [],
          methods: Array.from({ length: 90 }, (_, i) => ({ access: '+' as const, name: `m${i}`, params: '', returnType: 'void' })),
        },
      ],
      classEdges: [],
      fileGraph: { nodes: [], edges: [] },
    })
    const cls = big.nodes.find((n) => n.kind === 'class')!
    const columns = new Set(cls.children.map((m) => m.x))
    expect(columns.size).toBeGreaterThan(1)
    for (const m of cls.children) expect(within(m, cls)).toBe(true)
  })
})
