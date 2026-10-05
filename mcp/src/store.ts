import { createHash, randomUUID } from 'node:crypto'
import { readFile, readdir, mkdir, writeFile, rename, stat } from 'node:fs/promises'
import { execFile } from 'node:child_process'
import { promisify } from 'node:util'
import { resolve } from 'node:path'
import type { AnalysisSnapshot } from '../../frontend/src/bridge/models.ts'
import { parseAnalysis, parseGraph } from '../../frontend/src/bridge/parse.ts'
import { makeStops, type StopInput, type TourPlan } from '../../frontend/src/tour/model.ts'

export interface Project { id: string; snapshot: AnalysisSnapshot; hash: string }
export interface Draft {
  plan: TourPlan
  project: Project
  reviewToken: string
  approvedRevision?: number
  state: 'draft' | 'approved' | 'rendering' | 'completed' | 'failed'
  jobId?: string
  output?: string
  error?: string
}

export class TourStore {
  projects = new Map<string, Project>()
  drafts = new Map<string, Draft>()
  constructor(readonly outputRoot: string) {}
  async restore() {
    await mkdir(this.outputRoot, { recursive: true })
    for (const name of await readdir(this.outputRoot)) {
      if (!name.endsWith('.draft.json')) continue
      const draft = JSON.parse(await readFile(resolve(this.outputRoot, name), 'utf8')) as Draft
      if (!draft.project || !draft.reviewToken) continue
      try { makeStops(draft.project.snapshot, draft.plan.stops) } catch (error) {
        draft.state = 'draft'
        draft.approvedRevision = undefined
        draft.error = `Plan needs revision under current onboarding limits: ${String(error)}`
      }
      const hash = createHash('sha256').update(JSON.stringify(draft.project.snapshot)).digest('hex')
      if (hash !== draft.project.hash || hash !== draft.plan.snapshotHash) throw new Error(`Corrupt snapshot for ${draft.plan.id}`)
      if (draft.state === 'rendering') {
        draft.state = 'failed'
        draft.error = 'Rendering interrupted by server restart; revise and approve the tour again'
      }
      this.projects.set(draft.project.id, draft.project)
      this.drafts.set(draft.plan.id, draft)
    }
  }
  /**
   * Every saved tour, read from disk rather than memory so tours another
   * server process saved are included. Without the snapshot, which can be large.
   */
  async list() {
    const names = await readdir(this.outputRoot).catch(() => [] as string[])
    const tours = await Promise.all(names.filter(name => name.endsWith('.draft.json')).map(async name => {
      const path = resolve(this.outputRoot, name)
      try {
        const draft = JSON.parse(await readFile(path, 'utf8')) as Draft
        if (!draft.plan || typeof draft.project?.hash !== 'string') return []
        return [{ plan: draft.plan, snapshotHash: draft.project.hash, state: draft.state, updatedMs: Math.round((await stat(path)).mtimeMs) }]
      } catch { return [] }
    }))
    return tours.flat()
  }
  async open(path: string, headless?: string) {
    const absolute = resolve(path)
    const raw = absolute.toLowerCase().endsWith('.json') ? await readFile(absolute, 'utf8')
      : (await promisify(execFile)(headless ?? process.env.TELESCODE_HEADLESS ?? 'TelescodeHeadless', ['analyze', absolute], { maxBuffer: 128 * 1024 * 1024 })).stdout
    const snapshot = 'readingSequence' in JSON.parse(raw) ? parseAnalysis(raw) : { ...parseGraph(raw), readingSequence: [] }
    const project = { id: randomUUID(), snapshot, hash: createHash('sha256').update(JSON.stringify(snapshot)).digest('hex') }
    this.projects.set(project.id, project)
    return project
  }
  project(id: string) {
    const project = this.projects.get(id)
    if (!project) throw new Error('Project not found; call open_project first')
    return project
  }
  draft(id: string) {
    const draft = this.drafts.get(id)
    if (!draft) throw new Error('Tour not found')
    return draft
  }
  async save(draft: Draft) {
    await mkdir(this.outputRoot, { recursive: true })
    const path = resolve(this.outputRoot, `${draft.plan.id}.draft.json`)
    const temporary = `${path}.${randomUUID()}.tmp`
    await writeFile(temporary, JSON.stringify(draft, null, 2))
    await rename(temporary, path)
  }
  async create(projectId: string, title: string, language: 'ko' | 'en', stops: StopInput[]) {
    const project = this.project(projectId)
    if (!title.trim() || title.length > 200) throw new Error('Title must contain 1–200 characters')
    const plan: TourPlan = { schemaVersion: 1, id: randomUUID(), revision: 1, title, language, snapshotHash: project.hash, stops: makeStops(project.snapshot, stops) }
    const draft: Draft = { plan, project, reviewToken: randomUUID(), state: 'draft' }
    this.drafts.set(plan.id, draft)
    await this.save(draft)
    return draft
  }
  async revise(id: string, expectedRevision: number, title: string, stops: StopInput[]) {
    const draft = this.draft(id)
    if (draft.plan.revision !== expectedRevision) throw new Error('Stale tour revision')
    if (draft.state === 'rendering') throw new Error('Cannot revise a rendering tour')
    if (!title.trim() || title.length > 200) throw new Error('Invalid title')
    const updated = makeStops(draft.project.snapshot, stops)
    draft.plan = { ...draft.plan, revision: draft.plan.revision + 1, title, stops: updated }
    draft.approvedRevision = undefined
    draft.reviewToken = randomUUID()
    draft.state = 'draft'
    draft.error = undefined
    draft.output = undefined
    await this.save(draft)
    return draft
  }
  async approve(id: string, revision: number, token: string) {
    const draft = this.draft(id)
    if (draft.reviewToken !== token || draft.plan.revision !== revision || draft.state !== 'draft') throw new Error('Review expired or revision changed')
    draft.approvedRevision = revision
    draft.state = 'approved'
    await this.save(draft)
  }
  assertApproved(draft: Draft, revision: number) {
    makeStops(draft.project.snapshot, draft.plan.stops)
    if (draft.state !== 'approved' || draft.approvedRevision !== revision || draft.plan.revision !== revision) throw new Error('User approval required for this exact revision; open the review URL')
    if (draft.plan.snapshotHash !== draft.project.hash) throw new Error('Snapshot changed')
  }
}
