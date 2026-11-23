import type { McpServer } from '@modelcontextprotocol/sdk/server/mcp.js'
import { z } from 'zod'
import { CURRICULUM_LIMITS as L, LearningStore } from './learning.ts'
import type { TourStore } from './store.ts'
import type { QuizStore } from './quiz.ts'
import type { Curriculum } from '../../frontend/src/learning/contracts.ts'

export const LEARNING_INSTRUCTIONS = `Adaptive learning preparation is separate from video tours.
When asked to prepare a learning job, call get_learning_preparation_context(jobId), then save_learning_curriculum(jobId,curriculum).
Use only supplied source excerpts and graph facts. Use exact existing priorConcept definitions/versions for reused IDs.
Keep target IDs, required levels and goal unchanged. Start small, as rules.start says: at most rules.start.maxConcepts concepts with their relations,
and actions only at rules.start.levels with rules.start.questions questions in all, about the goal concepts in rules.start.concepts
(questions on other concepts are not asked until the goal needs them). The higher levels are written later as material.
Use nextCurriculumVersion. For supplements use new action IDs and new scenarios, not prior question stems with superficial rewording.
Concept definitions must be assessable behaviors. Relations are source-grounded hypotheses, not proven learning effects.
The context's rules are the exact limits save_learning_curriculum enforces; follow them before saving.
Pick each action nodeId from contexts[].nodeId. Questions must match the action level; their evidenceNodeIds come only from that context's evidence[level],
include the action node and have at least rules.questions.minEvidence[level] distinct nodes.
State why each explanation/code helps the goal. Never invent source behavior; when evidence is insufficient omit that item.
No evaluation/heldout material belongs in the curriculum. The server supplies provisional model rates; do not invent learning probabilities.
When asked to write material for a material job, call get_learning_material_context(jobId), then save_learning_material(jobId,items):
only the asked concepts and levels, one action per asked slot with two new questions grounded in that level's evidence, different from slots[].written.
Write the answer and distractors about the same length and detail, so the answer's length does not give it away.
Never record an answer, advance a session or mutate mastery. Only save the prepared materials and report the job ID.`

export function registerLearningTools(server: McpServer, tours: TourStore, quizzes: QuizStore) {
  const learning = new LearningStore(tours.outputRoot, tours, quizzes)
  const level = z.enum(['easy', 'medium', 'hard'])
  const target = z.object({ conceptId: z.string().min(1), level, weight: z.number().min(0) }).strict()
  const seed = z.object({ goalId: z.string().min(1), goalRevision: z.number().int().min(1), rawQuery: z.string().min(1).max(4000),
    retrievalQuery: z.string().min(1).max(4000), targetConcepts: z.array(target).min(1).max(8), budget: z.number().min(0).max(30),
    repositoryId: z.string().optional(), sourceSnapshotHash: z.string().optional() }).strict()
  const question = z.object({ type: z.enum(['multiple_choice', 'true_false']), difficulty: level, concept: z.string(), question: z.string(),
    answer: z.string(), distractors: z.array(z.string()).default([]), explanation: z.string(), evidenceNodeIds: z.array(z.string()).min(1) }).strict()
  const concept = z.object({ id: z.string().min(1), name: z.string().min(1).max(L.chars.conceptName), definition: z.string().min(1).max(L.chars.definition), version: z.number().int().min(1) }).strict()
  const relation = z.object({ from: z.string(), to: z.string(), kind: z.enum(['prerequisite', 'similar']), reason: z.string().min(1).max(L.chars.relationReason), evidenceNodeIds: z.array(z.string()).min(1) }).strict()
  /** The server's cost and learning model for an agent's action; the agent supplies neither. */
  const withEffects = (action: z.infer<typeof item>, prerequisites: { conceptId: string; level: 'easy' | 'medium' | 'hard' }[]) => ({
    ...action, cost: 1, effects: action.kind === 'diagnose' ? [] : [{ conceptId: action.conceptId, level: action.level,
      learn: .6, ...(prerequisites.length ? { prerequisites, unpreparedLearn: .15 } : {}) }] })
  const below = { easy: null, medium: 'easy', hard: 'medium' } as const
  const item = z.object({ id: z.string().min(1), kind: z.enum(['read', 'explain', 'compare', 'diagnose']), nodeId: z.string(), conceptId: z.string(), level,
    caption: z.string().min(1).max(L.chars.caption), questions: z.array(question).max(L.questionsPerAction), returnTo: z.string().optional() }).strict()
  const curriculum = z.object({ version: z.number().int().min(1), concepts: z.array(concept).min(1).max(L.concepts), items: z.array(item).min(1).max(L.actions), relations: z.array(relation).max(L.relations) }).strict()
  function tool(name: string, description: string, shape: z.ZodRawShape, readOnly: boolean, handler: (args: any) => unknown | Promise<unknown>) {
    server.registerTool(name, { description, inputSchema: shape, annotations: { readOnlyHint: readOnly, destructiveHint: false, openWorldHint: false } }, async args => {
      try { return { content: [{ type: 'text', text: JSON.stringify(await handler(args)) }] } }
      catch (error) { return { isError: true, content: [{ type: 'text', text: String(error) }] } }
    })
  }
  tool('start_learning_session', 'For the app: create a CPU-planned learning session for an explicit goal. DB paths are analyzed before ranking the actual retrievalQuery; default onboarding ranking is not required.', {
    projectId: z.string().optional(), path: z.string().optional(), sourceRoot: z.string().optional(), goal: seed, learnerId: z.string().min(1).max(200).default('local'),
  }, false, async ({ projectId, path, sourceRoot, goal, learnerId }) => {
    if (Boolean(projectId) === Boolean(path)) throw new Error('Provide exactly one projectId or path')
    let project = path ? await tours.openAnalysis(path, undefined, sourceRoot) : tours.project(projectId)
    if (project.databasePath) project = await tours.rank(project.id, goal.retrievalQuery)
    return learning.start(project, { ...goal, repositoryId: goal.repositoryId ?? project.sourceRoot ?? project.databasePath ?? project.hash,
      sourceSnapshotHash: goal.sourceSnapshotHash ?? project.sourceSnapshotHash ?? project.hash }, learnerId)
  })
  tool('get_learning_session', 'Read a persisted session with redacted fresh question, provisional estimates and current revision.', { sessionId: z.string() }, true, ({ sessionId }) => learning.get(sessionId))
  tool('request_learning_preparation', 'For the app: reserve/join a bounded, source-versioned preparation job; reuse matching cached materials. Caller runs the selected CLI only when shouldRun is true.', {
    sessionId: z.string(), revision: z.number().int().min(1), provider: z.enum(['codex', 'claude']),
    ownerPid: z.number().int().positive().describe('PID of the app process that runs the CLI when shouldRun is true'),
    useCache: z.boolean().optional().describe('false when the app saves material of its own for this job, so no cached curriculum stands in for it'),
  }, false, ({ sessionId, revision, provider, ownerPid, useCache }) => learning.requestPreparation(sessionId, revision, provider, ownerPid, { useCache }))
  tool('get_learning_preparation_context', 'For the material-writing agent: read frozen goal, prior concept definitions and code excerpts for this exact job. Never answer for the learner.', { jobId: z.string() }, true, ({ jobId }) => learning.jobContext(jobId))
  tool('save_learning_curriculum', 'For the material-writing agent: save grounded concepts/actions/questions for this job. Probabilities and action costs are assigned by the server, not generated by the agent.', {
    jobId: z.string(), curriculum,
  }, false, ({ jobId, curriculum: proposal }) => {
    const prepared: Curriculum = { ...proposal, items: proposal.items.map((action: z.infer<typeof item>) => withEffects(action,
      proposal.relations.filter((r: z.infer<typeof relation>) => r.kind === 'prerequisite' && r.to === action.conceptId)
        .map((r: z.infer<typeof relation>) => ({ conceptId: r.from, level: action.level })))) }
    return learning.saveJobCurriculum(jobId, prepared)
  })
  tool('request_learning_material', 'For the app, while the learner answers: work out what either answer to the current question needs next, add cached material at once, and reserve a job for the rest. Caller runs the selected CLI only when shouldRun is true.', {
    sessionId: z.string(), provider: z.enum(['codex', 'claude']),
    ownerPid: z.number().int().positive().describe('PID of the app process that runs the CLI when shouldRun is true'),
  }, false, ({ sessionId, provider, ownerPid }) => learning.requestMaterial(sessionId, provider, ownerPid))
  tool('get_learning_material_context', 'For the material-writing agent: the concepts, and for each asked concept and level the questions already written and the source it may cite. Never answer for the learner.', { jobId: z.string() }, true, ({ jobId }) => learning.materialContext(jobId))
  tool('save_learning_material', 'For the material-writing agent: save actions only for the asked concepts and levels, each with new questions grounded in that level\'s evidence. Costs and learning effects are assigned by the server.', {
    jobId: z.string(), items: z.array(item).min(1).max(8),
  }, false, ({ jobId, items }) => learning.saveMaterial(jobId, items.map((action: z.infer<typeof item>) => {
    // Each level builds on the one below it of the same concept.
    const lower = below[action.level]
    return withEffects(action, lower ? [{ conceptId: action.conceptId, level: lower }] : [])
  })))
  tool('finish_learning_material', 'For the app: report that the material worker ended; one that saved nothing counts as a failed attempt for its levels.', { jobId: z.string(), error: z.string().max(2000).optional() }, false,
    async ({ jobId, error }) => { await learning.finishMaterial(jobId, error); return { completed: true } })
  tool('finish_learning_preparation', 'For the app: report worker completion/failure without discarding answers or accepting stale results.', { jobId: z.string(), error: z.string().max(2000).optional() }, false,
    async ({ jobId, error }) => { await learning.finishPreparation(jobId, error); return { completed: true } })
  tool('record_learning_response', 'For the app only: server-grade a real learner answer, atomically update shared evidence and remaining policy. Retry the identical submissionId after a transport failure. Never for generation agents.', {
    sessionId: z.string(), revision: z.number().int().min(1), stopId: z.string(), submissionId: z.string(), questionId: z.string(), selectedIndex: z.number().int().min(0),
  }, false, input => learning.respond(input))
  tool('advance_learning_session', 'For the app only: explicitly complete, skip or end. Does not move the code on answer alone; stop also works during preparation with no stopId.', {
    sessionId: z.string(), revision: z.number().int().min(1), stopId: z.string().default(''), outcome: z.enum(['continue', 'skip', 'stop']),
  }, false, ({ sessionId, revision, stopId, outcome }) => learning.advance(sessionId, revision, stopId, outcome))
  return learning
}
