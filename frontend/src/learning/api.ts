import { agentInvoke, followRun, newRuns, type AgentProvider, type OnToolCall } from '../agent'
import type { LearningGoal } from './model'
import type { LearningAnswer, LearningResult, LearningView } from './contracts'
export type GoalSeed = Omit<LearningGoal, 'repositoryId' | 'sourceSnapshotHash'>
export interface LearningLibrary {
  start(input: { dbPath: string; goal: GoalSeed }): Promise<LearningView>
  get(sessionId: string): Promise<LearningView>
  /** Reserves or joins the preparation job and resolves only once the session has left `preparing`;
   * waiting and its deadline belong to the native side, so callers never poll. */
  prepare(input: { sessionId: string; revision: number; curriculum?: unknown }, onToolCall?: OnToolCall): Promise<LearningView>
  respond(input: LearningAnswer): Promise<LearningResult>
  advance(input: { sessionId: string; revision: number; stopId: string; outcome: 'continue' | 'skip' | 'stop' }): Promise<LearningView>
}
export function createLearningLibrary(provider: AgentProvider): LearningLibrary {
  return { start: input => agentInvoke('start_learning_session', input), get: sessionId => agentInvoke('get_learning_session', { sessionId }),
    prepare: async (input, onToolCall) => {
      let view: LearningView | undefined
      // The shell already joins duplicate preparations, so each call follows its own run.
      await followRun(newRuns(), 'prepare', async (progressId) => {
        view = await agentInvoke<LearningView>('prepare_learning_session', { ...input, provider, progressId })
      }, onToolCall)
      return view!
    },
    respond: answer => agentInvoke('record_learning_response', { answer }), advance: input => agentInvoke('advance_learning_session', { input }) }
}
