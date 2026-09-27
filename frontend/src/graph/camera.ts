// Camera math. Pure functions: no DOM, no animation loop.
//
// A camera is the world point shown at the viewport centre plus a scale in
// screen pixels per world unit:
//
//   screen = (world - camera) * k + viewport / 2

export interface Camera {
  x: number
  y: number
  k: number
}

export interface Viewport {
  width: number
  height: number
}

export interface Box {
  x: number
  y: number
  w: number
  h: number
}

export function toScreenX(c: Camera, v: Viewport, wx: number): number {
  return (wx - c.x) * c.k + v.width / 2
}

export function toScreenY(c: Camera, v: Viewport, wy: number): number {
  return (wy - c.y) * c.k + v.height / 2
}

export function toWorld(c: Camera, v: Viewport, sx: number, sy: number): [number, number] {
  return [(sx - v.width / 2) / c.k + c.x, (sy - v.height / 2) / c.k + c.y]
}

/** The camera at scale `k` that keeps world point (wx, wy) under screen point (sx, sy). */
export function anchoredAt(v: Viewport, k: number, wx: number, wy: number, sx: number, sy: number): Camera {
  return { k, x: wx - (sx - v.width / 2) / k, y: wy - (sy - v.height / 2) / k }
}

/** Zoom by `factor` around a screen point, which stays fixed. */
export function zoomAround(c: Camera, v: Viewport, factor: number, sx: number, sy: number, min: number, max: number): Camera {
  const k = clamp(c.k * factor, min, max)
  const [wx, wy] = toWorld(c, v, sx, sy)
  return anchoredAt(v, k, wx, wy, sx, sy)
}

/** The camera that shows `box` whole, with `margin` (a fraction) around it. */
export function fitBox(v: Viewport, box: Box, margin: number, min: number, max: number): Camera {
  const k = Math.min(v.width / (box.w * (1 + margin * 2)), v.height / (box.h * (1 + margin * 2)))
  return { x: box.x + box.w / 2, y: box.y + box.h / 2, k: clamp(k, min, max) }
}

export function clamp(x: number, min: number, max: number): number {
  return Math.min(max, Math.max(min, x))
}

export interface Flight {
  /** Path length, in van Wijk's units: grows with both pan distance and zoom change. */
  length: number
  at(t: number): Camera
}

/**
 * Smooth zoom-and-pan between two views (van Wijk & Nuij, 2003). For distant
 * targets it pulls back just enough to keep both ends in context, so the move
 * reads as travel through one space rather than a cut. `rho` sets how much it
 * pulls back; lower is flatter.
 */
export function flight(from: Camera, to: Camera, v: Viewport, rho = 1.25): Flight {
  const w0 = v.width / from.k
  const w1 = v.width / to.k
  const dx = to.x - from.x
  const dy = to.y - from.y
  const d2 = dx * dx + dy * dy
  const rho2 = rho * rho
  const rho4 = rho2 * rho2

  if (d2 < 1e-12 * w0 * w0) {
    const S = Math.log(w1 / w0) / rho
    return {
      length: Math.abs(S),
      at: (t) => ({ x: from.x + t * dx, y: from.y + t * dy, k: v.width / (w0 * Math.exp(rho * t * S)) }),
    }
  }

  const d1 = Math.sqrt(d2)
  const b0 = (w1 * w1 - w0 * w0 + rho4 * d2) / (2 * w0 * rho2 * d1)
  const b1 = (w1 * w1 - w0 * w0 - rho4 * d2) / (2 * w1 * rho2 * d1)
  const r0 = Math.log(Math.sqrt(b0 * b0 + 1) - b0)
  const r1 = Math.log(Math.sqrt(b1 * b1 + 1) - b1)
  const S = (r1 - r0) / rho
  const coshR0 = Math.cosh(r0)
  const sinhR0 = Math.sinh(r0)

  return {
    length: S,
    at(t) {
      if (t >= 1) return { ...to }
      const s = t * S
      const u = (w0 / (rho2 * d1)) * (coshR0 * Math.tanh(rho * s + r0) - sinhR0)
      const w = (w0 * coshR0) / Math.cosh(rho * s + r0)
      return { x: from.x + u * dx, y: from.y + u * dy, k: v.width / w }
    },
  }
}

/** Short, direct flights; longer ones take a little longer, within bounds. */
export function flightDuration(f: Flight): number {
  return clamp(f.length * 320, 180, 650)
}

/** Ease in-out without a long tail: quick to start, settles without drifting. */
export function easeInOut(t: number): number {
  return t < 0.5 ? 2 * t * t : 1 - (-2 * t + 2) ** 2 / 2
}
