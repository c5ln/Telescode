import { agentInvoke, followRun, newRuns, stoppable, type AgentProvider, type AgentRun, type OnToolCall } from '../agent'
import type { LearningGoal } from './model'
import type { LearningAnswer, LearningResult, LearningView } from './contracts'
export type GoalSeed = Omit<LearningGoal, 'repositoryId' | 'sourceSnapshotHash'>
export interface LearningLibrary {
  /** `learnerId` keeps what was already asked apart per learner; the local learner by default. */
  start(input: { dbPath: string; goal: GoalSeed; learnerId?: string }): Promise<LearningView>
  get(sessionId: string): Promise<LearningView>
  /** Reserves or joins the preparation job and resolves only once the session has left `preparing`;
   * waiting and its deadline belong to the native side, so callers never poll. */
  prepare(input: { sessionId: string; revision: number; curriculum?: unknown }, onToolCall?: OnToolCall): AgentRun<LearningView>
  respond(input: LearningAnswer): Promise<LearningResult>
  /** The goal whose material ships with the app (an example's suggested goal), once known. It is never
   * written by an agent, so a session for it ends when that material is used up. */
  preparedGoalId?(): string | null
  advance(input: { sessionId: string; revision: number; stopId: string; outcome: 'continue' | 'skip' | 'stop' }): Promise<LearningView>
}
export function createLearningLibrary(provider: AgentProvider): LearningLibrary {
  return { start: input => agentInvoke('start_learning_session', input), get: sessionId => agentInvoke('get_learning_session', { sessionId }),
    prepare: (input, onToolCall) => {
      let view: LearningView | undefined
      // The shell already joins duplicate preparations, so each call follows its own run.
      const run = followRun(newRuns(), 'prepare', async (progressId) => {
        view = await agentInvoke<LearningView>('prepare_learning_session', { ...input, provider, progressId })
      }, onToolCall)
      return stoppable(run.then(() => view!), run.cancel)
    },
    respond: answer => agentInvoke('record_learning_response', { answer }), advance: input => agentInvoke('advance_learning_session', { input }) }
}
