import { fileURLToPath } from 'node:url'
import { resolve } from 'node:path'
import { randomUUID } from 'node:crypto'
import { readFile } from 'node:fs/promises'
import { McpServer } from '@modelcontextprotocol/sdk/server/mcp.js'
import { StdioServerTransport } from '@modelcontextprotocol/sdk/server/stdio.js'
import { z } from 'zod'
import { TourStore } from './store.ts'
import { startHttp } from './http.ts'
import { renderTour } from './render.ts'
import { chromium } from './browser.ts'
import { buildGraphModel } from '../../frontend/src/graph/model.ts'
import { inspectNode, durationMs, readingTourOrder } from '../../frontend/src/tour/model.ts'

const root = fileURLToPath(new URL('../..', import.meta.url))
const store = new TourStore(resolve(process.env.TELESCODE_TOUR_OUTPUT ?? resolve(root, 'mcp/artifacts')))
await store.restore()
const http = await startHttp(store, resolve(root, 'frontend/dist'))
const progress = new Map<string, { frame: number; total: number }>()
const server = new McpServer({ name: 'telescode', version: '0.1.0' }, {
  instructions: await readFile(new URL('../prompts/onboarding.md', import.meta.url), 'utf8'),
})
const stop = z.object({ nodeId: z.string(), caption: z.string().min(1).max(300), transitionMs: z.number().int().min(0), holdMs: z.number().int().min(2000) })
function tool(name: string, description: string, schema: z.ZodRawShape, readOnly: boolean, handler: (args: any) => unknown | Promise<unknown>) {
  server.registerTool(name, { description, inputSchema: schema, annotations: { readOnlyHint: readOnly, destructiveHint: false, openWorldHint: false } }, async args => {
    try {
      const data = await handler(args)
      return { content: [{ type: 'text', text: JSON.stringify(data) }] }
    } catch (error) { return { isError: true, content: [{ type: 'text', text: String(error) }] } }
  })
}
tool('open_project', 'Open an existing SQLite analysis DB or graph/analyze JSON snapshot. Does not scan, clone, or modify the project.', { path: z.string(), headlessPath: z.string().optional() }, false, async ({ path, headlessPath }) => {
  const project = await store.open(path, headlessPath)
  return { projectId: project.id, snapshotHash: project.hash, totals: project.snapshot.totals }
})
tool('get_project_overview', 'Read repository totals, directories, and file reading ranks. Missing ranking data is reported explicitly.', { projectId: z.string() }, true, ({ projectId }) => {
  const p = store.project(projectId)
  return { totals: p.snapshot.totals, directories: buildGraphModel(p.snapshot, 'Repository').nodes.filter(n => n.kind === 'dir').map(n => ({ nodeId: n.id, title: n.label, files: n.fileCount, classes: n.classCount })), readingSequence: p.snapshot.readingSequence.filter(s => s.entityType === 'file').slice(0, 20), hasReadingSequence: p.snapshot.readingSequence.length > 0 }
})
tool('search_nodes', 'Find repository, directory, file and class IDs by case-insensitive text; use these IDs in tour stops.', { projectId: z.string(), query: z.string(), limit: z.number().int().min(1).max(100).default(30) }, true, ({ projectId, query, limit }) => {
  const model = buildGraphModel(store.project(projectId).snapshot, 'Repository')
  return model.nodes.filter(n => n.kind !== 'member' && `${n.id} ${n.label}`.toLowerCase().includes(query.toLowerCase())).slice(0, limit).map(n => ({ nodeId: n.id, kind: n.kind, title: n.label }))
})
tool('get_reading_tour_order', 'Read the onboarding path ordered by fileRank and class localRank. Maintain this relative order in tour stops.', { projectId: z.string() }, true, ({ projectId }) => readingTourOrder(store.project(projectId).snapshot))
tool('inspect_node', 'Read a node, its children, direct relationships and computed evidence. Class metrics do not inherit file scores silently.', { projectId: z.string(), nodeId: z.string() }, true, ({ projectId, nodeId }) => inspectNode(store.project(projectId).snapshot, nodeId))
tool('create_tour_draft', 'Save an onboarding tour (1–8 stops, at most 60 seconds) in reading sequence order; return the full plan and user review URL.', { projectId: z.string(), title: z.string(), language: z.enum(['ko', 'en']).default('ko'), stops: z.array(stop).min(1).max(8) }, false, async ({ projectId, title, language, stops }) => {
  const draft = await store.create(projectId, title, language, stops)
  return { plan: draft.plan, durationMs: durationMs(draft.plan), reviewUrl: http.reviewUrl(draft), state: draft.state }
})
tool('revise_tour_draft', 'Revise a saved plan; invalidates prior approval. Show the new review URL to the user.', { tourId: z.string(), expectedRevision: z.number().int(), title: z.string(), stops: z.array(stop).min(1).max(8) }, false, async ({ tourId, expectedRevision, title, stops }) => {
  const draft = await store.revise(tourId, expectedRevision, title, stops)
  return { plan: draft.plan, reviewUrl: http.reviewUrl(draft), durationMs: durationMs(draft.plan), state: draft.state }
})
tool('get_tour_status', 'Read draft approval and render job progress, errors and artifact links.', { tourId: z.string() }, true, ({ tourId }) => {
  const d = store.draft(tourId)
  return { plan: d.plan, state: d.state, approvedRevision: d.approvedRevision, jobId: d.jobId, progress: progress.get(tourId), error: d.error, outputDirectory: d.output, reviewUrl: http.reviewUrl(d), playerUrl: d.state === 'completed' ? http.playerUrl(d) : undefined }
})
server.registerTool('preview_tour_frame', {
  description: 'Observe a draft or approved tour at a requested time as a screenshot. Use this to verify focus and evidence before presenting the plan. Does not encode video or approve the tour.',
  inputSchema: { tourId: z.string(), timeMs: z.number().int().min(0) },
  annotations: { readOnlyHint: true, destructiveHint: false, openWorldHint: false },
}, async ({ tourId, timeMs }) => {
  const draft = store.draft(tourId)
  const session = http.registerRender(draft)
  const browser = await chromium.launch({ headless: true })
  try {
    if (timeMs >= durationMs(draft.plan)) throw new Error('Frame is outside tour duration')
    const page = await browser.newPage({ viewport: { width: 1920, height: 1080 }, deviceScaleFactor: 1 })
    await page.goto(session.url)
    await page.waitForFunction(() => window.telescodeTour || window.tourError)
    const error = await page.evaluate(() => window.tourError)
    if (error) throw new Error(error)
    await page.evaluate(ms => window.telescodeTour!.seek(ms), timeMs)
    const png = await page.screenshot()
    return { content: [{ type: 'image', data: png.toString('base64'), mimeType: 'image/png' }, { type: 'text', text: JSON.stringify({ tourId, revision: draft.plan.revision, timeMs }) }] }
  } finally { await browser.close(); session.dispose() }
})
tool('render_approved_tour', 'Start local Chromium/FFmpeg WebM and VTT generation for a user-approved revision. Returns a job ID immediately; poll get_tour_status. Cannot approve a draft.', { tourId: z.string(), approvedRevision: z.number().int() }, false, async ({ tourId, approvedRevision }) => {
  const d = store.draft(tourId)
  store.assertApproved(d, approvedRevision)
  const payload = http.registerRender(d)
  d.state = 'rendering'
  d.jobId = randomUUID()
  d.error = undefined
  await store.save(d)
  void renderTour(d, payload.url, store.outputRoot, (frame, total) => progress.set(tourId, { frame, total })).then(async output => {
    d.output = output
    d.state = 'completed'
    await store.save(d)
  }).catch(async error => {
    d.state = 'failed'
    d.error = String(error)
    await store.save(d)
  }).finally(payload.dispose).catch(error => console.error('Could not persist render result:', error))
  return { tourId, jobId: d.jobId, state: d.state }
})
console.error(`Telescode review server: ${http.baseUrl}`)
await server.connect(new StdioServerTransport())
