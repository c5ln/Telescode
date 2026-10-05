// The frontend's only door to the C++ core.
//
// Each call is one complete, coarse-grained operation: the desktop shell runs
// TelescodeHeadless once and hands back the whole result. Components should
// request data at load or refresh time and keep it in state, never call this
// during rendering.

import { invoke, isTauri } from '@tauri-apps/api/core'

import { TelescodeError, fromNativeError } from './errors'
import type { AnalysisResponse, GraphResponse, SequenceResponse } from './models'
import { parseAnalysis, parseGraph, parseSequence, type Operation } from './parse'

export interface TelescodeApi {
  graph(dbPath: string): Promise<GraphResponse>
  sequence(dbPath: string): Promise<SequenceResponse>
  analyze(dbPath: string): Promise<AnalysisResponse>
}

/** Runs one headless operation and resolves with the core's raw stdout. */
export type HeadlessRunner = (op: Operation, dbPath: string) => Promise<string>

/** Builds the API over any runner, so tests can substitute the transport. */
export function createTelescodeApi(runner: HeadlessRunner): TelescodeApi {
  const call = async <T>(op: Operation, dbPath: string, parse: (s: string) => T): Promise<T> => {
    let stdout: string
    try {
      stdout = await runner(op, dbPath)
    } catch (e) {
      throw fromNativeError(e)
    }
    return parse(stdout)
  }

  return {
    graph: (dbPath) => call('graph', dbPath, parseGraph),
    sequence: (dbPath) => call('sequence', dbPath, parseSequence),
    analyze: (dbPath) => call('analyze', dbPath, parseAnalysis),
  }
}

/** The transport used inside the desktop app: the `run_headless` Tauri command. */
export const tauriRunner: HeadlessRunner = async (op, dbPath) => {
  if (!isTauri()) {
    throw new TelescodeError(
      'bridge_unavailable',
      'The C++ core is only reachable from the desktop app. Start it with `npm run desktop`.',
    )
  }
  return invoke<string>('run_headless', { op, dbPath })
}

export const telescode: TelescodeApi = createTelescodeApi(tauriRunner)

/** Where a repository comes from: a public GitHub URL, or a folder on this computer. */
export type RepositorySource = { kind: 'github'; url: string } | { kind: 'folder'; path: string }

/**
 * Scans a repository into a database (downloading it first if it is on
 * GitHub), resolving with the path of the database to analyze. One coarse
 * call, like the others.
 */
export type RepositoryOpener = (source: RepositorySource) => Promise<string>

/** The opener used inside the desktop app: the `open_repository` and `open_folder` Tauri commands. */
export const tauriRepositoryOpener: RepositoryOpener = async (source) => {
  if (!isTauri()) {
    throw new TelescodeError(
      'bridge_unavailable',
      'Repositories can only be opened from the desktop app. Start it with `npm run desktop`.',
    )
  }
  try {
    return source.kind === 'github'
      ? await invoke<string>('open_repository', { url: source.url })
      : await invoke<string>('open_folder', { path: source.path })
  } catch (e) {
    throw fromNativeError(e)
  }
}
