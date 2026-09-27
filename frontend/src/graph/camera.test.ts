import { describe, expect, it } from 'vitest'

import { fitBox, flight, toScreenX, toScreenY, toWorld, zoomAround } from './camera'

const v = { width: 800, height: 600 }

describe('camera', () => {
  it('round-trips between world and screen', () => {
    const c = { x: 120, y: -40, k: 2.5 }
    const [wx, wy] = toWorld(c, v, 333, 111)
    expect(toScreenX(c, v, wx)).toBeCloseTo(333)
    expect(toScreenY(c, v, wy)).toBeCloseTo(111)
  })

  it('zooms around the pointer: the point under it stays put', () => {
    const c = { x: 500, y: 300, k: 1 }
    const [wx, wy] = toWorld(c, v, 650, 180)
    const z = zoomAround(c, v, 3, 650, 180, 0.1, 10)
    expect(z.k).toBe(3)
    expect(toScreenX(z, v, wx)).toBeCloseTo(650)
    expect(toScreenY(z, v, wy)).toBeCloseTo(180)
  })

  it('clamps zoom to its limits', () => {
    const c = { x: 0, y: 0, k: 1 }
    expect(zoomAround(c, v, 100, 0, 0, 0.5, 4).k).toBe(4)
    expect(zoomAround(c, v, 0.001, 0, 0, 0.5, 4).k).toBe(0.5)
  })

  it('fits a box inside the viewport with a margin', () => {
    const c = fitBox(v, { x: 100, y: 100, w: 200, h: 100 }, 0.1, 0.01, 100)
    expect(c.x).toBe(200)
    expect(c.y).toBe(150)
    expect(200 * c.k).toBeLessThanOrEqual(v.width)
    expect(100 * c.k).toBeLessThanOrEqual(v.height)
    expect(200 * c.k * 1.2).toBeCloseTo(v.width)
  })

  it('flies smoothly between two views, ending exactly on the target', () => {
    const from = { x: 0, y: 0, k: 1 }
    const to = { x: 2000, y: 500, k: 4 }
    const f = flight(from, to, v)
    expect(f.at(0).x).toBeCloseTo(0)
    expect(f.at(0).k).toBeCloseTo(1)
    expect(f.at(1)).toEqual(to)
    // Continuous: no jumps between nearby times.
    let prev = f.at(0)
    for (let t = 0.02; t <= 1; t += 0.02) {
      const cur = f.at(t)
      expect(Math.abs(Math.log(cur.k / prev.k))).toBeLessThan(0.25)
      expect(Math.hypot(cur.x - prev.x, cur.y - prev.y)).toBeLessThan(200)
      prev = cur
    }
    // A far jump pulls back for context before closing in.
    expect(Math.min(...[0.25, 0.5, 0.75].map((t) => f.at(t).k))).toBeLessThan(1)
  })

  it('handles pure zoom flights', () => {
    const f = flight({ x: 5, y: 5, k: 1 }, { x: 5, y: 5, k: 8 }, v)
    expect(f.at(0.5).k).toBeCloseTo(Math.sqrt(8), 5)
    expect(f.at(1).k).toBeCloseTo(8)
  })
})
