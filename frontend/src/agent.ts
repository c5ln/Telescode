import { invoke, isTauri } from '@tauri-apps/api/core'
import { listen } from '@tauri-apps/api/event'
import { TelescodeError, fromNativeError } from './bridge/errors'

export type AgentProvider = 'claude' | 'codex'
export interface AgentStatus { installed: boolean; signedIn: boolean }

export const agents = {
  claude: { name: 'Claude Code', account: 'Claude' },
  codex: { name: 'Codex', account: 'Codex' },
} as const

const SELECTION_KEY = 'telescode.agent'
export function savedAgent(): AgentProvider {
  try { return localStorage.getItem(SELECTION_KEY) === 'codex' ? 'codex' : 'claude' } catch { return 'claude' }
}
export function saveAgent(provider: AgentProvider) {
  try { localStorage.setItem(SELECTION_KEY, provider) } catch { /* Selection still works for this session. */ }
}

export async function agentInvoke<T>(command: string, args?: Record<string, unknown>): Promise<T> {
  if (!isTauri()) throw new TelescodeError('bridge_unavailable', 'Agent generation is only available in the desktop app.')
  try { return await invoke<T>(command, args) } catch (error) { throw fromNativeError(error) }
}

export function agentStatus(provider: AgentProvider) { return agentInvoke<AgentStatus>('agent_status', { provider }) }
export function signIn(provider: AgentProvider) { return agentInvoke<void>('agent_sign_in', { provider }) }

export function installCommand(provider: AgentProvider): string {
  if (provider === 'codex') return 'npm install -g @openai/codex'
  return /Windows/i.test(navigator.userAgent)
    ? 'irm https://claude.ai/install.ps1 | iex'
    : 'curl -fsSL https://claude.ai/install.sh | bash'
}

/** One Telescode tool the agent called, as the shell read it from the agent's event stream. */
export interface ToolCall {
  tool: string
  nodeId: string | null
}
export type OnToolCall = (call: ToolCall) => void

/** What an agent has done so far in one run. */
export interface AgentActivity {
  startedMs: number
  calls: { call: ToolCall; atMs: number }[]
  /** Stops the run, when it can be stopped; it then fails with `agent_cancelled`. */
  cancel?: () => void
}

/** The nodes `activity` has read, each once, the latest last: what the map outlines. */
export function readNodes(activity: AgentActivity | undefined): string[] {
  const ids = new Set<string>()
  for (const { call } of activity?.calls ?? []) {
    if (!call.nodeId) continue
    ids.delete(call.nodeId)
    ids.add(call.nodeId)
  }
  return [...ids]
}

/** A run the shell follows: settles when the agent is done, and can be stopped meanwhile. */
export type AgentRun<T = void> = Promise<T> & { cancel?: () => void }

/** `promise`, stoppable with `cancel`. */
export function stoppable<T>(promise: Promise<T>, cancel: (() => void) | undefined): AgentRun<T> {
  return Object.assign(promise, { cancel })
}

/** `run`, calling `done` once it settles, still stoppable. */
export function whileRunning<T>(run: Promise<T> & { cancel?: () => void }, done: () => void): AgentRun<T> {
  return stoppable(run.finally(done), run.cancel)
}

/** The activity once its run is known, so the progress view can offer to stop it. */
export function withRun(activity: AgentActivity, run: AgentRun<unknown>): AgentActivity {
  return run.cancel ? { ...activity, cancel: run.cancel } : activity
}

/** What an agent writes. */
export type AgentWork = 'tour' | 'quiz' | 'learning path'

const DURATIONS_KEY = 'telescode.agentDurations'
/** How many finished runs of each kind the usual range is taken from. */
const DURATIONS_KEPT = 5

function savedDurations(): Partial<Record<AgentWork, number[]>> {
  try {
    const saved: unknown = JSON.parse(localStorage.getItem(DURATIONS_KEY) ?? '{}')
    return saved && typeof saved === 'object' ? (saved as Partial<Record<AgentWork, number[]>>) : {}
  } catch {
    return {}
  }
}

/** Remembers how long a finished run took, for the range the next one shows. */
export function recordDuration(work: AgentWork, ms: number) {
  if (!(ms > 0)) return
  const all = savedDurations()
  all[work] = [...(all[work] ?? []), Math.round(ms)].slice(-DURATIONS_KEPT)
  try { localStorage.setItem(DURATIONS_KEY, JSON.stringify(all)) } catch { /* The range is a hint; without storage it is not shown. */ }
}

/** The shortest and longest of the recent finished runs of `work` on this computer, if any. */
export function usualRange(work: AgentWork): { min: number; max: number } | null {
  const times = (savedDurations()[work] ?? []).filter((t) => typeof t === 'number' && t > 0)
  return times.length ? { min: Math.min(...times), max: Math.max(...times) } : null
}

/** Whether `error` is the user stopping the agent rather than the agent failing. */
export function isCancelled(error: unknown): boolean {
  return error instanceof TelescodeError && error.code === 'agent_cancelled'
}

export function startActivity(now = Date.now()): AgentActivity {
  return { startedMs: now, calls: [] }
}

export function addCall(activity: AgentActivity, call: ToolCall, now = Date.now()): AgentActivity {
  return { ...activity, calls: [...activity.calls, { call, atMs: now }] }
}

interface FollowedRun {
  promise: Promise<void>
  cancel: () => void
  listeners: Set<OnToolCall>
  calls: ToolCall[]
}

/**
 * Starts `start(progressId)` once per `key` and passes every tool call of
 * that run to each caller's `onToolCall`, earlier calls first for a caller
 * that joins late. The shell tags its `agent-progress` events with the ID.
 */
export function followRun(
  runs: Map<string, FollowedRun>,
  key: string,
  start: (progressId: string) => Promise<void>,
  onToolCall?: OnToolCall,
): AgentRun {
  let run = runs.get(key)
  if (!run) {
    const listeners = new Set<OnToolCall>()
    const calls: ToolCall[] = []
    const progressId = crypto.randomUUID()
    const promise = (async () => {
      // Progress is only shown, never needed: without events the run goes on as before.
      const unlisten = await listen<{ progressId: string; tool: string; nodeId?: string | null }>('agent-progress', (e) => {
        if (e.payload.progressId !== progressId) return
        const call = { tool: e.payload.tool, nodeId: e.payload.nodeId ?? null }
        calls.push(call)
        listeners.forEach((l) => l(call))
      }).catch(() => () => {})
      try {
        await start(progressId)
      } finally {
        unlisten()
      }
    })().finally(() => runs.delete(key))
    // Joined runs share the agent, so stopping it stops it for every caller.
    const cancel = () => void agentInvoke<void>('agent_cancel', { progressId }).catch(() => {})
    run = { promise, cancel, listeners, calls }
    runs.set(key, run)
  }
  const joined = run
  if (onToolCall) {
    joined.calls.forEach(onToolCall)
    joined.listeners.add(onToolCall)
  }
  return stoppable(joined.promise.finally(() => onToolCall && joined.listeners.delete(onToolCall)), joined.cancel)
}

export function newRuns(): Map<string, FollowedRun> {
  return new Map()
}
