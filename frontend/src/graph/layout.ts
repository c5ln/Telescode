// One static, deterministic layout for every zoom level.
//
// Each container's children are placed inside its own box (a squarified
// treemap for directories and files, a list of rows for class members), so
// semantic zoom never needs a relayout: detail resolves where its parent
// already is. The result depends only on the tree and its weights, so the same
// repository always produces the same map.

import type { GraphModel, GraphNode } from './model'

/** The root's box. Its aspect roughly matches a desktop window. */
export const WORLD = { width: 1600, height: 1000 }

interface Rect {
  x: number
  y: number
  w: number
  h: number
}

/** Space around and between children, as fractions of the parent's short side. */
const SPACING = {
  root: { pad: 0.012, header: 0, gap: 0.02 },
  dir: { pad: 0.03, header: 0.085, gap: 0.1 },
  file: { pad: 0.05, header: 0.13, gap: 0.07 },
} as const

export function layoutGraph(model: GraphModel): void {
  const { root } = model
  place(root, { x: 0, y: 0, w: WORLD.width, h: WORLD.height })
}

function place(node: GraphNode, rect: Rect) {
  node.x = rect.x
  node.y = rect.y
  node.w = rect.w
  node.h = rect.h
  if (node.children.length === 0) return

  if (node.kind === 'class') {
    placeMembers(node)
    return
  }

  const s = SPACING[node.kind === 'root' ? 'root' : node.kind === 'dir' ? 'dir' : 'file']
  const short = Math.min(rect.w, rect.h)
  node.pad = short * s.pad
  // Directory headers scale with area, so long flat regions still get a
  // readable label band; files use their short side.
  node.header =
    node.kind === 'dir' ? Math.min(Math.sqrt(rect.w * rect.h) * 0.065, rect.h * 0.25) : short * s.header
  const inner = {
    x: rect.x + node.pad,
    y: rect.y + node.header + node.pad,
    w: rect.w - node.pad * 2,
    h: rect.h - node.header - node.pad * 2,
  }
  if (inner.w <= 0 || inner.h <= 0) return

  // Largest first, name as tie-break, so the order never depends on input order.
  const items = [...node.children].sort((a, b) => b.weight - a.weight || (a.id < b.id ? -1 : 1))
  const cells = squarify(
    items.map((c) => c.weight),
    inner,
  )
  const gap = short * s.gap * (node.kind === 'root' ? 1 : 0.5)
  items.forEach((child, i) => {
    const c = cells[i]
    const g = Math.min(gap, c.w * 0.15, c.h * 0.15) / 2
    place(child, { x: c.x + g, y: c.y + g, w: c.w - g * 2, h: c.h - g * 2 })
  })
}

/**
 * Class members are rows under a header row, like a compact class diagram.
 * Large classes flow into columns: the column count is whichever gives the
 * tallest rows that are still wide enough for a name (about 9 row heights).
 */
function placeMembers(cls: GraphNode) {
  const n = cls.children.length
  cls.pad = Math.min(cls.w, cls.h) * 0.06
  const availH = cls.h - cls.pad * 2
  const availW = cls.w - cls.pad * 2
  let best = { cols: 1, row: 0 }
  for (let cols = 1; cols <= Math.min(n, 6); cols++) {
    const perCol = Math.ceil(n / cols)
    // Header takes 1.6 rows; rows never get taller than a comfortable line.
    const row = Math.min(availH / (perCol + 1.6), availW / cols / 9, cls.w * 0.11)
    if (row > best.row * 1.001) best = { cols, row }
  }
  const { cols, row } = best
  const perCol = Math.ceil(n / cols)
  const colW = availW / cols
  cls.rowHeight = row
  cls.header = row * 1.6
  cls.children.forEach((m, i) => {
    const col = Math.floor(i / perCol)
    m.x = cls.x + cls.pad + col * colW
    m.y = cls.y + cls.pad + cls.header + (i % perCol) * row
    m.w = colW * 0.96
    m.h = row
  })
}

/**
 * Squarified treemap (Bruls, Huizing, van Wijk 2000): lays out rows along the
 * shorter side, adding items while the worst aspect ratio in the row improves.
 * Values must be sorted descending. Returns one rect per value, in order.
 */
export function squarify(values: number[], rect: Rect): Rect[] {
  const out: Rect[] = []
  const total = values.reduce((a, b) => a + b, 0)
  if (total <= 0 || values.length === 0) return values.map(() => ({ ...rect, w: 0, h: 0 }))

  const scale = (rect.w * rect.h) / total
  const areas = values.map((v) => v * scale)
  let { x, y, w, h } = rect
  let i = 0

  // Worst aspect ratio of a row with this sum, largest and smallest area.
  const worst = (sum: number, max: number, min: number, side: number) => {
    const s2 = side * side
    const sum2 = sum * sum
    return Math.max((s2 * max) / sum2, sum2 / (s2 * min))
  }

  while (i < areas.length) {
    const side = Math.min(w, h)
    const row = [areas[i]]
    let sum = areas[i]
    let max = areas[i]
    let min = areas[i]
    let j = i + 1
    while (j < areas.length) {
      const a = areas[j]
      const next = worst(sum + a, Math.max(max, a), Math.min(min, a), side)
      if (next > worst(sum, max, min, side)) break
      row.push(a)
      sum += a
      max = Math.max(max, a)
      min = Math.min(min, a)
      j++
    }
    if (w >= h) {
      // Column on the left, filled top to bottom.
      const cw = h > 0 ? sum / h : 0
      let cy = y
      for (const a of row) {
        const ch = cw > 0 ? a / cw : 0
        out.push({ x, y: cy, w: cw, h: ch })
        cy += ch
      }
      x += cw
      w -= cw
    } else {
      // Row along the top, filled left to right.
      const rh = w > 0 ? sum / w : 0
      let cx = x
      for (const a of row) {
        const cw = rh > 0 ? a / rh : 0
        out.push({ x: cx, y, w: cw, h: rh })
        cx += cw
      }
      y += rh
      h -= rh
    }
    i = j
  }
  return out
}
