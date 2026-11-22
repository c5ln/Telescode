// @vitest-environment jsdom

import { act, cleanup, renderHook } from '@testing-library/react'
import { afterEach, describe, expect, it, vi } from 'vitest'

import type { AnalysisSnapshot, GraphResponse } from '../bridge'
import sherlock from '../graph/fixtures/sherlock.graph.json'
import { orderedFileIds, useSearch, type SourcesLoader, type SourcesResult } from './useSearch'

afterEach(() => {
  cleanup()
  vi.restoreAllMocks()
})

const baseGraph = sherlock as GraphResponse

function makeSnapshot(over: Partial<AnalysisSnapshot> = {}): AnalysisSnapshot {
  return {
    ...baseGraph,
    dbPath: 'C:/x/sherlock.db',
    readingSequence: [],
    ...over,
  }
}

describe('orderedFileIds', () => {
  it('orders readingSequence file rows by fileRank ascending and appends unranked files after', () => {
    const files = baseGraph.files.slice(0, 4)
    const snap = makeSnapshot({
      files,
      readingSequence: [
        {
          entityId: files[2].fileId,
          entityType: 'file',
          fileId: files[2].fileId,
          fileRank: 2,
          localRank: null,
          scores: { pagerank: null, betweenness: null, combined: null },
        },
        {
          entityId: `${files[1].fileId}::Cls`,
          entityType: 'class',
          fileId: files[1].fileId,
          fileRank: null,
          localRank: 1,
          scores: { pagerank: null, betweenness: null, combined: null },
        },
        {
          entityId: files[0].fileId,
          entityType: 'file',
          fileId: files[0].fileId,
          fileRank: 1,
          localRank: null,
          scores: { pagerank: null, betweenness: null, combined: null },
        },
      ],
    })

    expect(orderedFileIds(snap)).toEqual([
      files[0].fileId,
      files[2].fileId,
      files[1].fileId,
      files[3].fileId,
    ])
  })
})

describe('useSearch (BM25 loading, chunking, debounce, cancellation)', () => {
  it('keeps name search working while indexing, then merges debounced BM25 results once ready', async () => {
    let resolveSources!: (v: SourcesResult) => void
    const loader = vi.fn<SourcesLoader>(
      () => new Promise<SourcesResult>((resolve) => (resolveSources = resolve)),
    )
    const snap = makeSnapshot()
    const { result } = renderHook(() => useSearch(snap, snap.dbPath, loader))

    // Wait for initial yield so loadSources is invoked.
    await vi.waitFor(() => expect(loader).toHaveBeenCalledTimes(1))
    expect(result.current.status).toBe('indexing')

    // Name search works immediately even before BM25 finishes.
    act(() => result.current.setQuery('QueryNotify'))
    expect(result.current.results.some((r) => r.id === 'class:sherlock_project/notify.py::QueryNotify')).toBe(true)
    expect(result.current.results.some((r) => r.source === 'content')).toBe(false)

    // Resolve sources with a content-only keyword inside sites.py and skipped=3.
    await act(async () => {
      resolveSources({
        sources: {
          'sherlock_project/sites.py': 'def verify_jwt_signature(payload):\n    return payload\n',
        },
        skipped: 3,
      })
    })
    await vi.waitFor(() => expect(result.current.status).toBe('ready'))
    expect(result.current.skipped).toBe(3)

    // 1-char query does not trigger BM25 content matches.
    act(() => result.current.setQuery('v'))
    await new Promise((r) => setTimeout(r, 140))
    expect(result.current.results.some((r) => r.source === 'content')).toBe(false)

    // 2+ char query returns content match after 100ms debounce.
    act(() => result.current.setQuery('verify_jwt'))
    expect(result.current.results.some((r) => r.source === 'content')).toBe(false)

    await vi.waitFor(
      () =>
        expect(
          result.current.results.some(
            (r) => r.id === 'file:sherlock_project/sites.py' && r.source === 'content',
          ),
        ).toBe(true),
      { timeout: 500 },
    )
  })

  it('falls back to path-only BM25 without throwing when source loading fails', async () => {
    const loader = vi.fn<SourcesLoader>(() => Promise.reject(new Error('ipc failed')))
    const snap = makeSnapshot()
    const { result } = renderHook(() => useSearch(snap, snap.dbPath, loader))

    await vi.waitFor(() => expect(result.current.status).toBe('ready'))
    expect(result.current.skipped).toBe(0)

    // "sherlock_project" matches file paths via tokenizePath even with empty sources.
    act(() => result.current.setQuery('sherlock_project'))
    await vi.waitFor(
      () => expect(result.current.results.some((r) => r.source === 'content')).toBe(true),
      { timeout: 500 },
    )
  })

  it('ignores late source responses from a superseded snapshot', async () => {
    const pending: Array<(v: SourcesResult) => void> = []
    const loader = vi.fn<SourcesLoader>(
      () => new Promise<SourcesResult>((resolve) => pending.push(resolve)),
    )
    const snap1 = makeSnapshot({ dbPath: 'C:/x/one.db' })
    const snap2 = makeSnapshot({ dbPath: 'C:/x/two.db' })

    const { result, rerender } = renderHook(
      ({ snap }) => useSearch(snap, snap.dbPath, loader),
      { initialProps: { snap: snap1 } },
    )
    await vi.waitFor(() => expect(pending).toHaveLength(1))

    // Switch snapshot before snap1 finishes loading.
    rerender({ snap: snap2 })
    await vi.waitFor(() => expect(pending).toHaveLength(2))

    // Resolve stale snap1 response with skipped=99; must be ignored.
    await act(async () => {
      pending[0]!({
        sources: { 'sherlock_project/sites.py': 'obsolete_alpha_marker' },
        skipped: 99,
      })
      await new Promise((r) => setTimeout(r, 20))
    })
    expect(result.current.status).toBe('indexing')
    expect(result.current.skipped).toBe(0)

    // Resolve current snap2 response.
    await act(async () => {
      pending[1]!({
        sources: { 'sherlock_project/result.py': 'current_omega_signal' },
        skipped: 1,
      })
    })
    await vi.waitFor(() => expect(result.current.status).toBe('ready'))
    expect(result.current.skipped).toBe(1)

    act(() => result.current.setQuery('obsolete_alpha'))
    await new Promise((r) => setTimeout(r, 140))
    expect(result.current.results.some((r) => r.source === 'content')).toBe(false)

    act(() => result.current.setQuery('current_omega'))
    await vi.waitFor(
      () =>
        expect(
          result.current.results.some(
            (r) => r.id === 'file:sherlock_project/result.py' && r.source === 'content',
          ),
        ).toBe(true),
      { timeout: 500 },
    )
  })

  it('yields between ~12ms chunks while indexing and completes all documents', async () => {
    let fakeNow = 0
    vi.spyOn(performance, 'now').mockImplementation(() => {
      fakeNow += 15
      return fakeNow
    })
    const loader = vi.fn<SourcesLoader>(async () => ({
      sources: {
        'sherlock_project/notify.py': 'chunked_alpha_marker',
        'sherlock_project/sites.py': 'chunked_omega_marker',
      },
      skipped: 0,
    }))
    const snap = makeSnapshot()
    const { result } = renderHook(() => useSearch(snap, snap.dbPath, loader))

    await vi.waitFor(() => expect(result.current.status).toBe('ready'))

    act(() => result.current.setQuery('chunked_omega_marker'))
    await vi.waitFor(
      () =>
        expect(
          result.current.results.some(
            (r) => r.id === 'file:sherlock_project/sites.py' && r.source === 'content',
          ),
        ).toBe(true),
      { timeout: 500 },
    )
  })

  it('recovers to idle without staying stuck in indexing if an unexpected error occurs during build', async () => {
    const hostileSources = new Proxy<Record<string, string>>(
      {},
      {
        get() {
          throw new Error('unexpected indexing boom')
        },
      },
    )
    const loader = vi.fn<SourcesLoader>(async () => ({ sources: hostileSources, skipped: 0 }))
    const snap = makeSnapshot()
    const { result } = renderHook(() => useSearch(snap, snap.dbPath, loader))

    await vi.waitFor(() => expect(result.current.status).toBe('idle'))

    // Name search continues working normally after fallback.
    act(() => result.current.setQuery('QueryNotify'))
    expect(result.current.results.some((r) => r.id === 'class:sherlock_project/notify.py::QueryNotify')).toBe(true)
  })
})
