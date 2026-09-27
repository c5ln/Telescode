// Development-only: open the shell directly in a given state, since the
// browser cannot reach the core. main.tsx calls this only under
// import.meta.env.DEV, so none of it ships in builds.
//
//   ?preview=empty | loading | error | ready
//     `ready` shows real core output for the Sherlock sample repository
//     (src/graph/fixtures/sherlock.graph.json).
//   ?snapshot=<url>
//     Ready state from any JSON the core printed (`TelescodeHeadless graph`
//     or `analyze`), fetched from the dev server, e.g. a file placed under
//     node_modules/.cache/ for a quick look at a large repository.

import { TelescodeError, type AnalysisSnapshot, type GraphResponse } from '../bridge'
import type { WorkspaceState } from './useWorkspace'

export interface ShellPreview {
  initialState: WorkspaceState
}

const DB_PATH = 'C:/work/sherlock.db'

function asSnapshot(graph: GraphResponse): AnalysisSnapshot {
  return { readingSequence: [], ...graph } as AnalysisSnapshot
}

export async function readPreview(search: string): Promise<ShellPreview | null> {
  const params = new URLSearchParams(search)
  const snapshotUrl = params.get('snapshot')
  if (snapshotUrl) {
    const graph = (await (await fetch(snapshotUrl)).json()) as GraphResponse
    return { initialState: { status: 'ready', dbPath: graph.dbPath || snapshotUrl, snapshot: asSnapshot(graph) } }
  }

  switch (params.get('preview')) {
    case 'empty':
      return { initialState: { status: 'empty' } }
    case 'loading':
      return { initialState: { status: 'loading', dbPath: DB_PATH } }
    case 'error':
      return {
        initialState: {
          status: 'error',
          dbPath: DB_PATH,
          error: new TelescodeError('db_not_found', `No database at ${DB_PATH}. Nothing was created.`),
        },
      }
    case 'ready': {
      const { default: graph } = await import('../graph/fixtures/sherlock.graph.json')
      return { initialState: { status: 'ready', dbPath: DB_PATH, snapshot: asSnapshot(graph as GraphResponse) } }
    }
    default:
      return null
  }
}
