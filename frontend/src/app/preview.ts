// Development-only: open the shell directly in a given state with mock data,
// e.g. http://localhost:5173/?preview=ready. The browser cannot reach the
// core, so this is the only way to see the ready state there. main.tsx
// reads it only under import.meta.env.DEV, so it is dropped from builds.

import { TelescodeError, type AnalysisSnapshot } from '../bridge'
import type { Crumb } from '../ui/Breadcrumbs'
import type { WorkspaceState } from './useWorkspace'

export interface ShellPreview {
  initialState: WorkspaceState
  initialPath: Crumb[]
}

const DB_PATH = 'C:/work/telescode/telescode.db'

const MOCK_SNAPSHOT: AnalysisSnapshot = {
  schemaVersion: 1,
  dbPath: DB_PATH,
  totals: {
    fileCount: 16,
    classCount: 11,
    classEdgeCount: 1,
    fileNodeCount: 16,
    fileEdgeCount: 18,
    funcNodeCount: 198,
    funcEdgeCount: 219,
    sequenceCount: 74,
  },
  files: [],
  fileGraph: { nodes: [], edges: [] },
  classes: [],
  classEdges: [],
  readingSequence: [],
}

const crumbs = (...labels: string[]): Crumb[] => labels.map((label, i) => ({ id: `mock-${i}`, label }))

export function readPreview(search: string): ShellPreview | null {
  const name = new URLSearchParams(search).get('preview')
  switch (name) {
    case 'empty':
      return { initialState: { status: 'empty' }, initialPath: [] }
    case 'loading':
      return { initialState: { status: 'loading', dbPath: DB_PATH }, initialPath: [] }
    case 'error':
      return {
        initialState: {
          status: 'error',
          dbPath: DB_PATH,
          error: new TelescodeError('db_not_found', `No database at ${DB_PATH}. Nothing was created.`),
        },
        initialPath: [],
      }
    case 'ready':
      return {
        initialState: { status: 'ready', dbPath: DB_PATH, snapshot: MOCK_SNAPSHOT },
        initialPath: crumbs('src', 'core', 'dependency.cpp'),
      }
    case 'deep':
      return {
        initialState: { status: 'ready', dbPath: DB_PATH, snapshot: MOCK_SNAPSHOT },
        initialPath: crumbs(
          'src',
          'core',
          'analysis',
          'graph',
          'algorithms',
          'reading_sequence_with_a_deliberately_long_name.cpp',
        ),
      }
    default:
      return null
  }
}
