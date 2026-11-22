// A tour, quiz or learning path the AI agent could not finish, in the same
// layout as every other error, inside its panel.

import { agents, type AgentProvider } from '../agent'
import { Button } from '../ui/Button'
import { ErrorState } from '../ui/ErrorState'

interface AgentErrorProps {
  /** What the agent was writing. */
  what: 'tour' | 'quiz' | 'learning path'
  /** Signing in failed, rather than the writing. */
  signingIn?: boolean
  /** The shell's message, kept for the details. */
  message: string
  provider: AgentProvider
  onRetry: () => void
  /** Offered when given: switch to the other agent. */
  onSwitchAgent?: (provider: AgentProvider) => void
}

/** Why the agent stopped, from the shell's message. */
function cause(message: string, name: string, what: string): string {
  if (/was stopped before it finished/i.test(message)) return `You stopped ${name} before it saved the ${what}.`
  if (/did not finish in time/i.test(message)) return `${name} ran past the time limit and stopped before saving the ${what}.`
  if (/without saving/i.test(message)) return `${name} finished without saving a ${what}.`
  if (/could not start/i.test(message)) return `${name} could not be started.`
  if (/\bMCP\b/.test(message)) return 'Telescode could not reach its MCP server.'
  return `${name} stopped before saving the ${what}.`
}

export function AgentError({ what, signingIn = false, message, provider, onRetry, onSwitchAgent }: AgentErrorProps) {
  const { name, account } = agents[provider]
  const other: AgentProvider = provider === 'claude' ? 'codex' : 'claude'

  if (signingIn) {
    return (
      <ErrorState
        layout="inline"
        title="Sign-in did not finish"
        cause={`The ${account} sign-in was closed or did not complete.`}
        next="Try again, and finish signing in in your browser."
        details={message}
      >
        <Button variant="primary" onClick={onRetry}>Try again</Button>
      </ErrorState>
    )
  }

  return (
    <ErrorState
      layout="inline"
      title={`The ${what} was not finished`}
      cause={cause(message, name, what)}
      next={onSwitchAgent ? `Try again, or switch to ${agents[other].name} in Connect MCP.` : 'Try again.'}
      details={message}
    >
      {onSwitchAgent && <Button onClick={() => onSwitchAgent(other)}>Switch to {agents[other].name}</Button>}
      <Button variant="primary" onClick={onRetry}>Try again</Button>
    </ErrorState>
  )
}
