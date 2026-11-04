import type { Difficulty } from '../quiz/model'

export interface LearningGoal {
  goalId: string
  goalRevision: number
  rawQuery: string
  retrievalQuery: string
  repositoryId: string
  sourceSnapshotHash: string
  targetConcepts: { conceptId: string; level: Difficulty; weight: number }[]
  budget: number
}
export interface Knowledge {
  conceptId: string
  level: Difficulty
  probability: number
  observations: number
}
export interface LearningEffect {
  conceptId: string
  level: Difficulty
  learn: number
  prerequisites?: { conceptId: string; level: Difficulty }[]
  unpreparedLearn?: number
}
export interface LearningAction {
  id: string
  kind: 'read' | 'explain' | 'compare' | 'diagnose'
  nodeId: string
  conceptId: string
  level: Difficulty
  cost: number
  caption: string
  effects: LearningEffect[]
  observation?: { conceptId: string; level: Difficulty; guess: number; slip: number }
  returnTo?: string
}
export interface Decision {
  actionId: string | null
  value: number
  expectedGain: number
  nodes: number
  truncated: boolean
  branches?: { correct: Decision; incorrect: Decision }
}

const LEVELS = ['easy', 'medium', 'hard']
export const MODEL_VERSION = 'independent-bkt-actions-v1'
export const PRIOR = .3
const key = (conceptId: string, level: string) => `${conceptId}\0${level}`
const probability = (value: number, name: string) => {
  if (!Number.isFinite(value) || value < 0 || value > 1) throw new Error(`Invalid ${name} probability`)
}
export function validateGoal(goal: LearningGoal): void {
  if (!goal || !goal.goalId?.trim() || !Number.isInteger(goal.goalRevision) || goal.goalRevision < 1 ||
      !goal.rawQuery?.trim() || !goal.retrievalQuery?.trim() || !goal.repositoryId?.trim() || !goal.sourceSnapshotHash?.trim()) throw new Error('Invalid learning goal')
  if (!Number.isFinite(goal.budget) || goal.budget < 0) throw new Error('Invalid goal budget')
  if (!goal.targetConcepts.length || goal.targetConcepts.some(t => !t.conceptId?.trim() || !LEVELS.includes(t.level) || !Number.isFinite(t.weight) || t.weight < 0) ||
      !Number.isFinite(goal.targetConcepts.reduce((n, t) => n + t.weight, 0)) ||
      goal.targetConcepts.reduce((n, t) => n + t.weight, 0) <= 0) throw new Error('Invalid goal weights')
  const dimensions = goal.targetConcepts.map(t => key(t.conceptId, t.level))
  if (new Set(dimensions).size !== dimensions.length) throw new Error('Duplicate goal dimension')
}
export function mastery(knowledge: Knowledge[], conceptId: string, level: Difficulty): number {
  return knowledge.find(k => k.conceptId === conceptId && k.level === level)?.probability ?? PRIOR
}
export function goalScore(goal: LearningGoal, knowledge: Knowledge[]): number {
  validateGoal(goal)
  return goal.targetConcepts.reduce((sum, t) => sum + t.weight * mastery(knowledge, t.conceptId, t.level), 0) /
    goal.targetConcepts.reduce((sum, t) => sum + t.weight, 0)
}
function put(knowledge: Knowledge[], conceptId: string, level: Difficulty, value: number, evidence: boolean): Knowledge[] {
  const out = structuredClone(knowledge)
  let state = out.find(k => k.conceptId === conceptId && k.level === level)
  if (!state) { state = { conceptId, level, probability: PRIOR, observations: 0 }; out.push(state) }
  state.probability = value
  if (evidence) state.observations++
  return out
}
export function observe(knowledge: Knowledge[], observation: NonNullable<LearningAction['observation']>, correct: boolean): Knowledge[] {
  probability(observation.guess, 'guess'); probability(observation.slip, 'slip')
  const prior = mastery(knowledge, observation.conceptId, observation.level)
  const known = prior * (correct ? 1 - observation.slip : observation.slip)
  const unknown = (1 - prior) * (correct ? observation.guess : 1 - observation.guess)
  if (known + unknown === 0) throw new Error('Observation has zero probability under this model')
  return put(knowledge, observation.conceptId, observation.level, known / (known + unknown), true)
}
/** Expected learning under an explicitly assumed model, never observed proof of mastery. */
export function predict(knowledge: Knowledge[], action: LearningAction): Knowledge[] {
  let next = structuredClone(knowledge)
  for (const effect of action.effects) {
    probability(effect.learn, 'learn'); probability(effect.unpreparedLearn ?? effect.learn, 'unprepared learn')
    const readiness = (effect.prerequisites ?? []).reduce((p, c) => p * mastery(knowledge, c.conceptId, c.level), 1)
    const learn = (effect.unpreparedLearn ?? effect.learn) * (1 - readiness) + effect.learn * readiness
    const prior = mastery(knowledge, effect.conceptId, effect.level)
    next = put(next, effect.conceptId, effect.level, prior + (1 - prior) * learn, false)
  }
  return next
}
export function chooseAction(goal: LearningGoal, knowledge: Knowledge[], actions: LearningAction[], options: { depth: number; maxNodes: number; remainingBudget?: number; maxMs?: number }): Decision {
  validateGoal(goal)
  const dimensions = knowledge.map(k => key(k.conceptId, k.level))
  if (new Set(dimensions).size !== dimensions.length) throw new Error('Duplicate knowledge dimension')
  for (const k of knowledge) probability(k.probability, 'mastery')
  if (!Number.isInteger(options.depth) || options.depth < 0 || options.depth > 3 || !Number.isInteger(options.maxNodes) || options.maxNodes < 1 || options.maxNodes > 50000) throw new Error('Invalid planner limits')
  const budget = options.remainingBudget ?? goal.budget
  if (!Number.isFinite(budget) || budget < 0 || budget > goal.budget) throw new Error('Invalid remaining budget')
  if (new Set(actions.map(a => a.id)).size !== actions.length) throw new Error('Duplicate action ID')
  if (actions.length > 5) throw new Error('Planner supports at most five selected candidates')
  for (const a of actions) {
    if (!Number.isFinite(a.cost) || a.cost <= 0) throw new Error('Invalid action cost')
    if (a.kind === 'diagnose' && a.effects.length) throw new Error('Diagnosis cannot grant learning')
    for (const e of a.effects) { probability(e.learn, 'learn'); probability(e.unpreparedLearn ?? e.learn, 'unprepared learn') }
    if (a.observation) { probability(a.observation.guess, 'guess'); probability(a.observation.slip, 'slip') }
  }
  const started = performance.now()
  let nodes = 0, truncated = false
  const search = (state: Knowledge[], available: LearningAction[], left: number, depth: number): Decision => {
    const baseline = goalScore(goal, state)
    const terminal: Decision = { actionId: null, value: baseline, expectedGain: 0, nodes: 0, truncated: false }
    if (depth === 0 || left <= 0) return terminal
    if (nodes >= options.maxNodes || performance.now() - started > (options.maxMs ?? 250)) { truncated = true; return terminal }
    nodes++
    let best = terminal
    for (const action of available.slice(0, 5)) {
      if (action.cost > left) continue
      if (nodes >= options.maxNodes || performance.now() - started > (options.maxMs ?? 250)) { truncated = true; break }
      nodes++
      const next = predict(state, action)
      const remaining = available.filter(a => a.id !== action.id)
      let value: number
      let branches: Decision['branches']
      if (action.observation) {
        const o = action.observation
        const p = mastery(next, o.conceptId, o.level) * (1 - o.slip) + (1 - mastery(next, o.conceptId, o.level)) * o.guess
        const correct = p > 0 ? search(observe(next, o, true), remaining, left - action.cost, depth - 1) : terminal
        const incorrect = p < 1 ? search(observe(next, o, false), remaining, left - action.cost, depth - 1) : terminal
        branches = { correct, incorrect }
        value = p * correct.value + (1 - p) * incorrect.value
      } else value = search(next, remaining, left - action.cost, depth - 1).value
      if (value > best.value + 1e-12) best = { actionId: action.id, value, expectedGain: value - baseline, nodes: 0, truncated: false, ...(branches ? { branches } : {}) }
    }
    return best
  }
  const decision = search(knowledge, actions, budget, options.depth)
  return { ...decision, nodes, truncated }
}
