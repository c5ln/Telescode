/** Offline policy comparison for the adaptive planner (plan step 6 / 8).
 * SIMULATED learners only: results are not evidence of real human learning effect. Transition
 * parameters are design assumptions. The simulated learner is a sampled binary-state model that is
 * separate from the planner's belief model, and it is run under several assumption settings. */
import type { Difficulty } from '../../frontend/src/quiz/model.ts'
import { chooseAction, goalScore, mastery, observe, predict, PRIOR, type Knowledge, type LearningAction, type LearningGoal } from '../../frontend/src/learning/model.ts'

export const SIMULATION_DISCLAIMER = 'SIMULATED learners only. Not evidence of real human learning effect; transition parameters are design assumptions.'

export interface TrainingQuestion { id: string; text: string; choices: number }
export interface HeldOutItem { id: string; text: string; conceptId: string; level: Difficulty; choices: number }
export interface ScenarioAction extends LearningAction { questions: TrainingQuestion[] }
export interface Scenario {
  id: string
  description: string
  goal: LearningGoal
  actions: ScenarioAction[]
  /** Relevance order of the query-based fixed path. */
  fixedOrder: string[]
  /** Probability that a simulated learner already knows each dimension (`concept:level`). */
  priors: Record<string, number>
  /** True dependencies the planner's action model does not declare; active only in settings with `hiddenPrerequisites`. */
  hiddenPrerequisites?: { conceptId: string; level: Difficulty; requires: { conceptId: string; level: Difficulty }; unpreparedLearn: number }[]
  /** Never passed to a policy. Pre and post sets are disjoint items of equal difficulty. */
  evaluation: { pre: HeldOutItem[]; post: HeldOutItem[] }
}
export interface TrueSetting {
  name: string
  description: string
  /** Multiplies an action's planner-assumed learn rate in the simulated learner. */
  learnScale: number
  /** Extra per-kind multiplier; lets a setting reverse which action kinds actually teach best. */
  kindScale?: Partial<Record<LearningAction['kind'], number>>
  /** Multiplies the planner-assumed unprepared learn rate. */
  unpreparedScale: number
  /** False: prerequisites do not affect true learning, contrary to the planner's assumption. */
  prerequisiteMatters: boolean
  slip: number
  /** Enables the scenario's hidden prerequisites (structural misspecification unknown to the planner). */
  hiddenPrerequisites?: boolean
  /** True when the planner's assumptions do not match this simulated learner. */
  misspecified: boolean
}
export type PolicyName = 'fixed' | 'rule' | 'search-d1' | 'search-d2' | 'search-d3'
export const POLICIES: PolicyName[] = ['fixed', 'rule', 'search-d1', 'search-d2', 'search-d3']
export const PLANNER_SLIP = .1

const dim = (conceptId: string, level: string) => `${conceptId}:${level}`
const ITEM_CHOICES = 4
const repo = { repositoryId: 'simulated-repository', sourceSnapshotHash: 'simulated-source', goalRevision: 1 }
const teach = (id: string, kind: LearningAction['kind'], conceptId: string, level: Difficulty, learn: number, questions: number,
  prerequisite?: { conceptId: string; level: Difficulty; unpreparedLearn: number }): ScenarioAction => ({
  id, kind, nodeId: `file:${id}.py`, conceptId, level, cost: 1, caption: `${kind} ${conceptId}`,
  // Same assumed rates as the store assigns to AI-prepared actions (learn .6, unprepared .15) unless stated.
  effects: kind === 'diagnose' ? [] : [{ conceptId, level, learn, ...(prerequisite ? { prerequisites: [{ conceptId: prerequisite.conceptId, level: prerequisite.level }], unpreparedLearn: prerequisite.unpreparedLearn } : {}) }],
  questions: Array.from({ length: questions }, (_, i) => ({ id: `${id}-training-${i + 1}`, text: `Training scenario ${i + 1} for ${id}`, choices: ITEM_CHOICES })),
})
const items = (prefix: string, conceptId: string, level: Difficulty, count: number): HeldOutItem[] =>
  Array.from({ length: count }, (_, i) => ({ id: `${prefix}-${conceptId}-${level}-${i + 1}`, text: `Held-out ${prefix} task ${i + 1} on ${conceptId}/${level}`, conceptId, level, choices: ITEM_CHOICES }))

export const TRUE_SETTINGS: TrueSetting[] = [
  { name: 'conservative', description: 'learning x0.6, unprepared x0.3, read x0.5 / explain x1.3 (reverses the assumed read > explain order), slip .2',
    learnScale: .6, unpreparedScale: .3, kindScale: { read: .5, explain: 1.3 }, prerequisiteMatters: true, slip: .2, misspecified: true },
  { name: 'default', description: 'planner-matched rates and slip (still a sampled binary learner, not the planner belief update)',
    learnScale: 1, unpreparedScale: 1, prerequisiteMatters: true, slip: PLANNER_SLIP, misspecified: false },
  { name: 'optimistic', description: 'learning x1.4 (capped at 1), prerequisites do not matter, slip .05',
    learnScale: 1.4, unpreparedScale: 1, prerequisiteMatters: false, slip: .05, misspecified: true },
  { name: 'hidden-prerequisite', description: 'planner-matched rates, plus a true prerequisite the planner does not model (learning capped at .15 until it is known)',
    learnScale: 1, unpreparedScale: 1, prerequisiteMatters: true, slip: PLANNER_SLIP, hiddenPrerequisites: true, misspecified: true },
]
const B = { conceptId: 'B', level: 'easy' as const, unpreparedLearn: .15 }
export const SCENARIOS: Scenario[] = [
  { id: 'prerequisite-return', description: 'target A needs prerequisite B; return to A after remediation',
    goal: { ...repo, goalId: 'prerequisite-return', rawQuery: 'Understand A', retrievalQuery: 'A', budget: 5, targetConcepts: [{ conceptId: 'A', level: 'easy', weight: 1 }] },
    actions: [teach('read-A', 'read', 'A', 'easy', .6, 2, B), teach('explain-A', 'explain', 'A', 'easy', .5, 1, B),
      teach('compare-A', 'compare', 'A', 'easy', .4, 1, { ...B, unpreparedLearn: .2 }), teach('read-B', 'read', 'B', 'easy', .6, 1), teach('diagnose-A', 'diagnose', 'A', 'easy', 0, 2)],
    // Query relevance order, filled to the same budget (an action may repeat once, as in the store).
    fixedOrder: ['read-A', 'explain-A', 'compare-A', 'read-B', 'read-A'],
    priors: { 'A:easy': .15, 'B:easy': .5, 'D:easy': .4 },
    // D is outside the planner's concept map and no action teaches it.
    hiddenPrerequisites: [{ conceptId: 'A', level: 'easy', requires: { conceptId: 'D', level: 'easy' }, unpreparedLearn: .15 }],
    evaluation: { pre: items('pre', 'A', 'easy', 4), post: items('post', 'A', 'easy', 4) } },
  { id: 'two-targets', description: 'weighted targets A (easy, w2) and C (medium, w1) compete for a small budget',
    goal: { ...repo, goalId: 'two-targets', rawQuery: 'Understand A and C', retrievalQuery: 'A C', budget: 4,
      targetConcepts: [{ conceptId: 'A', level: 'easy', weight: 2 }, { conceptId: 'C', level: 'medium', weight: 1 }] },
    actions: [teach('read-A', 'read', 'A', 'easy', .6, 2), teach('explain-A', 'explain', 'A', 'easy', .5, 1),
      teach('read-C', 'read', 'C', 'medium', .6, 2), teach('compare-C', 'compare', 'C', 'medium', .4, 1), teach('diagnose-C', 'diagnose', 'C', 'medium', 0, 2)],
    fixedOrder: ['read-A', 'read-C', 'explain-A', 'compare-C'],
    priors: { 'A:easy': .3, 'C:medium': .4 },
    // The planner treats C as independent of A; truly C is hard to learn before A.
    hiddenPrerequisites: [{ conceptId: 'C', level: 'medium', requires: { conceptId: 'A', level: 'easy' }, unpreparedLearn: .15 }],
    evaluation: { pre: [...items('pre', 'A', 'easy', 3), ...items('pre', 'C', 'medium', 3)], post: [...items('post', 'A', 'easy', 3), ...items('post', 'C', 'medium', 3)] } },
]

/** Everything a policy may see. Held-out items and the simulated truth are never included. */
export interface PolicyInput { goal: LearningGoal; belief: Knowledge[]; remainingBudget: number; available: LearningAction[]; history: { actionId: string; conceptId: string; level: Difficulty; correct?: boolean }[] }
export interface PolicyContext { scenario: string; setting: string; policy: PolicyName; seed: number; step: number }
export interface EpisodeResult {
  scenario: string; setting: string; policy: PolicyName; seed: number
  goal: LearningGoal; budget: number; spent: number; steps: string[]
  /** Training question IDs observed during the episode, in order. */
  asked: string[]
  initialTruth: Record<string, boolean>; finalTruth: Record<string, boolean>
  pre: number; post: number; change: number; gain: number | null; preMax: boolean
  beliefStart: number; beliefEnd: number
  planningMs: number; nodes: number; truncated: number
}
export interface Stat { n: number; mean: number; sd: number; se: number }
export interface Row {
  scenario: string; setting: string; policy: PolicyName; seeds: number
  change: Stat; gain: Stat; preMaxCount: number; spent: Stat; steps: Stat; planningMs: Stat; nodes: Stat; truncated: number
}
export interface Flip { scenario: string; a: PolicyName; b: PolicyName; settings: [string, string]; significant: boolean }
export interface Experiment { seeds: number; firstSeed: number; episodes: EpisodeResult[]; rows: Row[]; flips: Flip[] }


/** mulberry32 over a splitmix-style seed mix, so streams for nearby seeds are unrelated. */
export function rng(seed: number): () => number {
  let a = Math.imul((seed ^ 0x9e3779b9) >>> 0, 0x85ebca6b) >>> 0
  a = Math.imul(a ^ (a >>> 13), 0xc2b2ae35) >>> 0
  return () => {
    a = (a + 0x6d2b79f5) >>> 0
    let t = a
    t = Math.imul(t ^ (t >>> 15), t | 1)
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61)
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296
  }
}
/** Independent random streams per seed: policies share the learner, the pre-test and the post-test draws. */
const stream = (seed: number, name: 'learner' | 'pre' | 'post' | 'answers' | 'learning') =>
  rng(seed * 8 + ['learner', 'pre', 'post', 'answers', 'learning'].indexOf(name))

/** Goal-weighted fraction of held-out items answered correctly (U on the independent test, not the planner estimate). */
export function evalScore(goal: LearningGoal, results: Record<string, { correct: number; total: number }>): number {
  let sum = 0, weights = 0
  for (const t of goal.targetConcepts) {
    const r = results[dim(t.conceptId, t.level)]
    if (!r || r.total <= 0) throw new Error(`No held-out items for ${t.conceptId}/${t.level}`)
    sum += t.weight * r.correct / r.total; weights += t.weight
  }
  return sum / weights
}
/** KnowLP E_p = (post - pre) / (max - pre); null for pre-max cases, which are reported separately. */
export function normalizedGain(pre: number, post: number, max = 1): number | null {
  return pre >= max ? null : (post - pre) / (max - pre)
}
export function summarize(values: number[]): Stat {
  const n = values.length
  if (!n) return { n: 0, mean: NaN, sd: NaN, se: NaN }
  const mean = values.reduce((a, b) => a + b, 0) / n
  const sd = n > 1 ? Math.sqrt(values.reduce((a, v) => a + (v - mean) ** 2, 0) / (n - 1)) : 0
  return { n, mean, sd, se: sd / Math.sqrt(n) }
}
export function aggregate(episodes: EpisodeResult[]): Row[] {
  const groups = new Map<string, EpisodeResult[]>()
  for (const e of episodes) {
    const key = `${e.scenario}\0${e.setting}\0${e.policy}`
    groups.set(key, [...groups.get(key) ?? [], e])
  }
  return [...groups.values()].map(group => ({ scenario: group[0]!.scenario, setting: group[0]!.setting, policy: group[0]!.policy, seeds: group.length,
    change: summarize(group.map(e => e.change)), gain: summarize(group.flatMap(e => e.gain === null ? [] : [e.gain])), preMaxCount: group.filter(e => e.preMax).length,
    spent: summarize(group.map(e => e.spent)), steps: summarize(group.map(e => e.steps.length)), planningMs: summarize(group.map(e => e.planningMs)),
    nodes: summarize(group.map(e => e.nodes)), truncated: group.reduce((n, e) => n + e.truncated, 0) }))
}
/** Pairs whose mean evaluated-change order differs between two assumption settings. Significant only when the
 * difference exceeds two standard errors in both settings. */
export interface Paired { scenario: string; setting: string; policy: PolicyName; baseline: PolicyName; diff: Stat }
/** Per-seed difference of evaluated change against a baseline. Policies share the learner and test draws per seed,
 * so the paired standard error is the appropriate one; the unpaired flip threshold stays as a conservative check. */
export function pairedDifferences(episodes: EpisodeResult[], baseline: PolicyName): Paired[] {
  const base = new Map(episodes.filter(e => e.policy === baseline).map(e => [`${e.scenario}\0${e.setting}\0${e.seed}`, e.change]))
  const groups = new Map<string, number[]>()
  for (const e of episodes) {
    if (e.policy === baseline) continue
    const b = base.get(`${e.scenario}\0${e.setting}\0${e.seed}`)
    if (b === undefined) throw new Error(`No ${baseline} episode to pair with ${e.policy} seed ${e.seed}`)
    const key = `${e.scenario}\0${e.setting}\0${e.policy}`
    groups.set(key, [...groups.get(key) ?? [], e.change - b])
  }
  return [...groups].map(([key, diffs]) => { const [scenario, setting, policy] = key.split('\0') as [string, string, PolicyName]
    return { scenario, setting, policy, baseline, diff: summarize(diffs) } })
}
export function rankingFlips(rows: Row[]): Flip[] {
  const flips: Flip[] = []
  for (const scenario of [...new Set(rows.map(r => r.scenario))]) {
    const of = rows.filter(r => r.scenario === scenario)
    const settings = [...new Set(of.map(r => r.setting))], policies = [...new Set(of.map(r => r.policy))]
    const row = (setting: string, policy: PolicyName) => of.find(r => r.setting === setting && r.policy === policy)
    for (let i = 0; i < settings.length; i++) for (let j = i + 1; j < settings.length; j++)
      for (let x = 0; x < policies.length; x++) for (let y = x + 1; y < policies.length; y++) {
        const a = policies[x]!, b = policies[y]!
        const diff = (setting: string) => {
          const ra = row(setting, a), rb = row(setting, b)
          return ra && rb ? { d: ra.change.mean - rb.change.mean, se: Math.sqrt(ra.change.se ** 2 + rb.change.se ** 2) } : null
        }
        const p = diff(settings[i]!), q = diff(settings[j]!)
        if (!p || !q || Math.sign(p.d) * Math.sign(q.d) >= 0) continue
        flips.push({ scenario, a, b, settings: [settings[i]!, settings[j]!], significant: Math.abs(p.d) > 2 * p.se && Math.abs(q.d) > 2 * q.se })
      }
  }
  return flips
}

/** Rule thresholds from adaptive-code-path-plan.md §8, recorded with the results. */
export const RULE_THRESHOLDS = { sufficient: .8, sufficientObservations: 2, low: .5, remediationPerTarget: 2, stalledWrongAnswers: 2 }
type Policy = (input: PolicyInput) => { actionId: string | null; nodes: number; truncated: boolean }
const known = (k: Knowledge[], conceptId: string, level: Difficulty) => k.find(x => x.conceptId === conceptId && x.level === level)
const fixedPolicy = (order: readonly string[]): Policy => input => {
  // The next planned position regardless of answers; an unavailable step is skipped, never replaced adaptively.
  let position = 0
  for (const h of input.history) position = order.indexOf(h.actionId, position) + 1
  const next = order.slice(position).find(id => input.available.some(a => a.id === id && a.cost <= input.remainingBudget))
  return { actionId: next ?? null, nodes: 0, truncated: false }
}
/** Branch table of the rule planner (adaptive-code-path-plan.md §8), using only the planner-visible input. */
const rulePolicy: Policy = ({ goal, belief, remainingBudget, available, history }) => {
  const T = RULE_THRESHOLDS
  const affordable = available.filter(a => a.cost <= remainingBudget)
  const uses = (id: string) => history.filter(h => h.actionId === id).length
  const sufficient = (c: string, l: Difficulty) => mastery(belief, c, l) >= T.sufficient && (known(belief, c, l)?.observations ?? 0) >= T.sufficientObservations
  const pick = (list: LearningAction[]) => [...list].sort((a, b) => uses(a.id) - uses(b.id))[0]?.id ?? null
  const targets = [...goal.targetConcepts].sort((a, b) => b.weight - a.weight).filter(t => t.weight > 0 && !sufficient(t.conceptId, t.level))
  for (const t of targets) {
    const forTarget = affordable.filter(a => a.conceptId === t.conceptId && a.level === t.level)
    const teaching = forTarget.filter(a => a.kind !== 'diagnose')
    const observations = known(belief, t.conceptId, t.level)?.observations ?? 0
    // Unclear state: diagnose with an independent question, or give a short basic explanation.
    if (observations === 0) {
      const diagnose = forTarget.find(a => a.kind === 'diagnose' && a.observation)
      const action = diagnose?.id ?? pick(teaching.filter(a => a.kind === 'read')) ?? pick(teaching)
      if (action) return { actionId: action, nodes: 0, truncated: false }
      continue
    }
    const low = mastery(belief, t.conceptId, t.level) < T.low
    const answers = history.filter(h => h.correct !== undefined && h.conceptId === t.conceptId && h.level === t.level)
    // Target and its prerequisite both weak: remediate the prerequisite (bounded), then return to the target.
    const prerequisites = teaching.flatMap(a => a.effects.flatMap(e => e.prerequisites ?? []))
    const remediated = history.filter(h => prerequisites.some(p => h.conceptId === p.conceptId && h.level === p.level)).length
    if (low && remediated < T.remediationPerTarget) {
      for (const p of prerequisites) if (!sufficient(p.conceptId, p.level) && mastery(belief, p.conceptId, p.level) < T.sufficient) {
        const action = pick(affordable.filter(a => a.conceptId === p.conceptId && a.level === p.level && a.kind !== 'diagnose'))
        if (action) return { actionId: action, nodes: 0, truncated: false }
      }
    }
    // Repeated stall on independent questions: compare with a contrasting example.
    const stalled = answers.slice(-T.stalledWrongAnswers)
    if (stalled.length === T.stalledWrongAnswers && stalled.every(h => h.correct === false)) {
      const compare = pick(teaching.filter(a => a.kind === 'compare'))
      if (compare) return { actionId: compare, nodes: 0, truncated: false }
    }
    // Weak or progressing: reinforce with a different example or explanation (least used first).
    const action = pick(teaching.filter(a => a.kind !== 'compare')) ?? pick(teaching)
    if (action) return { actionId: action, nodes: 0, truncated: false }
  }
  return { actionId: null, nodes: 0, truncated: false }
}
const searchPolicy = (depth: number): Policy => input => {
  const d = chooseAction(input.goal, input.belief, input.available, { depth, maxNodes: 2000, maxMs: 60_000, remainingBudget: input.remainingBudget })
  return { actionId: d.actionId, nodes: d.nodes, truncated: d.truncated }
}
function policyFor(name: PolicyName, scenario: Scenario): Policy {
  if (name === 'fixed') return fixedPolicy([...scenario.fixedOrder])
  if (name === 'rule') return rulePolicy
  return searchPolicy(Number(name.slice(-1)))
}

function trueLearnRate(setting: TrueSetting, scenario: Scenario, action: LearningAction, effect: LearningAction['effects'][number], truth: Record<string, boolean>) {
  const ready = (effect.prerequisites ?? []).every(p => truth[dim(p.conceptId, p.level)])
  const unprepared = setting.prerequisiteMatters && !ready
  let rate = unprepared ? (effect.unpreparedLearn ?? effect.learn) * setting.unpreparedScale : effect.learn * setting.learnScale
  if (setting.hiddenPrerequisites) for (const h of scenario.hiddenPrerequisites ?? [])
    if (h.conceptId === effect.conceptId && h.level === effect.level && !truth[dim(h.requires.conceptId, h.requires.level)]) rate = Math.min(rate, h.unpreparedLearn)
  return Math.min(1, Math.max(0, rate * (setting.kindScale?.[action.kind] ?? 1)))
}
function test(truth: Record<string, boolean>, list: HeldOutItem[], slip: number, random: () => number) {
  const results: Record<string, { correct: number; total: number }> = {}
  for (const item of list) {
    const r = results[dim(item.conceptId, item.level)] ??= { correct: 0, total: 0 }
    const draw = random()
    r.total++
    if (truth[dim(item.conceptId, item.level)] ? draw >= slip : draw < 1 / item.choices) r.correct++
  }
  return results
}

export function runEpisode(scenario: Scenario, setting: TrueSetting, policy: PolicyName, seed: number,
  hooks: { onPolicyInput?: (input: PolicyInput, context: PolicyContext) => void } = {}): EpisodeResult {
  const { goal } = scenario
  const learner = stream(seed, 'learner'), answers = stream(seed, 'answers'), learning = stream(seed, 'learning')
  const dims = [...new Set(scenario.actions.flatMap(a => [dim(a.conceptId, a.level), ...a.effects.flatMap(e => (e.prerequisites ?? []).map(p => dim(p.conceptId, p.level)))]))]
  // Hidden dimensions are drawn after the planner-visible ones so other settings consume the same draws.
  const hiddenDims = (scenario.hiddenPrerequisites ?? []).map(h => dim(h.requires.conceptId, h.requires.level)).filter(d => !dims.includes(d))
  const truth: Record<string, boolean> = Object.fromEntries([...dims, ...new Set(hiddenDims)].map(d => [d, learner() < (scenario.priors[d] ?? 0)]))
  const initialTruth = { ...truth }
  const pre = evalScore(goal, test(truth, scenario.evaluation.pre, setting.slip, stream(seed, 'pre')))
  // The planner starts every session from the same uninformed prior, as the store does.
  let belief: Knowledge[] = [...new Set(dims)].map(d => { const [conceptId, level] = d.split(':') as [string, Difficulty]; return { conceptId, level, probability: PRIOR, observations: 0 } })
  const beliefStart = goalScore(goal, belief)
  const decide = policyFor(policy, scenario)
  const asked = new Set<string>(), history: PolicyInput['history'] = [], steps: string[] = []
  let spent = 0, planningMs = 0, nodes = 0, truncated = 0
  for (let step = 0; ; step++) {
    const fresh = (a: ScenarioAction) => a.questions.find(q => !asked.has(q.id))
    // Mirrors LearningStore.candidates: at most two uses, a fresh question when the action has any, targets first, five candidates.
    const open = scenario.actions.filter(a => steps.filter(s => s === a.id).length < 2 && (a.questions.length === 0 || fresh(a)))
      .sort((a, b) => Number(goal.targetConcepts.some(t => t.conceptId === b.conceptId)) - Number(goal.targetConcepts.some(t => t.conceptId === a.conceptId))).slice(0, 5)
    const available: LearningAction[] = open.map(({ questions: _, ...a }) => {
      const q = fresh(scenario.actions.find(x => x.id === a.id)!)
      return { ...structuredClone(a), ...(q ? { observation: { conceptId: a.conceptId, level: a.level, guess: 1 / q.choices, slip: PLANNER_SLIP } } : {}) }
    })
    const input: PolicyInput = { goal: structuredClone(goal), belief: structuredClone(belief), remainingBudget: goal.budget - spent, available, history: structuredClone(history) }
    hooks.onPolicyInput?.(structuredClone(input), { scenario: scenario.id, setting: setting.name, policy, seed, step })
    const started = performance.now()
    const choice = decide(input)
    planningMs += performance.now() - started; nodes += choice.nodes; truncated += Number(choice.truncated)
    const action = available.find(a => a.id === choice.actionId)
    if (!action || action.cost > goal.budget - spent) break
    const source = scenario.actions.find(a => a.id === action.id)!
    for (const effect of action.effects) {
      const key = dim(effect.conceptId, effect.level)
      if (!truth[key] && learning() < trueLearnRate(setting, scenario, action, effect, truth)) truth[key] = true
    }
    belief = predict(belief, action)
    const q = fresh(source)
    if (q && action.observation) {
      const draw = answers()
      const correct = truth[dim(action.conceptId, action.level)] ? draw >= setting.slip : draw < 1 / q.choices
      belief = observe(belief, action.observation, correct)
      asked.add(q.id); history.push({ actionId: action.id, conceptId: action.conceptId, level: action.level, correct })
    } else history.push({ actionId: action.id, conceptId: action.conceptId, level: action.level })
    spent += action.cost; steps.push(action.id)
  }
  const post = evalScore(goal, test(truth, scenario.evaluation.post, setting.slip, stream(seed, 'post')))
  return { scenario: scenario.id, setting: setting.name, policy, seed, goal: structuredClone(goal), budget: goal.budget, spent, steps, asked: [...asked],
    initialTruth, finalTruth: { ...truth }, pre, post, change: post - pre, gain: normalizedGain(pre, post), preMax: pre >= 1,
    beliefStart, beliefEnd: goalScore(goal, belief), planningMs, nodes, truncated }
}
export function runExperiment(options: { seeds: number; firstSeed?: number; onPolicyInput?: (input: PolicyInput, context: PolicyContext) => void }): Experiment {
  const firstSeed = options.firstSeed ?? 1
  const episodes: EpisodeResult[] = []
  for (const scenario of SCENARIOS) for (const setting of TRUE_SETTINGS) for (let seed = firstSeed; seed < firstSeed + options.seeds; seed++)
    for (const policy of POLICIES) episodes.push(runEpisode(scenario, setting, policy, seed, { onPolicyInput: options.onPolicyInput }))
  const rows = aggregate(episodes)
  return { seeds: options.seeds, firstSeed, episodes, rows, flips: rankingFlips(rows) }
}
const f = (v: number, digits = 3) => Number.isFinite(v) ? v.toFixed(digits) : 'n/a'
export function formatTable(e: Experiment): string {
  const lines = [SIMULATION_DISCLAIMER, `Seeds per (scenario, setting, policy): ${e.seeds} (first seed ${e.firstSeed}). Evaluation: held-out pre/post items never shown to policies.`, '',
    'Assumption settings for the simulated learner:', ...TRUE_SETTINGS.map(s => `  ${s.name}${s.misspecified ? ' [misspecified]' : ' [planner-matched]'}: ${s.description}`),
    `Rule thresholds: ${JSON.stringify(RULE_THRESHOLDS)}`, '']
  for (const scenario of SCENARIOS) {
    lines.push(`Scenario ${scenario.id}: ${scenario.description}; budget ${scenario.goal.budget}`)
    lines.push('| setting | policy | eval change mean ± sd (se) | E_p mean ± sd (n) | pre-max | spent | steps | plan ms/episode | nodes/episode | truncated |')
    lines.push('| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |')
    for (const r of e.rows.filter(r => r.scenario === scenario.id))
      lines.push(`| ${r.setting} | ${r.policy} | ${f(r.change.mean)} ± ${f(r.change.sd)} (${f(r.change.se)}) | ${f(r.gain.mean)} ± ${f(r.gain.sd)} (${r.gain.n}) | ${r.preMaxCount} | ${f(r.spent.mean, 2)} | ${f(r.steps.mean, 2)} | ${f(r.planningMs.mean, 3)} | ${f(r.nodes.mean, 1)} | ${r.truncated} |`)
    lines.push('')
  }
  lines.push('Paired per-seed difference in evaluated change (policy - baseline; same learner and test draws). Primary comparison.')
  lines.push('| scenario | setting | policy | baseline | mean diff | paired se | abs(mean) > 2 paired se | unpaired 2se threshold |')
  lines.push('| --- | --- | --- | --- | --- | --- | --- | --- |')
  for (const baseline of ['fixed', 'rule'] as PolicyName[]) for (const p of pairedDifferences(e.episodes, baseline)) {
    if (baseline === 'rule' && !p.policy.startsWith('search')) continue
    const row = (policy: PolicyName) => e.rows.find(r => r.scenario === p.scenario && r.setting === p.setting && r.policy === policy)!
    const unpaired = 2 * Math.sqrt(row(p.policy).change.se ** 2 + row(baseline).change.se ** 2)
    lines.push(`| ${p.scenario} | ${p.setting} | ${p.policy} | ${baseline} | ${f(p.diff.mean)} | ${f(p.diff.se)} | ${Math.abs(p.diff.mean) > 2 * p.diff.se ? 'yes' : 'no'} | ${f(unpaired)} |`)
  }
  lines.push('')
  lines.push('Ranking flips across assumption settings (evaluated change; significant = |difference| > 2 se in both settings):')
  if (!e.flips.length) lines.push('  none')
  for (const x of e.flips) lines.push(`  ${x.scenario}: ${x.a} vs ${x.b} between ${x.settings[0]} and ${x.settings[1]}${x.significant ? ' [significant]' : ' [within noise]'}`)
  return lines.join('\n')
}
