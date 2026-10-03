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
export const MAX_DURATION_MS = 60000
export const durationMs = (plan: TourPlan) => plan.stops.reduce((sum, s) => sum + s.transitionMs + s.holdMs, 0)

export function readingTourOrder(snapshot: AnalysisSnapshot) {
  const model = buildGraphModel(snapshot, 'Repository')
  const files = snapshot.readingSequence.filter(s => s.entityType === 'file' && s.fileRank != null && s.fileRank > 0)
    .sort((a, b) => a.fileRank! - b.fileRank! || a.fileId.localeCompare(b.fileId))
  return files.flatMap(file => {
    const fileNode = model.byId.get(`file:${file.fileId}`)
    if (!fileNode) return []
    const classes = fileNode.children.map(node => {
      const rank = snapshot.readingSequence.find(s => s.entityType === 'class' && `class:${s.entityId}` === node.id)?.localRank ?? null
      return { nodeId: node.id, title: node.label, fileRank: file.fileRank!, localRank: rank }
    }).sort((a, b) => (a.localRank ?? Infinity) - (b.localRank ?? Infinity) || a.nodeId.localeCompare(b.nodeId))
    return [{ nodeId: fileNode.id, title: file.fileId, fileRank: file.fileRank!, localRank: null }, ...classes]
  })
}

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
  const order = new Map(readingTourOrder(snapshot).map((node, index) => [node.nodeId, index]))
  let previous = -1
  return inputs.map(input => {
    if (typeof input.caption !== 'string' || !input.caption.trim() || input.caption.length > 300 || [...input.caption].some(c => c.charCodeAt(0) < 9)) {
      throw new Error('Caption must contain 1–300 characters')
    }
    for (const [key, value] of Object.entries({ transitionMs: input.transitionMs, holdMs: input.holdMs })) {
      if (!Number.isInteger(value) || value < (key === 'holdMs' ? 2000 : 0) || value > MAX_DURATION_MS) throw new Error(`Invalid ${key}`)
    }
    total += input.transitionMs + input.holdMs
    if (total > MAX_DURATION_MS) throw new Error('Tour exceeds 60 seconds')
    const info = inspectNode(snapshot, input.nodeId)
    if (info.kind === 'file' || info.kind === 'class') {
      const index = order.get(input.nodeId)
      if (index == null) throw new Error('Reading sequence required: run algo and load an analyze snapshot before touring files/classes')
      if (index < previous) throw new Error('Stops must follow reading sequence order (fileRank, then class localRank)')
      previous = index
    }
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
