// Turns the core's stdout into typed responses.
//
// The checks are structural and stop at the top level: they confirm the
// payload is the shape the models describe without walking every element, so
// the cost stays flat as repositories grow. Values are never altered.

import { TelescodeError } from './errors'
import {
  SUPPORTED_SCHEMA_VERSION,
  type AnalysisSnapshot,
  type AnalysisTotals,
  type GraphResponse,
  type SequenceResponse,
} from './models'

export type Operation = 'graph' | 'sequence' | 'analyze'

const TOTAL_KEYS: readonly (keyof AnalysisTotals)[] = [
  'fileCount',
  'classCount',
  'classEdgeCount',
  'fileNodeCount',
  'fileEdgeCount',
  'funcNodeCount',
  'funcEdgeCount',
  'sequenceCount',
]

const ARRAY_KEYS: Record<Operation, readonly string[]> = {
  graph: ['files', 'classes', 'classEdges'],
  sequence: ['readingSequence'],
  analyze: ['files', 'classes', 'classEdges', 'readingSequence'],
}

function isObject(v: unknown): v is Record<string, unknown> {
  return typeof v === 'object' && v !== null && !Array.isArray(v)
}

function unexpected(op: Operation, what: string): TelescodeError {
  return new TelescodeError('unexpected_schema', `Unexpected ${op} response: ${what}`)
}

function parse(op: Operation, stdout: string): Record<string, unknown> {
  let value: unknown
  try {
    value = JSON.parse(stdout)
  } catch (e) {
    const reason = e instanceof Error ? e.message : String(e)
    throw new TelescodeError('malformed_json', `TelescodeHeadless returned invalid JSON: ${reason}`)
  }

  if (!isObject(value)) throw unexpected(op, 'top level is not an object')

  if (typeof value.schemaVersion !== 'number') throw unexpected(op, 'missing schemaVersion')
  if (value.schemaVersion !== SUPPORTED_SCHEMA_VERSION) {
    throw new TelescodeError(
      'unsupported_schema_version',
      `Unsupported schemaVersion ${value.schemaVersion} (expected ${SUPPORTED_SCHEMA_VERSION})`,
    )
  }
  if (typeof value.dbPath !== 'string') throw unexpected(op, 'missing dbPath')

  const totals = value.totals
  if (!isObject(totals)) throw unexpected(op, 'missing totals')
  for (const k of TOTAL_KEYS) {
    if (typeof totals[k] !== 'number') throw unexpected(op, `totals.${k} is not a number`)
  }

  for (const k of ARRAY_KEYS[op]) {
    if (!Array.isArray(value[k])) throw unexpected(op, `${k} is not an array`)
  }
  if (op !== 'sequence') {
    const g = value.fileGraph
    if (!isObject(g) || !Array.isArray(g.nodes) || !Array.isArray(g.edges)) {
      throw unexpected(op, 'fileGraph is not { nodes: [], edges: [] }')
    }
  }
  return value
}

export function parseGraph(stdout: string): GraphResponse {
  return parse('graph', stdout) as unknown as GraphResponse
}

export function parseSequence(stdout: string): SequenceResponse {
  return parse('sequence', stdout) as unknown as SequenceResponse
}

export function parseAnalysis(stdout: string): AnalysisSnapshot {
  return parse('analyze', stdout) as unknown as AnalysisSnapshot
}
