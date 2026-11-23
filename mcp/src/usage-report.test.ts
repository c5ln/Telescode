import { test } from 'node:test'
import assert from 'node:assert/strict'
import { compare, parseLog, summarize, table, type UsageLine } from './usage-report.ts'

const run = (over: Partial<UsageLine> & { input?: number; cached?: number; output?: number; saved?: number | null; reported?: boolean } = {}): UsageLine => ({
  work: 'quiz', provider: 'claude', model: 'light', durationMs: 1000, ok: true, scenario: 'baseline',
  usage: { inputTokens: over.input ?? 100, cachedInputTokens: over.cached ?? 1000, outputTokens: over.output ?? 50 },
  metrics: { readyMs: 300, firstToolCallMs: 500, modelCalls: 3, toolCalls: [{ tool: 'get_quiz_context', resultBytes: 400 }, { tool: 'create_quiz', resultBytes: 20 }],
    savedQuestions: over.saved === undefined ? 2 : over.saved, usageReported: over.reported ?? true },
  ...over,
})

test('the log is read line by line, skipping what is not a run', () => {
  const text = [JSON.stringify(run()), '', 'not json', '{"work":"quiz"}', JSON.stringify(run({ work: 'learning' }))].join('\r\n')
  assert.deepEqual(parseLog(text).map((r) => r.work), ['quiz', 'learning'])
})

test('runs are grouped by scenario, work, provider and model with means per run', () => {
  const [learning, quiz] = summarize([run({ durationMs: 1000 }), run({ durationMs: 3000, input: 300 }), run({ durationMs: 2000 }), run({ work: 'learning' })])
  assert.equal(learning!.key, 'baseline learning/claude/light')
  assert.equal(quiz!.key, 'baseline quiz/claude/light')
  assert.equal(quiz!.runs, 3)
  assert.equal(quiz!.medianDurationMs, 2000)
  assert.equal(quiz!.meanInputTokens, (100 + 300 + 100) / 3)
  assert.equal(quiz!.meanToolCalls, 2)
  assert.equal(quiz!.meanToolResultBytes, 420)
  assert.equal(quiz!.savedQuestions, 6)
  // New input and output tokens, not cached input, per saved question.
  assert.equal(quiz!.tokensPerQuestion, (500 + 150) / 6)
})

test('what a run did not report stays unknown instead of counting as zero', () => {
  const old = { ...run(), metrics: undefined }
  const [s] = summarize([run({ saved: null, reported: false }), old])
  assert.equal(s!.savedQuestions, null)
  assert.equal(s!.tokensPerQuestion, null)
  assert.equal(s!.unknownUsage, 2)
  // Means are over the runs that reported a value.
  assert.equal(s!.meanModelCalls, 3)
  // Their zero token counts are not measurements.
  assert.equal(s!.meanInputTokens, null)
  const [mixed] = summarize([run({ input: 100 }), run({ input: 0, reported: false })])
  assert.equal(mixed!.meanInputTokens, 100)
  assert.equal(mixed!.tokensPerQuestion, null)
  const codex = summarize([run({ provider: 'codex', metrics: { ...run().metrics!, modelCalls: null } })])[0]!
  assert.equal(codex.meanModelCalls, null)
  assert.match(table([codex]), /\| unknown \|/)
})

test('a run with a tool result of unknown size has no result size total', () => {
  const cut = run({ metrics: { ...run().metrics!, toolCalls: [{ tool: 'get_quiz_context', resultBytes: 400 }, { tool: 'create_quiz', resultBytes: null }] } })
  assert.equal(summarize([run(), cut])[0]!.meanToolResultBytes, 420)
  assert.equal(summarize([cut])[0]!.meanToolResultBytes, null)
})

test('before and after are compared per work, provider and model whatever their scenario names', () => {
  const out = compare([run({ scenario: 'before', input: 200 })], [run({ scenario: 'after', input: 100 }), run({ work: 'learning', scenario: 'after' })])
  assert.match(out, /\| quiz\/claude\/light \| new input \| 200 \| 100 \(-50\.0%\) \|/)
  assert.match(out, /\| learning\/claude\/light \| runs \| no runs \| 1 \|/)
})
