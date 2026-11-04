import { test } from 'node:test'
import assert from 'node:assert/strict'
import { chooseAction, goalScore, observe, predict, validateGoal,
  type LearningGoal, type Knowledge, type LearningAction } from '../../frontend/src/learning/model.ts'

const near = (actual: number, expected: number) => assert.ok(Math.abs(actual - expected) < 1e-10, `${actual} != ${expected}`)
const goal = (targets: LearningGoal['targetConcepts'], budget = 3): LearningGoal => ({
  goalId: 'goal', goalRevision: 1, rawQuery: '재시도가 안전한 조건 이해', retrievalQuery: 'safe request retry',
  repositoryId: 'repo', sourceSnapshotHash: 'source-v1', targetConcepts: targets, budget,
})
const skill = (conceptId: string, probability: number, level: Knowledge['level'] = 'easy'): Knowledge => ({ conceptId, level, probability, observations: 0 })
const teach = (id: string, conceptId: string, cost = 1, learn = 1): LearningAction => ({ id, kind: 'read', nodeId: `file:${id}.py`,
  conceptId, level: 'easy', cost, caption: `Understand ${conceptId}`, effects: [{ conceptId, level: 'easy', learn }] })

test('objective weights goal concepts at their required depth, excluding unrelated and easier mastery', () => {
  const g = goal([{ conceptId: 'A', level: 'hard', weight: 3 }, { conceptId: 'B', level: 'easy', weight: 1 }])
  const knowledge = [skill('A', .2, 'hard'), skill('A', 1), skill('B', .8), skill('unrelated', 1)]
  near(goalScore(g, knowledge), .35)
  assert.deepEqual(knowledge.map(s => s.probability), [.2, 1, .8, 1])
})

test('goal contract rejects empty/zero/negative weights and non-finite or negative budgets', () => {
  for (const g of [goal([]), goal([{ conceptId: 'A', level: 'easy', weight: 0 }]),
    goal([{ conceptId: 'A', level: 'easy', weight: -1 }]), goal([{ conceptId: 'A', level: 'easy', weight: NaN }]),
    goal([{ conceptId: 'A', level: 'easy', weight: 1 }], -1), goal([{ conceptId: 'A', level: 'easy', weight: 1 }], Infinity)]) {
    assert.throws(() => validateGoal(g), /goal|weight|budget/i)
  }
})

test('independent hard observation changes only that concept and depth', () => {
  const before = [skill('A', .4, 'hard'), skill('A', .7), skill('B', .3, 'hard')]
  const after = observe(before, { conceptId: 'A', level: 'hard', guess: .25, slip: .1 }, false)
  near(after[0]!.probability, .04 / (.04 + .45))
  assert.equal(after[0]!.observations, 1)
  assert.deepEqual(after.slice(1), before.slice(1))
  assert.equal(before[0]!.observations, 0)
})

test('pure diagnosis preserves expected mastery rather than granting learning for answering', () => {
  const before = [skill('A', .4)]
  const observation = { conceptId: 'A', level: 'easy' as const, guess: .25, slip: .1 }
  const pCorrect = .4 * .9 + .6 * .25
  const correct = observe(before, observation, true)
  const incorrect = observe(before, observation, false)
  near(pCorrect * correct[0]!.probability + (1 - pCorrect) * incorrect[0]!.probability, .4)
  assert.ok(correct[0]!.probability > .4)
  assert.ok(incorrect[0]!.probability < .4)
})

test('teaching transition affects its mapped depth and depends on prerequisite readiness', () => {
  const action = teach('A', 'A', 1, .9)
  action.effects[0]!.prerequisites = [{ conceptId: 'B', level: 'easy' }]
  action.effects[0]!.unpreparedLearn = .1
  near(predict([skill('A', 0), skill('B', 0), skill('A', .8, 'hard')], action)[0]!.probability, .1)
  const prepared = predict([skill('A', 0), skill('B', 1), skill('A', .8, 'hard')], action)
  near(prepared[0]!.probability, .9)
  near(prepared[2]!.probability, .8)
  const diagnostic = { ...action, kind: 'diagnose' as const, effects: [] }
  assert.deepEqual(predict(prepared, diagnostic), prepared)
})

test('two-step optimization selects prerequisite B then target A, and skips B when already known', () => {
  const g = goal([{ conceptId: 'A', level: 'easy', weight: 1 }], 2)
  const target = teach('target-A', 'A', 1, .9)
  target.effects[0]!.prerequisites = [{ conceptId: 'B', level: 'easy' }]
  target.effects[0]!.unpreparedLearn = .1
  const actions = [target, teach('prerequisite-B', 'B')]
  const decision = chooseAction(g, [skill('A', 0), skill('B', 0)], actions, { depth: 2, maxNodes: 100 })
  assert.equal(decision.actionId, 'prerequisite-B')
  near(decision.value, .9)
  const afterB = predict([skill('A', 0), skill('B', 0)], actions[1]!)
  const returned = chooseAction(g, afterB, [target], { depth: 2, maxNodes: 100, remainingBudget: 1 })
  assert.equal(returned.actionId, 'target-A')
  near(returned.value, .9)
  assert.equal(chooseAction(g, [skill('A', 0), skill('B', 1)], actions, { depth: 2, maxNodes: 100 }).actionId, 'target-A')
  const oneStep = chooseAction(g, [skill('A', 0), skill('B', 0)], actions, { depth: 2, maxNodes: 100, remainingBudget: 1 })
  assert.equal(oneStep.actionId, 'target-A')
  near(oneStep.value, .1)
})

test('optimizer rejects invalid probabilities, action costs and duplicate target dimensions', () => {
  const g = goal([{ conceptId: 'A', level: 'easy', weight: 1 }])
  const before = [skill('A', .5)]
  const options = { depth: 2, maxNodes: 100 }
  for (const cost of [-1, 0, Infinity, NaN]) assert.throws(() => chooseAction(g, before, [{ ...teach('a', 'A'), cost }], options), /cost/i)
  for (const probability of [-.1, 1.1, NaN]) {
    const a = teach('a', 'A', 1, probability)
    assert.throws(() => chooseAction(g, before, [a], options), /probability|learn/i)
    for (const field of ['guess', 'slip'] as const) {
      const diagnostic: LearningAction = { ...teach('diagnostic', 'A'), kind: 'diagnose', effects: [],
        observation: { conceptId: 'A', level: 'easy', guess: .25, slip: .1, [field]: probability } }
      assert.throws(() => chooseAction(g, before, [diagnostic], options), /probability|guess|slip/i)
    }
  }
  assert.throws(() => validateGoal(goal([...g.targetConcepts, ...g.targetConcepts])), /duplicate/i)
  assert.throws(() => validateGoal(goal([{ conceptId: 'A', level: 'easy', weight: Number.MAX_VALUE },
    { conceptId: 'B', level: 'easy', weight: Number.MAX_VALUE }])), /weight/i)
})

test('optimizer refuses an unsupported candidate set rather than silently hiding the useful sixth action', () => {
  const g = goal([{ conceptId: 'A', level: 'easy', weight: 1 }])
  const actions = Array.from({ length: 5 }, (_, i) => teach(`unrelated-${i}`, `unrelated-${i}`))
  actions.push(teach('target-A', 'A'))
  assert.throws(() => chooseAction(g, [skill('A', 0)], actions, { depth: 2, maxNodes: 100 }), /candidate/i)
})

test('diagnostic is valuable through answer-dependent teaching: hand-computed expected score .83125', () => {
  const g = goal([{ conceptId: 'X', level: 'easy', weight: 1 }, { conceptId: 'Y', level: 'easy', weight: 1 }], 3)
  const diagnostic: LearningAction = { ...teach('diagnose-X', 'X'), kind: 'diagnose', effects: [],
    observation: { conceptId: 'X', level: 'easy', guess: .25, slip: .1 } }
  const actions = [teach('teach-X', 'X', 2), teach('teach-Y', 'Y', 2), diagnostic]
  // P(correct)=.575; posteriors X=.45/.575 or .05/.425. Teach Y after
  // correct and X after incorrect: .575*(.45/.575+1)/2 + .425*(1+.5)/2.
  const decision = chooseAction(g, [skill('X', .5), skill('Y', .5)], actions, { depth: 2, maxNodes: 200 })
  assert.equal(decision.actionId, 'diagnose-X')
  near(decision.value, .83125)
  near(decision.expectedGain, .33125)
  assert.equal(decision.branches?.correct.actionId, 'teach-Y')
  assert.equal(decision.branches?.incorrect.actionId, 'teach-X')
  const shortBudget = chooseAction(g, [skill('X', .5), skill('Y', .5)], actions, { depth: 2, maxNodes: 200, remainingBudget: 2 })
  assert.ok(['teach-X', 'teach-Y'].includes(shortBudget.actionId!))
  near(shortBudget.value, .75)
})

test('stops at zero budget or no expected gain and respects bounded expansion without mutating inputs', () => {
  const g = goal([{ conceptId: 'A', level: 'easy', weight: 1 }])
  const skills = [skill('A', .5)]
  const actions = [teach('A', 'A')]
  const before = structuredClone({ g, skills, actions })
  assert.equal(chooseAction(g, skills, actions, { depth: 3, maxNodes: 100, remainingBudget: 0 }).actionId, null)
  near(chooseAction(g, skills, actions, { depth: 3, maxNodes: 100, remainingBudget: 0 }).value, .5)
  assert.equal(chooseAction(g, [skill('A', 1)], actions, { depth: 3, maxNodes: 100 }).actionId, null)
  const limited = chooseAction(g, skills, actions, { depth: 3, maxNodes: 1 })
  assert.ok(limited.nodes <= 1)
  assert.equal(limited.truncated, true)
  assert.deepEqual({ g, skills, actions }, before)
})
