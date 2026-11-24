/** Measures the agent contexts of one checkout, or compares two measurements; see context-size.ts for usage. */
import { mkdtemp, readFile, rm, writeFile } from 'node:fs/promises'
import { tmpdir } from 'node:os'
import { join, resolve } from 'node:path'
import { fileURLToPath, pathToFileURL } from 'node:url'
import { compare, measure, type Example, type Modules } from './context-size.ts'

const args = process.argv.slice(2)
const flag = (name: string) => {
  const i = args.indexOf(name)
  return i >= 0 ? args[i + 1] : undefined
}
const here = fileURLToPath(new URL('../..', import.meta.url))
const before = flag('--before')
const after = flag('--after')
const db = flag('--db')
const name = flag('--example')

if (before && after) {
  console.log(compare(JSON.parse(await readFile(before, 'utf8')), JSON.parse(await readFile(after, 'utf8'))))
} else if (db && name) {
  // The code measured may be another checkout's; the example is always this one's, so both sides get the same input.
  const root = resolve(flag('--root') ?? here)
  const load = (path: string) => import(pathToFileURL(join(root, path)).href)
  const [store, quiz, learning, model] = await Promise.all([load('mcp/src/store.ts'), load('mcp/src/quiz.ts'), load('mcp/src/learning.ts'), load('frontend/src/quiz/model.ts')])
  const modules: Modules = { TourStore: store.TourStore, QuizStore: quiz.QuizStore, sourceReader: quiz.sourceReader, LearningStore: learning.LearningStore,
    buildQuizContext: model.buildQuizContext, buildQuizContexts: model.buildQuizContexts }
  const example = JSON.parse(await readFile(join(here, `frontend/src/examples/data/${name}.json`), 'utf8')) as Example
  const workDir = await mkdtemp(join(tmpdir(), 'telescode-context-size-'))
  try {
    const result = await measure(modules, { path: resolve(db), sourceRoot: flag('--source') && resolve(flag('--source')!), rank: !args.includes('--no-rank'), example, workDir })
    const out = flag('--out')
    if (out) await writeFile(out, JSON.stringify(result, null, 2))
    console.log(JSON.stringify(result, null, 2))
  } finally { await rm(workDir, { recursive: true, force: true }) }
} else {
  throw new Error('usage: context-size --db <analysis.db> --source <dir> --example <name> [--root <checkout>] [--no-rank] [--out <file>] | --before <json> --after <json>')
}
