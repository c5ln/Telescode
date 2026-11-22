// Unified search state for the open repository.
//
// Builds the graph node index without running layout (D7) so node ids match
// CodeMap 1:1. Source files are loaded once via `read_sources` (D8) and
// indexed into BM25 asynchronously in ~12ms chunks (D9) so the main thread
// stays responsive. Query and index state are tied to the current snapshot.

import { invoke, isTauri } from '@tauri-apps/api/core'
import { useCallback, useEffect, useMemo, useRef, useState } from 'react'

import type { AnalysisSnapshot } from '../bridge'
import { buildGraphModel } from '../graph/model'
import { Bm25Builder, type Bm25Index } from '../search/bm25'
import { buildNameIndex, searchNames } from '../search/names'
import { mergeResults, type SearchResult } from '../search/search'
import { tokenize, tokenizePath } from '../search/tokenizer'

export type SearchIndexStatus = 'idle' | 'indexing' | 'ready'

export interface SourcesResult {
  sources: Record<string, string>
  skipped: number
}

export type SourcesLoader = (dbPath: string, fileIds: string[]) => Promise<SourcesResult>

export interface SearchState {
  query: string
  setQuery(q: string): void
  results: SearchResult[]
  status: SearchIndexStatus
  skipped: number
  reset(): void
}

const CHUNK_BUDGET_MS = 12
const BM25_DEBOUNCE_MS = 100
const BM25_LIMIT = 10

function yieldToMain(): Promise<void> {
  if (typeof MessageChannel !== 'undefined') {
    return new Promise((resolve) => {
      const channel = new MessageChannel()
      channel.port1.onmessage = () => {
        channel.port1.close()
        channel.port2.close()
        resolve()
      }
      channel.port2.postMessage(undefined)
    })
  }
  return new Promise((resolve) => setTimeout(resolve, 0))
}

async function tauriLoadSources(dbPath: string, fileIds: string[]): Promise<SourcesResult> {
  if (!isTauri()) return { sources: {}, skipped: 0 }
  return invoke<SourcesResult>('read_sources', { dbPath, fileIds })
}

/**
 * Orders snapshot files by `readingSequence` `fileRank` ascending, then
 * appends any remaining `snapshot.files` without a rank.
 */
export function orderedFileIds(snapshot: AnalysisSnapshot): string[] {
  const valid = new Set(snapshot.files.map((f) => f.fileId))
  const ranked = snapshot.readingSequence
    .filter((entry) => entry.entityType === 'file' && entry.fileRank !== null && valid.has(entry.fileId))
    .sort((a, b) => a.fileRank! - b.fileRank!)

  const seen = new Set<string>()
  const ordered: string[] = []
  for (const entry of ranked) {
    if (seen.has(entry.fileId)) continue
    seen.add(entry.fileId)
    ordered.push(entry.fileId)
  }
  for (const file of snapshot.files) {
    if (seen.has(file.fileId)) continue
    seen.add(file.fileId)
    ordered.push(file.fileId)
  }
  return ordered
}

interface IndexState {
  snapshot: AnalysisSnapshot | null
  status: SearchIndexStatus
  bm25: Bm25Index | null
  skipped: number
}

interface ContentState {
  snapshot: AnalysisSnapshot | null
  query: string
  results: SearchResult[]
}

export function useSearch(
  snapshot: AnalysisSnapshot | null,
  dbPath: string | null,
  loadSources: SourcesLoader = tauriLoadSources,
): SearchState {
  const [ownedQuery, setOwnedQuery] = useState<{ snapshot: AnalysisSnapshot | null; query: string }>({
    snapshot,
    query: '',
  })
  const query = ownedQuery.snapshot === snapshot ? ownedQuery.query : ''
  const setQuery = useCallback((next: string) => setOwnedQuery({ snapshot, query: next }), [snapshot])
  const reset = useCallback(() => setOwnedQuery({ snapshot, query: '' }), [snapshot])

  const model = useMemo(() => (snapshot ? buildGraphModel(snapshot, '') : null), [snapshot])
  const nameIndex = useMemo(() => (model ? buildNameIndex(model) : []), [model])
  const fileLabels = useMemo(() => {
    const labels = new Map<string, string>()
    if (!model) return labels
    for (const node of model.nodes) {
      if (node.kind === 'file') {
        const fileId = node.id.startsWith('file:') ? node.id.slice(5) : node.id
        labels.set(fileId, node.label)
      }
    }
    return labels
  }, [model])

  const [indexState, setIndexState] = useState<IndexState>({
    snapshot: null,
    status: 'idle',
    bm25: null,
    skipped: 0,
  })

  const generationRef = useRef(0)

  useEffect(() => {
    const invalidate = () => {
      generationRef.current++
    }
    const gen = ++generationRef.current
    if (!snapshot) {
      return invalidate
    }

    let cancelled = false
    const run = async () => {
      try {
        // Yield once so the canvas finishes its first render before indexing begins (D9).
        await yieldToMain()
        if (cancelled || gen !== generationRef.current) return

        const fileIds = orderedFileIds(snapshot)
        let loaded: SourcesResult = { sources: {}, skipped: 0 }
        const readStart = performance.now()
        if (dbPath) {
          try {
            loaded = await loadSources(dbPath, fileIds)
          } catch {
            loaded = { sources: {}, skipped: 0 }
          }
        }
        const readMs = performance.now() - readStart
        if (cancelled || gen !== generationRef.current) return

        const indexStart = performance.now()
        let maxChunkMs = 0
        const builder = new Bm25Builder()
        let chunkStart = performance.now()

        for (let i = 0; i < fileIds.length; i++) {
          const fileId = fileIds[i]
          const source = loaded.sources[fileId]
          const tokens = source
            ? [...tokenizePath(fileId), ...tokenize(source, { keepStopwords: true })]
            : tokenizePath(fileId)
          builder.add(fileId, tokens)

          const elapsed = performance.now() - chunkStart
          if (elapsed > maxChunkMs) maxChunkMs = elapsed
          if (elapsed >= CHUNK_BUDGET_MS && i + 1 < fileIds.length) {
            await yieldToMain()
            if (cancelled || gen !== generationRef.current) return
            chunkStart = performance.now()
          }
        }

        const bm25 = builder.finish()
        const totalIndexMs = performance.now() - indexStart
        if (cancelled || gen !== generationRef.current) return

        if (import.meta.env.DEV) {
          const totalChars = Object.values(loaded.sources).reduce((sum, s) => sum + s.length, 0)
          const metrics = {
            dbPath,
            fileCount: fileIds.length,
            loadedFiles: Object.keys(loaded.sources).length,
            skipped: loaded.skipped,
            totalChars,
            readMs,
            maxChunkMs,
            totalIndexMs,
          }
          ;(window as { __telescodeSearchMetrics?: typeof metrics }).__telescodeSearchMetrics = metrics
          if (import.meta.env.MODE !== 'test') {
            console.info('[search] BM25 index ready', metrics)
          }
        }

        setIndexState({ snapshot, status: 'ready', bm25, skipped: loaded.skipped })
      } catch {
        if (!cancelled && gen === generationRef.current) {
          setIndexState({ snapshot, status: 'idle', bm25: null, skipped: 0 })
        }
      }
    }

    void run()
    return () => {
      cancelled = true
      invalidate()
    }
  }, [snapshot, dbPath, loadSources])

  const status: SearchIndexStatus = !snapshot
    ? 'idle'
    : indexState.snapshot === snapshot
      ? indexState.status
      : 'indexing'
  const bm25 = snapshot && indexState.snapshot === snapshot ? indexState.bm25 : null
  const skipped = snapshot && indexState.snapshot === snapshot ? indexState.skipped : 0

  const [contentState, setContentState] = useState<ContentState>({
    snapshot: null,
    query: '',
    results: [],
  })

  useEffect(() => {
    if (!snapshot || !bm25 || query.trim().length < 2) return
    const timer = setTimeout(() => {
      const tokens = tokenize(query, { keepStopwords: true })
      const hits = bm25.top(tokens, BM25_LIMIT)
      const mapped: SearchResult[] = hits.map(({ docId }) => ({
        id: `file:${docId}`,
        kind: 'file',
        label: fileLabels.get(docId) ?? docId.split(/[\\/]/).pop() ?? docId,
        subtitle: docId,
        source: 'content',
      }))
      setContentState({ snapshot, query, results: mapped })
    }, BM25_DEBOUNCE_MS)
    return () => clearTimeout(timer)
  }, [snapshot, bm25, query, fileLabels])

  const nameResults = useMemo(() => (snapshot ? searchNames(nameIndex, query) : []), [snapshot, nameIndex, query])
  const contentResults = useMemo(
    () =>
      snapshot && bm25 && query.trim().length >= 2 && contentState.snapshot === snapshot && contentState.query === query
        ? contentState.results
        : [],
    [snapshot, bm25, query, contentState],
  )
  const results = useMemo(() => mergeResults(nameResults, contentResults), [nameResults, contentResults])

  return {
    query,
    setQuery,
    results,
    status,
    skipped,
    reset,
  }
}
