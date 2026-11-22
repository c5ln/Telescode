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
}

export function startActivity(now = Date.now()): AgentActivity {
  return { startedMs: now, calls: [] }
}

export function addCall(activity: AgentActivity, call: ToolCall, now = Date.now()): AgentActivity {
  return { ...activity, calls: [...activity.calls, { call, atMs: now }] }
}

interface FollowedRun {
  promise: Promise<void>
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
): Promise<void> {
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
    run = { promise, listeners, calls }
    runs.set(key, run)
  }
  const joined = run
  if (onToolCall) {
    joined.calls.forEach(onToolCall)
    joined.listeners.add(onToolCall)
  }
  return joined.promise.finally(() => onToolCall && joined.listeners.delete(onToolCall))
}

export function newRuns(): Map<string, FollowedRun> {
  return new Map()
}
