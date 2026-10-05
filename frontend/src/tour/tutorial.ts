// Tutorial mode: a saved code tour, played on the live map.
//
// Tours are written by an agent through the Telescode MCP server (mcp/), which
// saves each one beside the snapshot it was written against. The desktop shell
// lists them (src-tauri/src/tours.rs); this module picks the one that belongs
// to the open repository.

import { invoke, isTauri } from '@tauri-apps/api/core'

import type { AnalysisSnapshot } from '../bridge'
import { TelescodeError, fromNativeError } from '../bridge/errors'
import type { TourPlan, TourStop } from './model'

export interface SavedTour {
  plan: TourPlan
  /** SHA-256 of the snapshot the tour was written against. */
  snapshotHash: string
  state: string
  updatedMs: number
}

export interface TourLibrary {
  list(): Promise<SavedTour[]>
}

/** The tours the MCP server saved, read through the `list_tours` Tauri command. */
export const tauriTourLibrary: TourLibrary = {
  async list() {
    if (!isTauri()) {
      throw new TelescodeError('bridge_unavailable', 'Saved tours are only reachable from the desktop app.')
    }
    try {
      return await invoke<SavedTour[]>('list_tours')
    } catch (e) {
      throw fromNativeError(e)
    }
  },
}

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
): SavedTour | null {
  const playable = tours.filter((t) => {
    const stops: unknown = t.plan?.stops
    if (!Array.isArray(stops) || stops.length === 0 || !stops.every(isStop)) return false
    if (!stops.every((s) => hasNode(s.nodeId))) return false
    return t.snapshotHash === snapshotHash || stops.some((s) => s.nodeId !== 'root')
  })
  const exact = (t: SavedTour) => (t.snapshotHash === snapshotHash ? 1 : 0)
  return playable.sort((a, b) => exact(b) - exact(a) || b.updatedMs - a.updatedMs)[0] ?? null
}

/** The hash the MCP server records for a snapshot: SHA-256 of its JSON. */
export async function snapshotHash(snapshot: AnalysisSnapshot): Promise<string | null> {
  if (!globalThis.crypto?.subtle) return null
  const digest = await crypto.subtle.digest('SHA-256', new TextEncoder().encode(JSON.stringify(snapshot)))
  return [...new Uint8Array(digest)].map((b) => b.toString(16).padStart(2, '0')).join('')
}

/** What to ask the agent connected to the Telescode MCP server for. */
export function tutorialRequest(dbPath: string): string {
  return `Using the Telescode MCP server, open_project("${dbPath}") and create an onboarding tour draft for this repository.`
}

/** Playback runs a little quicker than the tour's video timing: there, the viewer cannot pause. */
export const PLAYBACK_SPEED = 1.25

/** How long a step stays on screen during playback. */
export function stepDurationMs(stop: Pick<TourStop, 'transitionMs' | 'holdMs'>): number {
  return Math.round((stop.transitionMs + stop.holdMs) / PLAYBACK_SPEED)
}
