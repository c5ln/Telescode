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
//   &tour=<url> | none
//     The saved tours tutorial mode finds: a tour plan, or an MCP tour spec
//     such as mcp/tours/sherlock-onboarding.json, fetched from the dev server.
//     `none` has no tours, to show how a tutorial is made.

import { TelescodeError, type AnalysisSnapshot, type GraphResponse } from '../bridge'
import { inspectNode, type StopInput, type TourPlan } from '../tour/model'
import type { TourLibrary } from '../tour/tutorial'
import type { WorkspaceState } from './useWorkspace'

export interface ShellPreview {
  initialState: WorkspaceState
  tours?: TourLibrary
}

const DB_PATH = 'C:/work/sherlock.db'

function asSnapshot(graph: GraphResponse): AnalysisSnapshot {
  return { readingSequence: [], ...graph } as AnalysisSnapshot
}

/** Tours from `?tour=`, with titles and evidence filled in from the snapshot as the MCP server would. */
function previewTours(tourUrl: string | null, initialState: WorkspaceState): TourLibrary | undefined {
  if (!tourUrl) return undefined
  return {
    async list() {
      if (tourUrl === 'none' || initialState.status !== 'ready') return []
      const response = await fetch(tourUrl)
      if (!response.ok) throw new Error(`HTTP ${response.status} ${response.statusText}`.trim())
      const spec = (await response.json()) as Partial<TourPlan> & { stops: StopInput[] }
      const snapshot = initialState.snapshot
      const stops = spec.stops.map((s) => {
        const { title, evidence } = inspectNode(snapshot, s.nodeId)
        return { ...s, title, evidence }
      })
      const plan: TourPlan = { schemaVersion: 1, id: tourUrl, revision: 1, title: spec.title ?? 'Tour', language: spec.language ?? 'en', snapshotHash: '', stops }
      return [{ plan, snapshotHash: '', state: 'draft', updatedMs: 0 }]
    },
  }
}

export async function readPreview(search: string): Promise<ShellPreview | null> {
  const preview = await readState(search)
  return preview && { ...preview, tours: previewTours(new URLSearchParams(search).get('tour'), preview.initialState) }
}

async function readState(search: string): Promise<ShellPreview | null> {
  const params = new URLSearchParams(search)
  const snapshotUrl = params.get('snapshot')
  if (snapshotUrl) {
    // A failure here must not reject: main.tsx awaits this before rendering.
    try {
      const response = await fetch(snapshotUrl)
      if (!response.ok) throw new Error(`HTTP ${response.status} ${response.statusText}`.trim())
      const graph = (await response.json()) as GraphResponse
      return { initialState: { status: 'ready', dbPath: graph.dbPath || snapshotUrl, snapshot: asSnapshot(graph) } }
    } catch (e) {
      const reason = e instanceof Error ? e.message : String(e)
      return {
        initialState: {
          status: 'error',
          dbPath: snapshotUrl,
          error: new TelescodeError('unknown', `Could not load the snapshot at ${snapshotUrl}: ${reason}`),
        },
      }
    }
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
