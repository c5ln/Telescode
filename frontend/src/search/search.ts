// Shared search result types and merging of name + BM25 content results (D1).

export type ResultKind = 'file' | 'class' | 'method' | 'field'

export interface SearchResult {
  id: string
  kind: ResultKind
  label: string
  subtitle: string
  source: 'name' | 'content'
}

const NAME_LIMIT = 15
const CONTENT_LIMIT = 10

/**
 * Places name results (up to 15) ahead of BM25 content results (up to 10).
 * Drops a BM25 `file:<fileId>` result only when that exact file node already
 * appears directly in `names`; class/member hits inside the same file keep the
 * file-level BM25 hit.
 */
export function mergeResults(names: readonly SearchResult[], content: readonly SearchResult[]): SearchResult[] {
  const cappedNames = names.slice(0, NAME_LIMIT)
  const directFileIds = new Set<string>()
  for (const item of cappedNames) {
    if (item.kind === 'file') {
      directFileIds.add(item.id)
    }
  }

  const filteredContent: SearchResult[] = []
  for (const item of content) {
    if (item.kind === 'file' && directFileIds.has(item.id)) continue
    filteredContent.push(item)
    if (filteredContent.length >= CONTENT_LIMIT) break
  }

  return [...cappedNames, ...filteredContent]
}
