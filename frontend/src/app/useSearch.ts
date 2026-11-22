// Unified search state for the open repository.
//
// Builds the graph node index without running layout (D7) so node ids match
// CodeMap 1:1. Query state is tied to the current snapshot and resets whenever
// the repository changes, reloads, or closes.

import { useCallback, useMemo, useState } from 'react'

import type { AnalysisSnapshot } from '../bridge'
import { buildGraphModel } from '../graph/model'
import { buildNameIndex, searchNames } from '../search/names'
import type { SearchResult } from '../search/search'

export type SearchIndexStatus = 'idle' | 'indexing' | 'ready'

export interface SearchState {
  query: string
  setQuery(q: string): void
  results: SearchResult[]
  status: SearchIndexStatus
  skipped: number
  reset(): void
}

export function useSearch(snapshot: AnalysisSnapshot | null, _dbPath: string | null): SearchState {
  const [owned, setOwned] = useState<{ snapshot: AnalysisSnapshot | null; query: string }>({
    snapshot,
    query: '',
  })
  const query = owned.snapshot === snapshot ? owned.query : ''
  const setQuery = useCallback((next: string) => setOwned({ snapshot, query: next }), [snapshot])
  const reset = useCallback(() => setOwned({ snapshot, query: '' }), [snapshot])

  const model = useMemo(() => (snapshot ? buildGraphModel(snapshot, '') : null), [snapshot])
  const nameIndex = useMemo(() => (model ? buildNameIndex(model) : []), [model])
  const results = useMemo(() => (snapshot ? searchNames(nameIndex, query) : []), [snapshot, nameIndex, query])

  return {
    query,
    setQuery,
    results,
    status: 'idle',
    skipped: 0,
    reset,
  }
}
