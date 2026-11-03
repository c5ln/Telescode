import { invoke, isTauri } from '@tauri-apps/api/core'
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
