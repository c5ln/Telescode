// Name-based search across graph nodes (files, classes, methods, and fields).

import type { GraphModel, GraphNode } from '../graph/model'
import type { ResultKind, SearchResult } from './search'

export type { ResultKind, SearchResult } from './search'

export interface NameEntry {
  id: string
  kind: ResultKind
  label: string
  labelLower: string
  subtitle: string
  fileId: string
  fileIdLower: string
  pieces: string[]
}

const PIECE_RE = /[A-Z]+(?![a-z])|[A-Z][a-z0-9]*|[a-z0-9]+/g

const KIND_RANK: Record<ResultKind, number> = {
  class: 0,
  file: 1,
  method: 2,
  field: 3,
}

function stripFileExtension(label: string): string {
  const dot = label.lastIndexOf('.')
  return dot > 0 ? label.slice(0, dot) : label
}

function splitPieces(label: string): string[] {
  const matches = label.match(PIECE_RE)
  return matches ? matches.map((part) => part.toLowerCase()) : []
}

function fileIdFromNode(fileNode: GraphNode | null): string {
  if (!fileNode) return ''
  return fileNode.id.startsWith('file:') ? fileNode.id.slice(5) : fileNode.id
}

export function buildNameIndex(model: GraphModel): NameEntry[] {
  const entries: NameEntry[] = []

  for (const node of model.nodes) {
    if (node.kind === 'root' || node.kind === 'dir') continue

    if (node.kind === 'file') {
      const fileId = fileIdFromNode(node)
      entries.push({
        id: node.id,
        kind: 'file',
        label: node.label,
        labelLower: node.label.toLowerCase(),
        subtitle: fileId,
        fileId,
        fileIdLower: fileId.toLowerCase(),
        pieces: splitPieces(stripFileExtension(node.label)),
      })
    } else if (node.kind === 'class') {
      const fileId = fileIdFromNode(node.parent)
      entries.push({
        id: node.id,
        kind: 'class',
        label: node.label,
        labelLower: node.label.toLowerCase(),
        subtitle: fileId,
        fileId,
        fileIdLower: fileId.toLowerCase(),
        pieces: splitPieces(node.label),
      })
    } else if (node.kind === 'member') {
      const classNode = node.parent
      const fileId = fileIdFromNode(classNode?.parent ?? null)
      const kind: ResultKind = node.memberKind === 'field' ? 'field' : 'method'
      entries.push({
        id: node.id,
        kind,
        label: node.label,
        labelLower: node.label.toLowerCase(),
        subtitle: classNode ? `${classNode.label} · ${fileId}` : fileId,
        fileId,
        fileIdLower: fileId.toLowerCase(),
        pieces: splitPieces(node.label),
      })
    }
  }

  return entries
}

function matchTier(entry: NameEntry, query: string): number {
  if (entry.labelLower === query) return 1
  if (entry.labelLower.startsWith(query)) return 2
  if (entry.pieces.some((piece) => piece.startsWith(query))) return 3
  if (entry.labelLower.includes(query)) return 4
  if (entry.kind === 'file' && entry.fileIdLower.includes(query)) return 4
  return 0
}

function compareStrings(a: string, b: string): number {
  return a < b ? -1 : a > b ? 1 : 0
}

export function searchNames(index: readonly NameEntry[], query: string, limit = 15): SearchResult[] {
  const q = query.trim().toLowerCase()
  if (!q || limit <= 0) return []

  const matched: Array<{ entry: NameEntry; tier: number }> = []
  for (const entry of index) {
    const tier = matchTier(entry, q)
    if (tier > 0) {
      matched.push({ entry, tier })
    }
  }

  matched.sort((a, b) => {
    if (a.tier !== b.tier) return a.tier - b.tier
    const kindDiff = KIND_RANK[a.entry.kind] - KIND_RANK[b.entry.kind]
    if (kindDiff !== 0) return kindDiff
    const lenDiff = a.entry.label.length - b.entry.label.length
    if (lenDiff !== 0) return lenDiff
    const lowDiff = compareStrings(a.entry.labelLower, b.entry.labelLower)
    if (lowDiff !== 0) return lowDiff
    const exactDiff = compareStrings(a.entry.label, b.entry.label)
    if (exactDiff !== 0) return exactDiff
    return compareStrings(a.entry.id, b.entry.id)
  })

  return matched.slice(0, limit).map(({ entry }) => ({
    id: entry.id,
    kind: entry.kind,
    label: entry.label,
    subtitle: entry.subtitle,
    source: 'name',
  }))
}
