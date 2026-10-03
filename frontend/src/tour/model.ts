import type { AnalysisSnapshot } from '../bridge/models'
import { buildGraphModel } from '../graph/model'

export interface StopInput {
  nodeId: string
  caption: string
  transitionMs: number
  holdMs: number
}
export interface Evidence { label: string; value: string }
export interface TourStop extends StopInput { title: string; evidence: Evidence[] }
export interface TourPlan {
  schemaVersion: 1
  id: string
  revision: number
  title: string
  language: 'ko' | 'en'
  snapshotHash: string
  stops: TourStop[]
}
export const FPS = 30
export const WIDTH = 1920
export const HEIGHT = 1080
export const durationMs = (plan: TourPlan) => plan.stops.reduce((sum, s) => sum + s.transitionMs + s.holdMs, 0)

export function inspectNode(snapshot: AnalysisSnapshot, nodeId: string) {
  const model = buildGraphModel(snapshot, 'Repository')
  const node = model.byId.get(nodeId)
  if (!node || node.kind === 'member') throw new Error(`Unsupported tour node: ${nodeId}`)
  const evidence: Evidence[] = []
  const add = (label: string, value: number | null | undefined) => {
    if (value != null && Number.isFinite(value)) evidence.push({ label, value: String(value) })
  }
  if (node.kind === 'root' || node.kind === 'dir') {
    add('Files', node.fileCount)
    add('Classes', node.classCount)
  } else {
    const cls = node.kind === 'class' ? snapshot.classes.find(c => `class:${c.classId}` === nodeId) : undefined
    const fileId = cls?.fileId ?? nodeId.slice(5)
    const file = snapshot.files.find(f => f.fileId === fileId)
    const graph = snapshot.fileGraph.nodes.find(f => f.fileId === fileId)
    const rank = snapshot.readingSequence.find(s => s.entityType === 'file' && s.fileId === fileId)
    if (cls) {
      add('Methods', cls.methods.length)
      add('Fields', cls.fields.length)
      add('Relationships', snapshot.classEdges.filter(e => e.source === cls.classId || e.target === cls.classId).length)
      add('Containing file rank', rank?.fileRank)
    } else {
      add('Reading rank', rank?.fileRank)
      add('Inbound imports', graph?.inbound)
      add('Outbound imports', graph?.outbound)
      add('Complexity score', file?.metrics.complexityScore)
    }
  }
  return {
    nodeId, kind: node.kind, title: node.kind === 'file' ? nodeId.slice(5) : node.label,
    evidence,
    children: node.children.filter(n => n.kind !== 'member').map(n => ({ nodeId: n.id, kind: n.kind, title: n.label })),
    relationships: model.edges.filter(e => e.source === node || e.target === node).slice(0, 100)
      .map(e => ({ source: e.source.id, target: e.target.id, kind: e.kind })),
  }
}

export function makeStops(snapshot: AnalysisSnapshot, inputs: StopInput[]): TourStop[] {
  if (!Array.isArray(inputs) || inputs.length < 1 || inputs.length > 8) throw new Error('A tour requires 1–8 stops')
  let total = 0
  return inputs.map(input => {
    if (typeof input.caption !== 'string' || !input.caption.trim() || input.caption.length > 300 || [...input.caption].some(c => c.charCodeAt(0) < 9)) {
      throw new Error('Caption must contain 1–300 characters')
    }
    for (const [key, value] of Object.entries({ transitionMs: input.transitionMs, holdMs: input.holdMs })) {
      if (!Number.isInteger(value) || value < (key === 'holdMs' ? 2000 : 0) || value > 120000) throw new Error(`Invalid ${key}`)
    }
    total += input.transitionMs + input.holdMs
    if (total > 120000) throw new Error('Tour exceeds 120 seconds')
    const info = inspectNode(snapshot, input.nodeId)
    return { ...input, caption: input.caption.trim(), title: info.title, evidence: info.evidence }
  })
}

function stamp(ms: number) {
  const value = Math.round(ms)
  return `${String(Math.floor(value / 3600000)).padStart(2, '0')}:${String(Math.floor(value / 60000) % 60).padStart(2, '0')}:${String(Math.floor(value / 1000) % 60).padStart(2, '0')}.${String(value % 1000).padStart(3, '0')}`
}
export function toVtt(plan: TourPlan): string {
  let time = 0
  const cues = plan.stops.map((s, i) => {
    const start = time + s.transitionMs
    time += s.transitionMs + s.holdMs
    // Escape cue text so repository-derived text is never interpreted as VTT markup.
    const caption = s.caption.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/\r?\n\s*\r?\n/g, '\n')
    return `${i + 1}\n${stamp(start)} --> ${stamp(time)}\n${caption}\n`
  })
  return `WEBVTT\n\n${cues.join('\n')}`
}

declare global {
  interface Window {
    telescodeTour?: { seek(ms: number): Promise<void>; durationMs: number }
    tourError?: string
  }
}
