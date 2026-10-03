import { useEffect, useRef, useState } from 'react'
import type { AnalysisSnapshot } from '../bridge/models'
import { buildGraphModel } from '../graph/model'
import { layoutGraph } from '../graph/layout'
import { easeInOut, fitBox, flight } from '../graph/camera'
import { GraphRenderer } from '../graph/renderer'
import { readTheme } from '../graph/theme'
import { HEIGHT, WIDTH, durationMs, type TourPlan, type TourStop } from './model'
import './tour.css'

export function RenderApp() {
  const canvas = useRef<HTMLCanvasElement>(null)
  const [stop, setStop] = useState<TourStop | null>(null)
  const [title, setTitle] = useState('Telescode')
  const [path, setPath] = useState<TourStop[]>([])
  const [activeIndex, setActiveIndex] = useState(0)
  useEffect(() => {
    let disposed = false
    let renderer: GraphRenderer | undefined
    const init = async () => {
      const response = await fetch(`/tour-api/payload${window.location.search}`)
      if (!response.ok) throw new Error('Tour payload unavailable')
      const { plan, snapshot } = await response.json() as { plan: TourPlan; snapshot: AnalysisSnapshot }
      const text = plan.title + plan.stops.map(s => s.title).join('') + 'Analysis evidence'
      const font = getComputedStyle(document.body).fontFamily
      await document.fonts.load(`20px ${font}`, text)
      await document.fonts.ready
      if (disposed || !canvas.current) return
      const model = buildGraphModel(snapshot, plan.title)
      layoutGraph(model)
      renderer = new GraphRenderer(model, { canvas: canvas.current, theme: readTheme(canvas.current), reducedMotion: true, callbacks: {} })
      // Reserve the right column for evidence so no graph content is obscured.
      renderer.resize(WIDTH - 380, HEIGHT - 80, 1)
      renderer.fit(false)
      let camera = { ...renderer.camera }
      let time = 0
      const timeline = plan.stops.map(s => {
        const node = model.byId.get(s.nodeId)!
        const target = fitBox(renderer!.viewport, node, 0.08, renderer!.minK, renderer!.maxK)
        const path = flight(camera, target, renderer!.viewport)
        const item = { s, node, path, target, start: time, end: time + s.transitionMs + s.holdMs }
        time = item.end
        camera = target
        return item
      })
      setTitle(plan.title)
      setPath(plan.stops)
      const seek = async (ms: number) => {
        const item = timeline.find(s => ms < s.end) ?? timeline.at(-1)!
        setActiveIndex(timeline.indexOf(item))
        const t = item.s.transitionMs === 0 ? 1 : Math.min(1, Math.max(0, (ms - item.start) / item.s.transitionMs))
        renderer!.camera = item.path.at(easeInOut(t))
        renderer!.select(t === 1 ? item.node : null)
        setStop(t === 1 ? item.s : null)
        renderer!.invalidate()
        await new Promise<void>(resolve => requestAnimationFrame(() => requestAnimationFrame(() => resolve())))
      }
      await seek(0)
      window.telescodeTour = { seek, durationMs: durationMs(plan) }
    }
    void init().catch(e => { window.tourError = String(e) })
    return () => { disposed = true; renderer?.destroy(); delete window.telescodeTour }
  }, [])
  return <main className="tour-screen">
    <header>{title}<span>Telescode · Onboarding</span></header>
    <canvas ref={canvas} />
    <aside aria-label="Analysis evidence">
      <nav aria-label="Reading path"><p>읽기 경로 · {activeIndex + 1}/{path.length}</p><ol>{path.map((s, i) => <li key={i} aria-current={i === activeIndex ? 'step' : undefined} className={i < activeIndex ? 'visited' : ''}>{s.title}</li>)}</ol></nav>
      {stop && <><h2>{stop.title}</h2><p>Analysis evidence</p><dl>{stop.evidence.map(e => <div key={e.label}><dt>{e.label}</dt><dd>{e.value}</dd></div>)}</dl></>}
    </aside>
  </main>
}
