import { describe, expect, it } from 'vitest'

import type { GraphResponse } from '../bridge'
import { complexityLevels, modelComplexityLevels, NO_COMPLEXITY } from './complexity'
import sherlock from './fixtures/sherlock.graph.json'
import { buildGraphModel } from './model'

const levels = (values: (number | null)[]) => Array.from(complexityLevels(values))

describe('complexityLevels', () => {
  it('keeps tightly clustered values in the lightest shade', () => {
    expect(levels([6, 7, 7, 8, 8, 8, 9])).toEqual([0, 0, 0, 0, 0, 0, 0])
  })

  it('uses the whole range for a wide spread, darkest only for the hotspot', () => {
    expect(levels([2, 3, 4, 5, 8, 14, 27])).toEqual([0, 0, 0, 1, 1, 2, 3])
  })

  it('is relative to the codebase, not to fixed thresholds', () => {
    // Every file is "complex" by a fixed yardstick, yet the scale still separates them.
    const high = levels([20, 30, 40, 50, 80, 140, 270])
    expect(high[0]).toBe(0)
    expect(high.at(-1)).toBe(3)
    expect(levels([60, 70, 70, 80, 80, 80, 90])).toEqual([0, 0, 0, 0, 0, 0, 0])
  })

  it('gives files without data no level', () => {
    expect(levels([null, 0, 5])).toEqual([NO_COMPLEXITY, NO_COMPLEXITY, 0])
    expect(levels([])).toEqual([])
  })
})

describe('modelComplexityLevels', () => {
  it('shades files only', () => {
    const model = buildGraphModel(sherlock as GraphResponse, 'sherlock')
    const out = modelComplexityLevels(model)
    for (const n of model.nodes) {
      if (n.kind !== 'file') expect(out[n.order]).toBe(NO_COMPLEXITY)
    }
    // The fixture has one clear hotspot (cyclomatic complexity 48).
    expect(model.nodes.filter((n) => out[n.order] === 3).map((n) => n.complexity)).toEqual([48])
  })
})
