// Complexity shading for the optional complexity mode.
//
// Each file's cyclomatic complexity is placed relative to the other files in
// the repository, so a hotspot stands out whatever the codebase's overall
// level. The range of shades used shrinks with the spread of the values:
// tightly clustered files stay in the lightest shades, and only a wide spread
// (a real hotspot) reaches the darkest.
//
// Only files carry complexity; every other node has no data and keeps its
// neutral appearance.

import type { GraphModel } from './model'

/** Number of shades, lightest first (see --graph-complexity-* in tokens.css). */
export const COMPLEXITY_LEVELS = 4
/** Level of a node without complexity data. */
export const NO_COMPLEXITY = -1

/**
 * Damps small values, where one extra branch is a large ratio but no real
 * difference (1 vs 2 should not read like 10 vs 20).
 */
const OFFSET = 2
/**
 * Spread, in log units of (value + OFFSET), that uses the whole range of
 * shades: the most complex file being six times the least complex one.
 */
const FULL_SPREAD = Math.log(6)

/**
 * A shade level per value, in [0, COMPLEXITY_LEVELS), or NO_COMPLEXITY for
 * missing values (null, or not positive: a file without functions).
 */
export function complexityLevels(values: readonly (number | null | undefined)[]): Int8Array {
  const out = new Int8Array(values.length).fill(NO_COMPLEXITY)
  let lo = Infinity
  let hi = -Infinity
  const logs = values.map((v) => {
    if (v == null || !(v > 0)) return NaN
    const x = Math.log(v + OFFSET)
    lo = Math.min(lo, x)
    hi = Math.max(hi, x)
    return x
  })
  const spread = hi - lo
  // How much of the scale this repository's spread earns.
  const contrast = Math.min(1, spread / FULL_SPREAD)
  for (let i = 0; i < logs.length; i++) {
    const x = logs[i]
    if (Number.isNaN(x)) continue
    const t = spread > 0 ? ((x - lo) / spread) * contrast : 0
    out[i] = Math.min(COMPLEXITY_LEVELS - 1, Math.floor(t * COMPLEXITY_LEVELS))
  }
  return out
}

/** A shade level per node, by order. */
export function modelComplexityLevels(model: GraphModel): Int8Array {
  return complexityLevels(model.nodes.map((n) => n.complexity))
}
