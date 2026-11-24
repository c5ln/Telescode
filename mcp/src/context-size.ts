// Measures the contexts an agent receives for quizzes and adaptive learning,
// without starting an agent: the combined quiz context of each tour stop, the
// first preparation context, and the follow-up material context after a
// wrong first answer. Reads only; it never calls a model.
//
//   npm --prefix mcp run context:size -- --db <analysis.db> --source <dir> --example <name> [--root <checkout>] [--no-rank] [--out <file>]
//   npm --prefix mcp run context:size -- --before <before.json> --after <after.json>
//
// `--root` measures another checkout's code (an earlier commit), with this
// checkout's example as the same input. See mcp/README.md › Measuring context size.
import { createHash } from 'node:crypto'
import { join } from 'node:path'

/** What a measurement replays: an app example's goal, its first prepared action and its tour. */
export interface Example {
  requirements: string
  tour: { plan: { stops: { nodeId: string }[] } }
  curriculum: { version: number; concepts: unknown[]; relations: unknown[]; items: ExampleItem[] }
}
interface ExampleItem {
  id: string
  kind: string
  nodeId: string
  conceptId: string
  level: 'easy' | 'medium' | 'hard'
  questions: { question: string; answer: string }[]
  [key: string]: unknown
}

/**
 * The code measured: the classes and functions of one checkout, loaded by the
 * caller so an earlier commit can be measured with the same input.
 */
export interface Modules {
  TourStore: any
  QuizStore: any
  LearningStore: any
  sourceReader: (root?: string) => ((fileId: string) => string | null) | undefined
  buildQuizContext: (...args: any[]) => any
  /** Absent before #84, when each difficulty had its own context. */
  buildQuizContexts?: (...args: any[]) => any
}

type Excerpt = { fileId: string; text: string; truncated?: boolean }
type Context = { nodeId?: string; excerpts: Excerpt[] }

export interface Measurement {
  quiz: { chars: number; sourceChars: number; repeatedChars: number; stops: { nodeId: string; chars: number; sourceChars: number; repeatedChars: number }[] }
  preparation: {
    chars: number; sourceChars: number; candidates: number; truncated: number; repeatedLines: number; repeatedChars: number
    perCandidate: { nodeId: string; sourceChars: number; truncated: boolean; omittedMarkers: number }[]
  }
  material: { shouldRun: boolean; chars: number | null; sourceChars: number | null; repeatedLines: number | null; repeatedChars: number | null
    slots: { branch: string; level: string; nodeIds: string[] }[] }
}

// A numbered source line, in the current numbering ("12| ") or the padded one before #84 ("   12| ").
const NUMBERED = /^\s*(\d+)\| /
const sourceChars = (contexts: Context[]) => contexts.reduce((n, c) => n + c.excerpts.reduce((m, e) => m + e.text.length, 0), 0)

/** Source lines (file and line number) given more than once across `contexts`, and the characters they take. */
export function repeatedSource(contexts: { excerpts: { fileId: string; text: string }[] }[]) {
  const seen = new Set<string>()
  let sourceLines = 0, repeatedLines = 0, repeatedChars = 0
  for (const c of contexts) for (const e of c.excerpts) for (const line of e.text.split('\n')) {
    const m = NUMBERED.exec(line)
    if (!m) continue
    sourceLines++
    const key = `${e.fileId}:${m[1]}`
    if (seen.has(key)) { repeatedLines++; repeatedChars += line.length + 1 }
    seen.add(key)
  }
  return { sourceLines, repeatedLines, repeatedChars }
}

/** Replays `example` on the analysis at `path` and measures what an agent would receive at each step. */
export async function measure(modules: Modules, input: { path: string; sourceRoot?: string; rank: boolean; example: Example; workDir: string }): Promise<Measurement> {
  const { example } = input
  const tours = new modules.TourStore(join(input.workDir, 'artifacts'))
  const base = await tours.openAnalysis(input.path, undefined, input.sourceRoot)
  const query = example.requirements.normalize('NFKC').trim().replace(/\s+/g, ' ')
  // As start_learning_session does: rank the reading order by the goal's query.
  const project = input.rank ? await tours.rank(base.id, query) : base
  const store = new modules.LearningStore(tours.outputRoot, tours, new modules.QuizStore(tours.outputRoot, tours))
  try {
    const quiz = quizContexts(modules, project, example)
    const conceptId = example.curriculum.items[0]!.conceptId
    const goal = {
      goalId: conceptId.replace(/^goal:/, '') || `measure:${createHash('sha256').update(query).digest('hex')}`, goalRevision: 1,
      rawQuery: query, retrievalQuery: query, budget: 12,
      repositoryId: project.sourceRoot ?? project.databasePath ?? project.hash, sourceSnapshotHash: project.sourceSnapshotHash ?? project.hash,
      targetConcepts: (['easy', 'medium', 'hard'] as const).map(level => ({ conceptId, level, weight: 1 })),
    }
    return { quiz, preparation: await preparation(store, project, goal), material: await material(store, project, goal, example) }
  } finally { store.close() }
}

/** Each tour stop's quiz context, as the quiz agent receives it: one combined context, or one per difficulty before #84. */
function quizContexts(modules: Modules, project: any, example: Example): Measurement['quiz'] {
  const read = modules.sourceReader(project.sourceRoot)
  const stops = example.tour.plan.stops.map(({ nodeId }) => {
    const parts: { excerpts: Excerpt[] }[] = modules.buildQuizContexts
      ? [modules.buildQuizContexts(project.snapshot, nodeId, ['easy', 'medium'], read)]
      : ['easy', 'medium'].map(level => modules.buildQuizContext(project.snapshot, nodeId, level, read))
    return { nodeId, chars: parts.reduce((n, p) => n + JSON.stringify(p).length, 0), sourceChars: sourceChars(parts), repeatedChars: repeatedSource(parts).repeatedChars }
  })
  const sum = (key: 'chars' | 'sourceChars' | 'repeatedChars') => stops.reduce((n, s) => n + s[key], 0)
  return { chars: sum('chars'), sourceChars: sum('sourceChars'), repeatedChars: sum('repeatedChars'), stops }
}

async function preparation(store: any, project: any, goal: unknown): Promise<Measurement['preparation']> {
  const started = await store.start(project, goal)
  const job = await store.requestPreparation(started.session.id, started.session.revision, 'claude', process.pid, { useCache: false })
  const context = await store.jobContext(job.id) as { contexts: (Context & { nodeId: string })[] }
  await store.finishPreparation(job.id, 'measurement only')
  const perCandidate = context.contexts.map(c => ({
    nodeId: c.nodeId, sourceChars: sourceChars([c]), truncated: c.excerpts.some(e => e.truncated),
    omittedMarkers: c.excerpts.reduce((n, e) => n + (e.text.match(/^…\| lines/gm)?.length ?? 0), 0),
  }))
  const { repeatedLines, repeatedChars } = repeatedSource(context.contexts)
  return { chars: JSON.stringify(context).length, sourceChars: sourceChars(context.contexts), candidates: context.contexts.length,
    truncated: perCandidate.filter(c => c.truncated).length, repeatedLines, repeatedChars, perCandidate }
}

/** The material context after a wrong answer to the first of only two prepared questions, which asks for both outcomes. */
async function material(store: any, project: any, goal: unknown, example: Example): Promise<Measurement['material']> {
  const first = example.curriculum.items[0]!
  // The server's cost and learning model for a saved action (learning-tools.ts).
  const action = { ...first, cost: 1, effects: [{ conceptId: first.conceptId, level: first.level, learn: .6 }] }
  const started = await store.start(project, goal, 'context-size')
  let view = await store.prepare(started.session.id, started.session.revision, { version: 1, relations: [], concepts: example.curriculum.concepts, items: [action] })
  const q = view.question
  const right = q.choices.indexOf(first.questions.find(x => x.question === q.question)?.answer)
  const answered = await store.respond({ sessionId: view.session.id, revision: view.session.revision, stopId: view.session.current.id,
    submissionId: 'context-size-1', questionId: q.id, selectedIndex: (right + 1) % q.choices.length })
  view = await store.advance(view.session.id, answered.view.session.revision, view.session.current.id, 'continue')
  const job = await store.requestMaterial(view.session.id, 'claude', process.pid)
  if (!job.shouldRun) return { shouldRun: false, chars: null, sourceChars: null, repeatedLines: null, repeatedChars: null, slots: [] }
  const context = await store.materialContext(job.id) as { contexts?: Context[]; slots: { branch: string; level: string; nodeIds?: string[]; contexts?: (Context & { nodeId: string })[] }[] }
  await store.finishMaterial(job.id, 'measurement only')
  // Since #84 the candidates are sent once for the job; before, each slot carried its own.
  const contexts = context.contexts ?? context.slots.flatMap(s => s.contexts ?? [])
  const { repeatedLines, repeatedChars } = repeatedSource(contexts)
  return { shouldRun: true, chars: JSON.stringify(context).length, sourceChars: sourceChars(contexts), repeatedLines, repeatedChars,
    slots: context.slots.map(s => ({ branch: s.branch, level: s.level, nodeIds: s.nodeIds ?? (s.contexts ?? []).map(c => c.nodeId) })) }
}

const number = (n: number | null) => (n === null ? '—' : n.toLocaleString('en-US'))
function change(before: number | null, after: number | null) {
  if (before === null || after === null || before === 0) return '—'
  const pct = ((after - before) / before) * 100
  return `${pct < 0 ? '−' : pct > 0 ? '+' : ''}${Math.abs(pct).toFixed(1)}%`
}

/** A Markdown table of two measurements, before and after a change. */
export function compare(before: Measurement, after: Measurement) {
  const rows: [string, (m: Measurement) => number | null][] = [
    ['Quiz contexts (chars)', m => m.quiz.chars],
    ['Quiz source (chars)', m => m.quiz.sourceChars],
    ['Quiz repeated source (chars)', m => m.quiz.repeatedChars],
    ['Preparation context (chars)', m => m.preparation.chars],
    ['Preparation source (chars)', m => m.preparation.sourceChars],
    ['Preparation truncated candidates', m => m.preparation.truncated],
    ['Material context (chars)', m => m.material.chars],
    ['Material source (chars)', m => m.material.sourceChars],
    ['Material repeated source (chars)', m => m.material.repeatedChars],
  ]
  return ['| | Before | After | Change |', '|---|---:|---:|---:|',
    ...rows.map(([label, get]) => `| ${label} | ${number(get(before))} | ${number(get(after))} | ${change(get(before), get(after))} |`)].join('\n')
}
