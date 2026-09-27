import { afterEach, describe, expect, it, vi } from 'vitest'

import sherlock from '../graph/fixtures/sherlock.graph.json'
import { readPreview } from './preview'

afterEach(() => {
  vi.unstubAllGlobals()
})

const respond = (response: Response | Promise<Response>) => vi.stubGlobal('fetch', vi.fn(() => Promise.resolve(response)))

describe('readPreview ?snapshot=', () => {
  it('opens a valid snapshot in the ready state', async () => {
    respond(new Response(JSON.stringify(sherlock), { status: 200 }))
    const preview = await readPreview('?snapshot=/snap.json')
    expect(preview?.initialState.status).toBe('ready')
    if (preview?.initialState.status !== 'ready') return
    expect(preview.initialState.dbPath).toBe('sherlock.db')
    expect(preview.initialState.snapshot.files).toHaveLength(16)
    expect(preview.initialState.snapshot.readingSequence).toEqual([])
  })

  it('shows the error state for an HTTP error instead of rejecting', async () => {
    respond(new Response('<!doctype html>not found', { status: 404, statusText: 'Not Found' }))
    const preview = await readPreview('?snapshot=/missing.json')
    expect(preview?.initialState.status).toBe('error')
    if (preview?.initialState.status !== 'error') return
    expect(preview.initialState.dbPath).toBe('/missing.json')
    expect(preview.initialState.error.message).toContain('HTTP 404')
  })

  it('shows the error state for invalid JSON', async () => {
    respond(new Response('{ not json', { status: 200 }))
    const preview = await readPreview('?snapshot=/bad.json')
    expect(preview?.initialState.status).toBe('error')
  })

  it('shows the error state when the fetch itself fails', async () => {
    vi.stubGlobal('fetch', vi.fn(() => Promise.reject(new TypeError('Failed to fetch'))))
    const preview = await readPreview('?snapshot=/down.json')
    expect(preview?.initialState.status).toBe('error')
    if (preview?.initialState.status !== 'error') return
    expect(preview.initialState.error.message).toContain('Failed to fetch')
  })
})
