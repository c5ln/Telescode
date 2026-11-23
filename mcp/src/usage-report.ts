// Summarizes the app's agent usage log (artifacts/agent-usage.jsonl, one line
// per run, written by agent.rs) so runs before and after a change can be
// compared. Reads only; it never starts an agent.
//
//   npm --prefix mcp run usage:report -- <log> [--scenario <name>]
//   npm --prefix mcp run usage:report -- --before <log> [--before-scenario <name>] --after <log> [--after-scenario <name>]
//
// A value the log has as null (the agent's stream did not report it) is left
// out of averages and counted as unknown; it is never taken as zero.
export interface UsageLine {
  runId?: string
  work: string
  provider: string
  model: string
  durationMs: number
  ok: boolean
  scenario?: string | null
  usage: { inputTokens: number; cachedInputTokens: number; outputTokens: number }
  metrics?: {
    readyMs: number | null
    firstToolCallMs: number | null
    modelCalls: number | null
    toolCalls: { tool: string; resultBytes: number | null }[]
    savedQuestions: number | null
    usageReported: boolean
  }
}

/** One group of runs: same scenario, work, provider and model. */
export interface Summary {
  key: string
  runs: number
  ok: number
  medianDurationMs: number
  meanDurationMs: number
  /** Over the runs whose stream reported usage; `null` when none did. */
  meanInputTokens: number | null
  meanCachedInputTokens: number | null
  meanOutputTokens: number | null
  /** Over the runs that reported it; `null` when none did. */
  meanReadyMs: number | null
  meanModelCalls: number | null
  meanToolCalls: number | null
  meanToolResultBytes: number | null
  /** `null` when any run's count is unknown. */
  savedQuestions: number | null
  /** New input and output tokens per saved question; `null` when any run's usage or count is not known. */
  tokensPerQuestion: number | null
  /** Runs whose stream did not report usage, or that predate the metrics. */
  unknownUsage: number
}

/** The log's lines, skipping any that are not a run. */
export function parseLog(text: string): UsageLine[] {
  return text.split(/\r?\n/).flatMap((line) => {
    if (!line.trim()) return []
    try {
      const run = JSON.parse(line)
      return typeof run?.work === 'string' && run.usage ? [run as UsageLine] : []
    } catch {
      return []
    }
  })
}

const mean = (xs: number[]) => (xs.length ? xs.reduce((a, b) => a + b, 0) / xs.length : 0)
const known = (xs: (number | null | undefined)[]) => xs.filter((x): x is number => typeof x === 'number')
const meanKnown = (xs: (number | null | undefined)[]) => (known(xs).length ? mean(known(xs)) : null)

function median(xs: number[]) {
  const sorted = [...xs].sort((a, b) => a - b)
  const mid = sorted.length >> 1
  return sorted.length % 2 ? sorted[mid]! : (sorted[mid - 1]! + sorted[mid]!) / 2
}

/** `work/provider/model`, after the scenario when `withScenario`. */
export function keyOf(run: UsageLine, withScenario = true) {
  const base = `${run.work}/${run.provider}/${run.model}`
  return withScenario ? `${run.scenario ?? '-'} ${base}` : base
}

export function summarize(runs: UsageLine[], withScenario = true): Summary[] {
  const groups = new Map<string, UsageLine[]>()
  for (const run of runs) groups.set(keyOf(run, withScenario), [...(groups.get(keyOf(run, withScenario)) ?? []), run])
  return [...groups].sort(([a], [b]) => a.localeCompare(b)).map(([key, group]) => {
    const saved = group.map((r) => r.metrics?.savedQuestions)
    const savedQuestions = saved.every((s) => typeof s === 'number') ? (saved as number[]).reduce((a, b) => a + b, 0) : null
    // A run that did not report usage has zeros in its counts, not measurements.
    const reported = (get: (u: UsageLine['usage']) => number) => group.map((r) => (r.metrics?.usageReported ? get(r.usage) : null))
    const fresh = known(reported((u) => u.inputTokens + u.outputTokens))
    // A run's result bytes are known only when every call's are.
    const resultBytes = (r: UsageLine) => {
      const sizes = r.metrics?.toolCalls.map((c) => c.resultBytes)
      return sizes && sizes.every((b) => typeof b === 'number') ? (sizes as number[]).reduce((a, b) => a + b, 0) : null
    }
    return {
      key,
      runs: group.length,
      ok: group.filter((r) => r.ok).length,
      medianDurationMs: median(group.map((r) => r.durationMs)),
      meanDurationMs: mean(group.map((r) => r.durationMs)),
      meanInputTokens: meanKnown(reported((u) => u.inputTokens)),
      meanCachedInputTokens: meanKnown(reported((u) => u.cachedInputTokens)),
      meanOutputTokens: meanKnown(reported((u) => u.outputTokens)),
      meanReadyMs: meanKnown(group.map((r) => r.metrics?.readyMs)),
      meanModelCalls: meanKnown(group.map((r) => r.metrics?.modelCalls)),
      meanToolCalls: meanKnown(group.map((r) => r.metrics?.toolCalls.length)),
      meanToolResultBytes: meanKnown(group.map(resultBytes)),
      savedQuestions,
      tokensPerQuestion: savedQuestions && fresh.length === group.length ? fresh.reduce((a, b) => a + b, 0) / savedQuestions : null,
      unknownUsage: group.filter((r) => !r.metrics?.usageReported).length,
    }
  })
}

const show = (n: number | null) => (n === null ? 'unknown' : Number.isInteger(n) ? String(n) : n.toFixed(1))

const COLUMNS: [string, (s: Summary) => number | null][] = [
  ['runs', (s) => s.runs],
  ['ok', (s) => s.ok],
  ['median ms', (s) => s.medianDurationMs],
  ['ready ms', (s) => s.meanReadyMs],
  ['new input', (s) => s.meanInputTokens],
  ['cached input', (s) => s.meanCachedInputTokens],
  ['output', (s) => s.meanOutputTokens],
  ['model calls', (s) => s.meanModelCalls],
  ['tool calls', (s) => s.meanToolCalls],
  ['tool result bytes', (s) => s.meanToolResultBytes],
  ['saved questions', (s) => s.savedQuestions],
  ['tokens / question', (s) => s.tokensPerQuestion],
  ['usage unknown', (s) => s.unknownUsage],
]

/** A Markdown table, one row per group; token, time and call columns are means per run. */
export function table(summaries: Summary[]) {
  const rows = summaries.map((s) => `| ${s.key} | ${COLUMNS.map(([, get]) => show(get(s))).join(' | ')} |`)
  return [`| group | ${COLUMNS.map(([name]) => name).join(' | ')} |`, `|${' --- |'.repeat(COLUMNS.length + 1)}`, ...rows].join('\n')
}

/** Before and after side by side for each work/provider/model both have, with the change in percent. */
export function compare(before: UsageLine[], after: UsageLine[]) {
  const a = new Map(summarize(before, false).map((s) => [s.key, s]))
  const b = new Map(summarize(after, false).map((s) => [s.key, s]))
  const keys = [...new Set([...a.keys(), ...b.keys()])].sort()
  const change = (x: number | null, y: number | null) => (x === null || y === null || x === 0 ? '' : ` (${y >= x ? '+' : ''}${(((y - x) / x) * 100).toFixed(1)}%)`)
  const lines = ['| group | metric | before | after |', '| --- | --- | --- | --- |']
  for (const key of keys) {
    const x = a.get(key)
    const y = b.get(key)
    for (const [name, get] of COLUMNS) {
      const before = x ? get(x) : null
      const after = y ? get(y) : null
      lines.push(`| ${key} | ${name} | ${x ? show(before) : 'no runs'} | ${y ? show(after) + change(before, after) : 'no runs'} |`)
    }
  }
  return lines.join('\n')
}
