// Tutorial mode: a saved code tour, played on the live map.
//
// Tours are written by an agent through the Telescode MCP server (mcp/), which
// saves each one beside the snapshot it was written against. The desktop shell
// lists them and has new ones written through that server
// (src-tauri/src/tours.rs); this module picks the one that belongs to the open
// repository.

import { invoke, isTauri } from '@tauri-apps/api/core'
import { agentStatus, signIn, type AgentProvider, type AgentStatus } from '../agent'
export type { AgentStatus } from '../agent'

import type { AnalysisSnapshot } from '../bridge'
import { TelescodeError, fromNativeError } from '../bridge/errors'
import type { TourPlan, TourStop } from './model'

export interface SavedTour {
  plan: TourPlan
  /** SHA-256 of the snapshot the tour was written against. */
  snapshotHash: string
  /** Baseline analysis hash before query-specific ranking was applied. */
  sourceSnapshotHash?: string
  state: string
  updatedMs: number
}

export interface TourLibrary {
  list(): Promise<SavedTour[]>
  /** Live desktop onboarding regenerates tours made before model integration. */
  requireRankedOrder?: boolean
  /** Have a tour written for the repository analyzed in `dbPath`; resolves once it is saved. */
  generate?(dbPath: string): Promise<void>
  /** Whether the agent is installed and signed in. Asked only when a tour must be written. */
  agentStatus?(): Promise<AgentStatus>
  /** Start the agent's sign-in in the browser; resolves once signed in. */
  signIn?(): Promise<void>
}

async function invokeTours<T>(command: string, args?: Record<string, unknown>): Promise<T> {
  if (!isTauri()) {
    throw new TelescodeError('bridge_unavailable', 'Tutorials are only reachable from the desktop app.')
  }
  try {
    return await invoke<T>(command, args)
  } catch (e) {
    throw fromNativeError(e)
  }
}

/** Generations under way, by provider and database: asking again joins the one already running. */
const generating = new Map<string, Promise<void>>()

/**
 * Tours kept by the Telescode MCP server, through the desktop shell: it lists
 * them with the server's tools, and has new ones written by the selected CLI agent with
 * the server attached.
 */
export function createTourLibrary(provider: AgentProvider): TourLibrary {
  return {
    requireRankedOrder: true,
    list: () => invokeTours<SavedTour[]>('list_tours'),
    agentStatus: () => agentStatus(provider),
    signIn: () => signIn(provider),
    generate(dbPath) {
      const key = `${provider}\n${dbPath}`
      let run = generating.get(key)
      if (!run) {
        run = invokeTours<void>('generate_tour', { dbPath, provider }).finally(() => generating.delete(key))
        generating.set(key, run)
      }
      return run
    },
  }
}
export const tauriTourLibrary = createTourLibrary('claude')

/** The step's node, a caption and at least the fields the player shows. */
function isStop(s: unknown): s is TourStop {
  const stop = s as TourStop | null
  return (
    typeof stop?.nodeId === 'string' &&
    typeof stop.caption === 'string' &&
    Number.isFinite(stop.transitionMs) &&
    Number.isFinite(stop.holdMs)
  )
}

/**
 * The tour to play for a repository: one whose every stop is on the current
 * map, preferring one written against this exact snapshot, then the newest.
 * A tour of the repository root alone says nothing about which repository it
 * was for, so it never matches by node alone.
 */
export function pickTutorial(
  tours: readonly SavedTour[],
  hasNode: (id: string) => boolean,
  snapshotHash: string | null,
  requireRankedOrder = false,
): SavedTour | null {
  const matches = (t: SavedTour) => snapshotHash !== null &&
    (t.snapshotHash === snapshotHash || t.sourceSnapshotHash === snapshotHash)
  const playable = tours.filter((t) => {
    if (requireRankedOrder && !t.sourceSnapshotHash) return false
    const stops: unknown = t.plan?.stops
    if (!Array.isArray(stops) || stops.length === 0 || !stops.every(isStop)) return false
    if (!stops.every((s) => hasNode(s.nodeId))) return false
    if (t.sourceSnapshotHash) return matches(t)
    return matches(t) || stops.some((s) => s.nodeId !== 'root')
  })
  const exact = (t: SavedTour) => (matches(t) ? 1 : 0)
  return playable.sort((a, b) => exact(b) - exact(a) || b.updatedMs - a.updatedMs)[0] ?? null
}

/** The hash the MCP server records for a snapshot: SHA-256 of its JSON. */
export async function snapshotHash(snapshot: AnalysisSnapshot): Promise<string | null> {
  if (!globalThis.crypto?.subtle) return null
  const digest = await crypto.subtle.digest('SHA-256', new TextEncoder().encode(JSON.stringify(snapshot)))
  return [...new Uint8Array(digest)].map((b) => b.toString(16).padStart(2, '0')).join('')
}

/** Playback runs a little quicker than the tour's video timing: there, the viewer cannot pause. */
export const PLAYBACK_SPEED = 1.25

/** How long a step stays on screen during playback. */
export function stepDurationMs(stop: Pick<TourStop, 'transitionMs' | 'holdMs'>): number {
  return Math.round((stop.transitionMs + stop.holdMs) / PLAYBACK_SPEED)
}
