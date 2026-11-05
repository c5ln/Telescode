import { test } from 'node:test'
import assert from 'node:assert/strict'
import { aggregate, evalScore, normalizedGain, pairedDifferences, rankingFlips, runEpisode, runExperiment, summarize, POLICIES, SCENARIOS, TRUE_SETTINGS,
  type EpisodeResult, type PolicyContext, type PolicyInput, type Row } from './learning-eval.ts'
import type { LearningGoal } from '../../frontend/src/learning/model.ts'

const near = (actual: number | null, expected: number) => assert.ok(actual !== null && Math.abs(actual - expected) < 1e-10, `${actual} != ${expected}`)
const withoutTiming = (episodes: EpisodeResult[]) => episodes.map(({ planningMs: _, ...rest }) => rest)

test('experiment is deterministic under a seed and covers every scenario, assumption setting and policy', () => {
  assert.deepEqual(POLICIES, ['fixed', 'rule', 'search-d1', 'search-d2', 'search-d3'])
  assert.deepEqual(TRUE_SETTINGS.map(s => s.name), ['conservative', 'default', 'optimistic', 'hidden-prerequisite'])
  assert.ok(SCENARIOS.length >= 2)
  const a = runExperiment({ seeds: 12 }), b = runExperiment({ seeds: 12 })
  assert.equal(a.episodes.length, 12 * SCENARIOS.length * TRUE_SETTINGS.length * POLICIES.length)
  assert.deepEqual(withoutTiming(a.episodes), withoutTiming(b.episodes))
  assert.notDeepEqual(withoutTiming(runExperiment({ seeds: 12, firstSeed: 1000 }).episodes), withoutTiming(a.episodes))
})

test('policies are compared on the same goal, budget, concept map, simulated learner and pre-test', () => {
  const { episodes } = runExperiment({ seeds: 10 })
  const groups = new Map<string, EpisodeResult[]>()
  for (const e of episodes) groups.set(`${e.scenario}|${e.setting}|${e.seed}`, [...groups.get(`${e.scenario}|${e.setting}|${e.seed}`) ?? [], e])
  assert.ok(groups.size > 0)
  for (const group of groups.values()) {
    assert.deepEqual(group.map(e => e.policy).sort(), [...POLICIES].sort())
    const scenario = SCENARIOS.find(s => s.id === group[0]!.scenario)!
    for (const e of group) {
      assert.deepEqual(e.goal, scenario.goal)
      assert.equal(e.budget, scenario.goal.budget)
      assert.ok(e.spent <= e.budget, `${e.policy} spent ${e.spent} over budget ${e.budget}`)
      assert.deepEqual(e.initialTruth, group[0]!.initialTruth)
      assert.equal(e.pre, group[0]!.pre)
      assert.equal(new Set(e.asked).size, e.asked.length, `${e.policy} observed a training question twice`)
    }
  }
})

test('held-out pre/post items never reach a policy and are disjoint from training questions', () => {
  const inputs: string[] = []
  const first = new Map<string, PolicyInput[]>()
  runExperiment({ seeds: 6, onPolicyInput: (input: PolicyInput, at: PolicyContext) => {
    inputs.push(JSON.stringify(input))
    // Pre-test outcomes differ by seed; if they fed the belief, the first decision's input would too.
    if (at.step === 0) first.set(`${at.scenario}|${at.setting}|${at.policy}`, [...first.get(`${at.scenario}|${at.setting}|${at.policy}`) ?? [], structuredClone(input)])
  } })
  assert.ok(inputs.length > 0, 'policies must be observed deciding')
  assert.ok(first.size > 0)
  for (const [key, starts] of first) {
    assert.equal(starts.length, 6, `${key} decides once per seed`)
    for (const start of starts) {
      assert.deepEqual(start.history, [], `${key} starts without evaluation history`)
      assert.deepEqual(start.belief, starts[0]!.belief, `${key} belief must not depend on the seed's pre-test`)
    }
  }
  assert.ok(SCENARIOS.length > 0)
  for (const s of SCENARIOS) {
    const heldOut = [...s.evaluation.pre, ...s.evaluation.post]
    assert.ok(s.evaluation.pre.length > 0 && s.evaluation.post.length > 0)
    for (const t of s.goal.targetConcepts) {
      assert.ok(s.evaluation.pre.some(i => i.conceptId === t.conceptId && i.level === t.level), `pre-test covers ${t.conceptId}`)
      assert.ok(s.evaluation.post.some(i => i.conceptId === t.conceptId && i.level === t.level), `post-test covers ${t.conceptId}`)
    }
    const preIds = new Set(s.evaluation.pre.map(i => i.id))
    assert.ok(s.evaluation.post.every(i => !preIds.has(i.id)), 'pre and post use different items')
    const training = s.actions.flatMap(a => a.questions)
    for (const item of heldOut) {
      assert.ok(training.every(q => q.id !== item.id && q.text !== item.text), `${item.id} is not a training question`)
      assert.ok(inputs.every(i => !i.includes(item.id) && !i.includes(item.text)), `${item.id} leaked to a policy`)
    }
  }
})

test('estimate inflation is not success: with no true learning the evaluated change is zero while the planner belief rises', () => {
  const inert = { name: 'inert', description: 'test-only learner that never learns', learnScale: 0, unpreparedScale: 0, prerequisiteMatters: true, slip: .1, misspecified: true }
  const scenario = SCENARIOS[0]!
  const changes: number[] = [], beliefGains: number[] = []
  for (let seed = 1; seed <= 200; seed++) {
    const e = runEpisode(scenario, inert, 'search-d3', seed)
    assert.deepEqual(e.finalTruth, e.initialTruth)
    changes.push(e.change); beliefGains.push(e.beliefEnd - e.beliefStart)
  }
  const change = summarize(changes), belief = summarize(beliefGains)
  assert.ok(Math.abs(change.mean) <= 3 * change.se, `evaluated change ${change.mean} must be noise around zero (se ${change.se})`)
  assert.ok(belief.mean > change.mean + .1, `the planner's own estimate rises (${belief.mean}) but is not the success metric`)
  assert.ok(TRUE_SETTINGS.filter(s => s.misspecified).length >= 2, 'conservative and optimistic settings must not match the planner assumptions')
})

test('evaluation score, KnowLP E_p and summaries match hand-computed values', () => {
  const goal: LearningGoal = { goalId: 'g', goalRevision: 1, rawQuery: 'q', retrievalQuery: 'q', repositoryId: 'r', sourceSnapshotHash: 's', budget: 3,
    targetConcepts: [{ conceptId: 'A', level: 'hard', weight: 3 }, { conceptId: 'B', level: 'easy', weight: 1 }] }
  near(evalScore(goal, { 'A:hard': { correct: 1, total: 4 }, 'B:easy': { correct: 3, total: 4 } }), (3 * .25 + .75) / 4)
  near(normalizedGain(.25, .75), 2 / 3)
  near(normalizedGain(.5, .25), -.5)
  assert.equal(normalizedGain(1, 1), null, 'pre-max cases are separated, not divided by zero')
  const s = summarize([1, 2, 3, 4])
  assert.equal(s.n, 4); near(s.mean, 2.5); near(s.sd, Math.sqrt(5 / 3)); near(s.se, Math.sqrt(5 / 3) / 2)
  const episode = (seed: number, pre: number, post: number): EpisodeResult => ({ scenario: 'x', setting: 'default', policy: 'rule', seed, goal, budget: 3,
    spent: 2, steps: ['a', 'b'], asked: [], initialTruth: {}, finalTruth: {}, pre, post, change: post - pre, gain: normalizedGain(pre, post), preMax: pre >= 1,
    beliefStart: 0, beliefEnd: 0, planningMs: 1, nodes: 4, truncated: 0 })
  const [row] = aggregate([episode(1, .5, 1), episode(2, .25, .25), episode(3, 1, .75)])
  assert.equal(row!.seeds, 3)
  assert.equal(row!.preMaxCount, 1)
  assert.equal(row!.change.n, 3); near(row!.change.mean, (.5 + 0 - .25) / 3)
  assert.equal(row!.gain.n, 2); near(row!.gain.mean, (1 + 0) / 2)
})

test('policy ranking flips across assumption settings are flagged, consistent orders are not', () => {
  const stat = (mean: number, se = .01) => ({ n: 100, mean, sd: se * 10, se })
  const row = (setting: string, policy: Row['policy'], mean: number): Row => ({ scenario: 'x', setting, policy, seeds: 100, change: stat(mean),
    gain: stat(mean), preMaxCount: 0, spent: stat(2), steps: stat(2), planningMs: stat(0), nodes: stat(0), truncated: 0 })
  const flips = rankingFlips([row('conservative', 'fixed', .3), row('conservative', 'rule', .2), row('conservative', 'search-d3', .1),
    row('optimistic', 'fixed', .1), row('optimistic', 'rule', .25), row('optimistic', 'search-d3', .2)])
  const pairs = flips.map(f => [f.a, f.b].sort().join('/')).sort()
  assert.deepEqual(pairs, ['fixed/rule', 'fixed/search-d3'])
  assert.ok(flips.every(f => f.significant))
  const noisy = rankingFlips([row('conservative', 'fixed', .3), row('conservative', 'rule', .29), row('optimistic', 'fixed', .29), row('optimistic', 'rule', .3)])
  assert.equal(noisy.length, 1)
  assert.equal(noisy[0]!.significant, false, 'a flip within sampling noise is reported but not called significant')
})

test('hidden prerequisite setting slows true learning through a dependency absent from every policy input', () => {
  const hidden = TRUE_SETTINGS.find(s => s.name === 'hidden-prerequisite')
  const matched = TRUE_SETTINGS.find(s => s.name === 'default')
  assert.ok(hidden?.misspecified && matched)
  const { name: _h, description: _hd, misspecified: _hm, hiddenPrerequisites, ...hiddenRates } = hidden!
  const { name: _m, description: _md, misspecified: _mm, hiddenPrerequisites: matchedHidden, ...matchedRates } = matched!
  assert.deepEqual(hiddenRates, matchedRates, 'only the hidden dependency differs from the planner-matched setting')
  assert.equal(hiddenPrerequisites, true); assert.ok(!matchedHidden)
  for (const scenario of SCENARIOS) {
    const unknown = (scenario.hiddenPrerequisites ?? []).map(h => h.requires)
    assert.ok(unknown.length > 0, `${scenario.id} declares a hidden prerequisite`)
    const inputs: string[] = []
    let hiddenLearned = 0, matchedLearned = 0
    for (let seed = 1; seed <= 150; seed++) {
      const target = (e: EpisodeResult) => scenario.goal.targetConcepts.filter(t => !e.initialTruth[`${t.conceptId}:${t.level}`] && e.finalTruth[`${t.conceptId}:${t.level}`]).length
      hiddenLearned += target(runEpisode(scenario, hidden!, 'fixed', seed, { onPolicyInput: input => { inputs.push(JSON.stringify(input)) } }))
      matchedLearned += target(runEpisode(scenario, matched!, 'fixed', seed))
    }
    assert.ok(hiddenLearned < matchedLearned, `${scenario.id}: hidden dependency must reduce true learning (${hiddenLearned} vs ${matchedLearned})`)
    for (const h of scenario.hiddenPrerequisites ?? []) {
      assert.ok(!scenario.actions.some(a => a.effects.some(e => e.conceptId === h.conceptId && e.level === h.level &&
        (e.prerequisites ?? []).some(p => p.conceptId === h.requires.conceptId && p.level === h.requires.level))), 'the planner must not know this dependency')
      if (!scenario.actions.some(a => a.conceptId === h.requires.conceptId || a.effects.some(e => e.conceptId === h.requires.conceptId)))
        assert.ok(inputs.every(i => !i.includes(`"${h.requires.conceptId}"`)), `unmodeled concept ${h.requires.conceptId} must not reach a policy`)
    }
  }
})

test('paired differences against a baseline match by seed and use the per-seed difference variance', () => {
  const e = (policy: EpisodeResult['policy'], seed: number, change: number) => ({ scenario: 'x', setting: 'default', policy, seed, change }) as EpisodeResult
  // Deliberately out of order: pairing is by seed, not by position.
  const rows = pairedDifferences([e('search-d3', 3, .1), e('fixed', 1, .3), e('search-d3', 1, .5), e('fixed', 3, .4), e('search-d3', 2, .2), e('fixed', 2, .2)], 'fixed')
  assert.equal(rows.length, 1, 'only non-baseline policies are compared')
  const [row] = rows
  assert.equal(row!.policy, 'search-d3'); assert.equal(row!.baseline, 'fixed')
  assert.equal(row!.diff.n, 3)
  near(row!.diff.mean, -.1 / 3)
  near(row!.diff.sd, Math.sqrt(.57 / 9))
  near(row!.diff.se, Math.sqrt(.57 / 9) / Math.sqrt(3))
  assert.throws(() => pairedDifferences([e('search-d3', 1, .5), e('fixed', 2, .3)], 'fixed'), /pair|seed/i)
})
