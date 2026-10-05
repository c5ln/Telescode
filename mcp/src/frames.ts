import { FPS, durationMs, tourTimeline, tourFrameState, type TourPlan } from '../../frontend/src/tour/model.ts'

export interface FrameStats { capturedFrames: number; reusedFrames: number }

// Only the current stop's settled image is retained. The capture callback must
// await seek and UI painting before returning the PNG.
export async function* tourFrames(plan: TourPlan, capture: (ms: number) => Promise<Buffer>, stats: FrameStats) {
  const timeline = tourTimeline(plan.stops)
  const totalFrames = Math.ceil(durationMs(plan) * FPS / 1000)
  let cached: { index: number; png: Buffer } | undefined
  for (let frame = 0; frame < totalFrames; frame++) {
    const ms = frame * 1000 / FPS
    const { index, holding } = tourFrameState(timeline, ms)
    let png: Buffer
    if (holding && cached?.index === index) {
      png = cached.png
      stats.reusedFrames++
    } else {
      cached = undefined
      png = await capture(ms)
      stats.capturedFrames++
      if (holding) cached = { index, png }
    }
    yield png
  }
}
