// Quizzes on tutorial steps, as the app reaches them: kept and graded by the
// Telescode MCP server, written by the selected CLI agent (src-tauri/src/quizzes.rs).
// This module picks the quiz set that belongs to the current step.

import { invoke, isTauri } from '@tauri-apps/api/core'

import { TelescodeError, fromNativeError } from '../bridge/errors'
import { agentStatus, signIn, type AgentProvider, type AgentStatus } from '../agent'
import type { QuizFeedback, QuizResponse, QuizSet, TourContext } from './model'

export interface QuizAnswer {
  quizSetId: string
  quizId: string
  selectedIndex: number
  responseTimeMs: number | null
}

export interface QuizLibrary {
  list(): Promise<QuizSet[]>
  /** Have a quiz written for one tour step; resolves once it is saved. */
  generate?(target: TourContext & { dbPath: string }): Promise<void>
  /** Grade and record an answer. */
  respond(answer: QuizAnswer): Promise<{ response: QuizResponse; feedback: QuizFeedback }>
  /** Whether the agent is installed and signed in. Asked only when a quiz must be written. */
  agentStatus?(): Promise<AgentStatus>
  /** Start the agent's sign-in in the browser; resolves once signed in. */
  signIn?(): Promise<void>
}

async function invokeQuizzes<T>(command: string, args?: Record<string, unknown>): Promise<T> {
  if (!isTauri()) {
    throw new TelescodeError('bridge_unavailable', 'Quizzes are only reachable from the desktop app.')
  }
  try {
    return await invoke<T>(command, args)
  } catch (e) {
    throw fromNativeError(e)
  }
}

/** Generations under way, by tour step: asking again joins the one already running. */
const generating = new Map<string, Promise<void>>()

export function createQuizLibrary(provider: AgentProvider): QuizLibrary {
  return {
    list: () => invokeQuizzes<QuizSet[]>('list_quizzes'),
    respond: (answer) => invokeQuizzes('record_quiz_response', { ...answer }),
    agentStatus: () => agentStatus(provider),
    signIn: () => signIn(provider),
    generate(target) {
      const key = `${provider}\n${target.dbPath}\n${target.tourId}\n${target.tourRevision}\n${target.stopIndex}`
      let run = generating.get(key)
      if (!run) {
        run = invokeQuizzes<void>('generate_quiz', { target, provider }).finally(() => generating.delete(key))
        generating.set(key, run)
      }
      return run
    },
  }
}
export const tauriQuizLibrary = createQuizLibrary('claude')

/**
 * The quiz set for a tour step: one written for this revision and stop of
 * the tour, about the same node, preferring this exact snapshot, then the
 * newest.
 */
export function pickQuiz(sets: readonly QuizSet[], at: TourContext, snapshotHash: string | null): QuizSet | null {
  const matching = sets.filter(
    (s) =>
      s.tourId === at.tourId &&
      s.tourRevision === at.tourRevision &&
      s.stopIndex === at.stopIndex &&
      s.nodeId === at.nodeId &&
      Array.isArray(s.questions) &&
      s.questions.length > 0,
  )
  const exact = (s: QuizSet) => (s.snapshotHash === snapshotHash ? 1 : 0)
  return matching.sort((a, b) => exact(b) - exact(a) || b.createdMs - a.createdMs)[0] ?? null
}
