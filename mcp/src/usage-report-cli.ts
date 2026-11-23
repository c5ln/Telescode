/** Prints the usage log summary, or before and after side by side; see usage-report.ts for usage. */
import { readFileSync } from 'node:fs'
import { compare, parseLog, summarize, table } from './usage-report.ts'

const args = process.argv.slice(2)
const flag = (name: string) => {
  const i = args.indexOf(name)
  return i >= 0 ? args[i + 1] : undefined
}
const load = (path: string, scenario?: string) => parseLog(readFileSync(path, 'utf8')).filter((r) => !scenario || r.scenario === scenario)
const before = flag('--before')
const after = flag('--after')
const log = args.find((a, i) => !a.startsWith('--') && !args[i - 1]?.startsWith('--'))
if (before && after) console.log(compare(load(before, flag('--before-scenario')), load(after, flag('--after-scenario'))))
else if (log) console.log(table(summarize(load(log, flag('--scenario')))))
else throw new Error('usage: usage-report <log> [--scenario <name>] | --before <log> --after <log>')
