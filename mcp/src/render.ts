import { spawn } from 'node:child_process'
import { once } from 'node:events'
import { mkdir, writeFile, rename } from 'node:fs/promises'
import { resolve } from 'node:path'
import { chromium } from './browser.ts'
import { FPS, WIDTH, HEIGHT, CAPTION_BASE_FONT_SIZE_PX, CAPTION_SCALE, CAPTION_FONT_SIZE_PX, durationMs, toVtt, makeStops } from '../../frontend/src/tour/model.ts'
import type { Draft } from './store.ts'
import { tourFrames } from './frames.ts'

// MP4/H.264 plays in Slack, Notion, PowerPoint, QuickTime and iOS; WebM stays available for browser-only use.
const VIDEO_FORMATS = {
  mp4: { codec: 'h264', mime: 'video/mp4', args: ['-c:v', 'libx264', '-preset', 'medium', '-crf', '18', '-pix_fmt', 'yuv420p', '-movflags', '+faststart'] },
  webm: { codec: 'vp8', mime: 'video/webm', args: ['-c:v', 'libvpx', '-deadline', 'good', '-cpu-used', '4', '-crf', '10', '-b:v', '3M', '-pix_fmt', 'yuv420p'] },
} as const
export type VideoFormat = keyof typeof VIDEO_FORMATS
export function videoFormat(value = process.env.TELESCODE_VIDEO_FORMAT ?? 'mp4'): VideoFormat {
  if (value !== 'mp4' && value !== 'webm') throw new Error(`Unsupported TELESCODE_VIDEO_FORMAT: ${value}`)
  return value
}

export async function renderTour(draft: Draft, url: string, outputRoot: string, onProgress?: (frame: number, total: number) => void) {
  const started = performance.now()
  const stats = { capturedFrames: 0, reusedFrames: 0 }
  const plan = structuredClone(draft.plan)
  const snapshot = structuredClone(draft.project.snapshot)
  makeStops(snapshot, plan.stops)
  const format = videoFormat()
  const { codec, mime, args } = VIDEO_FORMATS[format]
  const output = resolve(outputRoot, plan.id, `revision-${plan.revision}`)
  await mkdir(output, { recursive: true })
  const browser = await chromium.launch({ headless: true })
  let encoder: ReturnType<typeof spawn> | undefined
  try {
    const page = await browser.newPage({ viewport: { width: WIDTH, height: HEIGHT }, deviceScaleFactor: 1, reducedMotion: 'reduce' })
    await page.goto(url)
    await page.waitForFunction(() => window.telescodeTour || window.tourError, undefined, { timeout: 30000 })
    const error = await page.evaluate(() => window.tourError)
    if (error) throw new Error(error)
    const totalFrames = Math.ceil(durationMs(plan) * FPS / 1000)
    // Frame-driven rendering excludes all agent/model latency and fixes VTT synchronization.
    encoder = spawn(process.env.TELESCODE_FFMPEG ?? 'ffmpeg', [
      '-hide_banner', '-loglevel', 'error', '-y', '-f', 'image2pipe', '-vcodec', 'png', '-framerate', String(FPS), '-i', 'pipe:0',
      '-an', ...args, resolve(output, `tour.partial.${format}`),
    ], { stdio: ['pipe', 'ignore', 'pipe'] })
    let stderr = ''
    encoder.stderr!.on('data', data => { stderr = (stderr + String(data)).slice(-8192) })
    encoder.stdin!.on('error', () => {})
    const finished = new Promise<void>((resolve, reject) => {
      encoder!.once('error', reject)
      encoder!.once('close', code => code === 0 ? resolve() : reject(new Error(`FFmpeg exited ${code}: ${stderr}`)))
    })
    // Attach a handler immediately so an early encoder failure is never unhandled.
    let encoderFailure: unknown
    void finished.catch(e => { encoderFailure = e })
    const frames = tourFrames(plan, async ms => {
      if (encoderFailure) throw encoderFailure
      await page.evaluate(ms => window.telescodeTour!.seek(ms), ms)
      return page.screenshot({ type: 'png', animations: 'allow' })
    }, stats)
    let frame = 0
    for await (const png of frames) {
      if (encoderFailure) throw encoderFailure
      if (!encoder.stdin!.write(png)) await Promise.race([once(encoder.stdin!, 'drain'), finished])
      onProgress?.(++frame, totalFrames)
    }
    encoder.stdin!.end()
    await finished
    await rename(resolve(output, `tour.partial.${format}`), resolve(output, `tour.${format}`))
    const token = encodeURIComponent(draft.reviewToken)
    await Promise.all([
      writeFile(resolve(output, 'tour.json'), JSON.stringify(plan, null, 2)),
      writeFile(resolve(output, 'snapshot.json'), JSON.stringify(snapshot)),
      writeFile(resolve(output, 'tour.vtt'), toVtt(plan)),
      writeFile(resolve(output, 'manifest.json'), JSON.stringify({ schemaVersion: 1, tourId: plan.id, revision: plan.revision, snapshotHash: plan.snapshotHash, createdAt: new Date().toISOString(), rendering: { ...stats, elapsedMs: Math.round(performance.now() - started) }, video: { file: `tour.${format}`, codec, width: WIDTH, height: HEIGHT, fps: FPS, durationMs: totalFrames * 1000 / FPS, audio: false }, subtitles: { format: 'webvtt', language: plan.language, embedded: false, burnedIn: true, baseFontSizePx: CAPTION_BASE_FONT_SIZE_PX, scale: CAPTION_SCALE, fontSizePx: CAPTION_FONT_SIZE_PX } }, null, 2)),
      writeFile(resolve(output, 'player.html'), `<!doctype html><html lang="${plan.language}"><meta charset="utf-8"><title>Telescode tour</title><style>body{margin:0;background:#121212;color:white;font-family:system-ui}video{width:100%;max-height:95vh}</style><video controls><source src="tour.${format}?token=${token}" type="${mime}"></video></html>`),
    ])
    return output
  } finally {
    if (encoder && encoder.exitCode === null) encoder.kill()
    await browser.close()
  }
}
