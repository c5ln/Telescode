import { describe, expect, it } from 'vitest'

import { createTelescodeApi, type HeadlessRunner } from './api'
import { TelescodeError } from './errors'

const totals = {
  fileCount: 16,
  classCount: 11,
  classEdgeCount: 1,
  fileNodeCount: 16,
  fileEdgeCount: 18,
  funcNodeCount: 198,
  funcEdgeCount: 219,
  sequenceCount: 74,
}

const snapshot = {
  schemaVersion: 1,
  dbPath: 'C:/x/s.db',
  totals,
  files: [],
  fileGraph: { nodes: [], edges: [] },
  classes: [],
  classEdges: [],
  readingSequence: [],
}

const returning = (stdout: string): HeadlessRunner => async () => stdout
const rejecting = (err: unknown): HeadlessRunner => async () => {
  throw err
}

async function codeOf(p: Promise<unknown>): Promise<string> {
  try {
    await p
  } catch (e) {
    expect(e).toBeInstanceOf(TelescodeError)
    return (e as TelescodeError).code
  }
  throw new Error('expected a rejection')
}

describe('telescode bridge', () => {
  it('passes the operation and path to the runner unchanged', async () => {
    const calls: [string, string][] = []
    const api = createTelescodeApi(async (op, db) => {
      calls.push([op, db])
      return JSON.stringify(snapshot)
    })
    await api.analyze('C:/x/s.db')
    await api.graph('C:/x/s.db')
    await api.sequence('C:/x/s.db')
    expect(calls).toEqual([
      ['analyze', 'C:/x/s.db'],
      ['graph', 'C:/x/s.db'],
      ['sequence', 'C:/x/s.db'],
    ])
  })

  it('returns the core JSON without altering it', async () => {
    const api = createTelescodeApi(returning(JSON.stringify(snapshot) + '\n'))
    expect(await api.analyze('db')).toEqual(snapshot)
  })

  it('accepts each slice with only its own keys', async () => {
    const { readingSequence: _rs, ...graph } = snapshot
    const sequence = { schemaVersion: 1, dbPath: 'db', totals, readingSequence: [] }
    await expect(createTelescodeApi(returning(JSON.stringify(graph))).graph('db')).resolves.toBeTruthy()
    await expect(createTelescodeApi(returning(JSON.stringify(sequence))).sequence('db')).resolves.toBeTruthy()
    // A sequence payload is not a full analysis.
    expect(await codeOf(createTelescodeApi(returning(JSON.stringify(sequence))).analyze('db'))).toBe(
      'unexpected_schema',
    )
  })

  it('reports malformed JSON', async () => {
    expect(await codeOf(createTelescodeApi(returning('{"schemaVersion":1,')).analyze('db'))).toBe(
      'malformed_json',
    )
    expect(await codeOf(createTelescodeApi(returning('')).analyze('db'))).toBe('malformed_json')
  })

  it('reports JSON of the wrong shape', async () => {
    const bad = [
      '[]',
      '{}',
      JSON.stringify({ ...snapshot, totals: { ...totals, fileCount: '16' } }),
      JSON.stringify({ ...snapshot, fileGraph: [] }),
      JSON.stringify({ ...snapshot, classes: null }),
    ]
    for (const s of bad) {
      expect(await codeOf(createTelescodeApi(returning(s)).analyze('db'))).toBe('unexpected_schema')
    }
  })

  it('reports an unsupported schema version', async () => {
    const s = JSON.stringify({ ...snapshot, schemaVersion: 2 })
    expect(await codeOf(createTelescodeApi(returning(s)).analyze('db'))).toBe('unsupported_schema_version')
  })

  it('keeps structured native errors', async () => {
    const api = createTelescodeApi(
      rejecting({ code: 'sidecar_failed', message: 'boom', exitCode: 1, stderr: 'boom' }),
    )
    const err = await api.analyze('db').catch((e: unknown) => e)
    expect(err).toBeInstanceOf(TelescodeError)
    expect(err).toMatchObject({ code: 'sidecar_failed', message: 'boom', details: { exitCode: 1, stderr: 'boom' } })
  })

  it.each(['db_not_found', 'db_invalid', 'sidecar_missing'])('maps %s', async (code) => {
    const api = createTelescodeApi(rejecting({ code, message: code, path: 'p' }))
    expect(await codeOf(api.analyze('db'))).toBe(code)
  })

  it('wraps unrecognised rejections', async () => {
    expect(await codeOf(createTelescodeApi(rejecting('plain string')).analyze('db'))).toBe('unknown')
    expect(await codeOf(createTelescodeApi(rejecting({ code: 'nope' })).analyze('db'))).toBe('unknown')
  })
})
