import { describe, expect, it } from 'vitest'

import type { GraphResponse } from '../bridge'
import sherlock from './fixtures/sherlock.graph.json'
import { layoutGraph } from './layout'
import { buildGraphModel, contains, type GraphNode } from './model'
import { LodFrame, openness, reveal, settle } from './semantic'

const model = buildGraphModel(sherlock as GraphResponse, 'sherlock')
layoutGraph(model)
// From far beyond the whole-repository view to far inside the smallest class.
const scales = Array.from({ length: 500 }, (_, i) => 0.002 * Math.pow(1.03, i))

describe('semantic zoom', () => {
  it('opens every container continuously and monotonically as you zoom in', () => {
    for (const n of model.nodes) {
      let prev = 0
      for (const k of scales) {
        const o = openness(n, k)
        expect(o).toBeGreaterThanOrEqual(prev - 1e-12)
        expect(o - prev).toBeLessThan(0.35) // no hard switch between nearby scales
        prev = o
      }
      expect(openness(n, scales[0])).toBeLessThan(0.01)
      if (n.children.length) expect(prev).toBeCloseTo(1)
      else expect(prev).toBe(0)
    }
  })

  it('opens the label band before revealing the children', () => {
    expect(settle(0.55)).toBeCloseTo(1)
    expect(reveal(0.35)).toBe(0)
    for (let o = 0; o <= 1; o += 0.05) expect(settle(o)).toBeGreaterThanOrEqual(reveal(o))
  })

  it('shows children only inside a visible parent', () => {
    const lod = new LodFrame(model)
    for (const k of scales) {
      lod.begin(k)
      for (const n of model.nodes) if (n.parent) expect(lod.vis(n)).toBeLessThanOrEqual(lod.vis(n.parent) + 1e-12)
    }
  })

  it('ends edges only on boxes that are drawn, handing over smoothly from region to file', () => {
    const lod = new LodFrame(model)
    const file = model.byId.get('file:sherlock_project/result.py')!
    const dir = file.parent!
    let prev: Map<GraphNode, number> | null = null
    let sawDir = false
    let sawFile = false
    for (const k of scales) {
      lod.begin(k)
      const list = lod.standIns(file)
      expect(list.reduce((a, s) => a + s.weight, 0)).toBeCloseTo(1, 2)
      const now = new Map<GraphNode, number>()
      for (const s of list) {
        // Every stand-in is on the node's path and visible as a box of its own.
        expect(contains(s.node, file)).toBe(true)
        expect(lod.vis(s.node)).toBeGreaterThanOrEqual(s.weight - 1e-9)
        now.set(s.node, s.weight)
        if (s.node === dir && s.weight > 0.99) sawDir = true
        if (s.node === file && s.weight > 0.99) sawFile = true
      }
      // No line jumps: each stand-in's weight changes only a little between nearby scales.
      if (prev) for (const n of new Set([...prev.keys(), ...now.keys()])) expect(Math.abs((now.get(n) ?? 0) - (prev.get(n) ?? 0))).toBeLessThan(0.35)
      prev = now
    }
    expect(sawDir && sawFile).toBe(true)
  })
})
