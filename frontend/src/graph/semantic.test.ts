import { describe, expect, it } from 'vitest'

import type { GraphResponse } from '../bridge'
import sherlock from './fixtures/sherlock.graph.json'
import { layoutGraph } from './layout'
import { buildGraphModel } from './model'
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

  it('moves edge endpoints continuously from region to file', () => {
    const lod = new LodFrame(model)
    const file = model.byId.get('file:sherlock_project/result.py')!
    const dir = file.parent!
    const box = { x: 0, y: 0, w: 0, h: 0 }
    let prev: typeof box | null = null
    let sawDir = false
    let sawFile = false
    for (const k of scales) {
      lod.begin(k)
      const rep = lod.endpoint(file, box)
      if (rep === dir) sawDir = true
      if (rep === file) sawFile = true
      if (prev) expect(Math.hypot(box.x - prev.x, box.y - prev.y)).toBeLessThan(dir.w * 0.2)
      prev = { ...box }
    }
    expect(sawDir && sawFile).toBe(true)
  })
})
