import { describe, expect, it } from 'vitest'

import { mergeResults, type SearchResult } from './search'

const fileResult = (fileId: string, source: 'name' | 'content'): SearchResult => ({
  id: `file:${fileId}`,
  kind: 'file',
  label: fileId.split('/').pop() ?? fileId,
  subtitle: fileId,
  source,
})

const classResult = (fileId: string, className: string): SearchResult => ({
  id: `class:${fileId}::${className}`,
  kind: 'class',
  label: className,
  subtitle: fileId,
  source: 'name',
})

const methodResult = (fileId: string, className: string, methodName: string): SearchResult => ({
  id: `member:${fileId}::${className}::${methodName}`,
  kind: 'method',
  label: methodName,
  subtitle: `${className} · ${fileId}`,
  source: 'name',
})

describe('mergeResults', () => {
  it('places name results first and content results after, capping at 15 and 10', () => {
    const names = Array.from({ length: 18 }, (_, i) => classResult('src/a.py', `Class${i}`))
    const content = Array.from({ length: 14 }, (_, i) => fileResult(`src/doc_${i}.py`, 'content'))

    const merged = mergeResults(names, content)
    expect(merged).toHaveLength(25)
    expect(merged.slice(0, 15)).toEqual(names.slice(0, 15))
    expect(merged.slice(15)).toEqual(content.slice(0, 10))
  })

  it('drops a BM25 file result when the same file:<fileId> is directly in name results', () => {
    const names = [fileResult('src/notify.py', 'name'), classResult('src/result.py', 'QueryResult')]
    const content = [fileResult('src/notify.py', 'content'), fileResult('src/other.py', 'content')]

    const merged = mergeResults(names, content)
    expect(merged.map((r) => `${r.source}:${r.id}`)).toEqual([
      'name:file:src/notify.py',
      'name:class:src/result.py::QueryResult',
      'content:file:src/other.py',
    ])
  })

  it('keeps a BM25 file result when name results only contain a class or member from that file', () => {
    const names = [
      classResult('src/notify.py', 'QueryNotify'),
      methodResult('src/notify.py', 'QueryNotify', 'update'),
    ]
    const content = [fileResult('src/notify.py', 'content')]

    const merged = mergeResults(names, content)
    expect(merged.map((r) => `${r.source}:${r.id}`)).toEqual([
      'name:class:src/notify.py::QueryNotify',
      'name:member:src/notify.py::QueryNotify::update',
      'content:file:src/notify.py',
    ])
  })
})
