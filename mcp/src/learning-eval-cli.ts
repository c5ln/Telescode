/** Prints the simulated policy comparison. Usage: npm run eval:learning -- [--seeds N] [--first-seed S] */
import { formatTable, runExperiment } from './learning-eval.ts'

const arg = (name: string, fallback: number) => {
  const i = process.argv.indexOf(name)
  const value = i >= 0 ? Number(process.argv[i + 1]) : fallback
  if (!Number.isInteger(value) || value < 1) throw new Error(`${name} must be a positive integer`)
  return value
}
console.log(formatTable(runExperiment({ seeds: arg('--seeds', 500), firstSeed: arg('--first-seed', 1) })))
