// Comprehension quizzes on a tour step.
//
// The quiz is grounded in the code graph the core already computed and in the
// source it was computed from, never in anything the model was not shown. This
// module does the retrieval half of GraphRAG: from the step's node it collects
// the nodes, relations, source excerpts and learning order a question of a
// given difficulty may draw on (`buildQuizContext`). An agent writes the
// questions from that context through the Telescode MCP server, which checks
// them against it again (`makeQuestions`) before saving.
//
// The context leaves out the core's metrics and counts on purpose: questions
// are about what the code does and why, not how big it is.
//
// Two graphs are kept apart. `relations` is the code graph: containment,
// imports and class edges, as the core reports them. `learning` is reading
// order, from the core's reading sequence: what to understand before what. An
// import is not a prerequisite, and the context never presents it as one.
//
// Shared with mcp/: plain data in and out, no browser or Node APIs. Source
// text comes through a reader the caller supplies.

import type { AnalysisSnapshot, ClassNode } from '../bridge/models'
import { buildGraphModel, type GraphModel, type GraphNode } from '../graph/model'
import { readingTourOrder } from '../tour/model'

export const DIFFICULTIES = ['easy', 'medium', 'hard'] as const
export type Difficulty = (typeof DIFFICULTIES)[number]
export type QuestionType = 'multiple_choice' | 'true_false'

/**
 * How far the context reaches, per difficulty. Difficulty is what a question
 * asks of the reader (reasoning depth, graph hops, abstraction), not who the
 * reader is: easy stays on the node, medium adds what it is directly
 * connected to, hard reaches two hops and the reading order around it.
 * `excerpts` and `sourceChars` bound how much source comes with it.
 */
export const SCOPE: Record<Difficulty, { hops: number; maxNodes: number; minEvidence: number; excerpts: number; sourceChars: number }> = {
  easy: { hops: 0, maxNodes: 12, minEvidence: 1, excerpts: 2, sourceChars: 10_000 },
  medium: { hops: 1, maxNodes: 24, minEvidence: 2, excerpts: 4, sourceChars: 20_000 },
  hard: { hops: 2, maxNodes: 40, minEvidence: 2, excerpts: 6, sourceChars: 32_000 },
}

export type RelationKind = 'contains' | 'imports' | 'class-dependency' | 'class-association' | 'class-unknown'

export interface ContextNode {
  nodeId: string
  kind: GraphNode['kind']
  title: string
  /** Distance from the step's node in the code graph. */
  hop: number
  /** Facts the core reports about the node, one per line, for the agent to cite. */
  facts: string[]
}

export interface ContextRelation {
  source: string
  target: string
  kind: RelationKind
}

/** Source of a file or class in the context, with line numbers. */
export interface SourceExcerpt {
  nodeId: string
  fileId: string
  startLine: number
  endLine: number
  /** Each line prefixed with its number, e.g. "  12| return x". */
  text: string
  /** Cut short to fit the context; the code goes on past `endLine`. */
  truncated: boolean
}

/** A file's text by its repo-relative fileId, or null when it cannot be read. */
export type SourceReader = (fileId: string) => string | null

export interface QuizContext {
  nodeId: string
  difficulty: Difficulty
  nodes: ContextNode[]
  relations: ContextRelation[]
  /** The step's code first, then what it is related to. Empty without a source reader. */
  excerpts: SourceExcerpt[]
  /**
   * Hard questions only: the reading order around the node. `before` are
   * nodes read earlier that the node is directly related to, the likely
   * prerequisites; `after` are the next stops on the reading path.
   * Empty when the snapshot has no reading sequence.
   */
  learning: { before: string[]; after: string[] }
}

const MAX_MEMBER_FACTS = 20
const MAX_RELATIONS = 120

/** How many excerpts, and how many characters of them, a context carries. */
export interface ExcerptLimits {
  excerpts: number
  sourceChars: number
}

/** A combined context for several difficulties at once: shared nodes and source, and what each difficulty may cite. */
export interface MultiQuizContext extends Omit<QuizContext, 'difficulty'> {
  difficulties: Difficulty[]
  /** Per difficulty, the nodes a question of that difficulty may cite: exactly its own context's nodes. */
  evidence: Partial<Record<Difficulty, string[]>>
}

const numberedLine = (line: string, n: number) => `${n}| ${line.trimEnd()}`

/**
 * The lines of a Python class: from its `class` line to the last line indented
 * under it. The core parses Python only; null when the class is not found.
 */
export function classLines(lines: string[], className: string): [number, number] | null {
  const name = className.replace(/[.*+?^$()|[\]\\{}]/g, '\\$&')
  const header = new RegExp(`^(\\s*)class\\s+${name}\\b`)
  const start = lines.findIndex((l) => header.test(l))
  if (start === -1) return null
  const indent = header.exec(lines[start]!)![1]!.length
  let end = headerEnd(lines, start, lines.length - 1)
  for (let i = end + 1; i < lines.length; i++) {
    const line = lines[i]!
    if (!line.trim()) continue
    if (line.length - line.trimStart().length <= indent) break
    end = i
  }
  return [start, end]
}

// A Python def or class header; the core parses Python only.
const DEF = /^(\s*)(async\s+def|def|class)\s+([A-Za-z_]\w*)/
/** Most of a context's source budget one related node's excerpt may take. */
const NEIGHBOUR_SHARE = 0.25
/** A cut related excerpt of code without definitions shorter than this is dropped. */
const MIN_NEIGHBOUR_CHARS = 400
/** At most this part of an excerpt's budget a header may take and still come before the definitions. */
const HEADER_SHARE = 0.5
const indentOf = (line: string) => line.length - line.trimStart().length

/**
 * The last line of the def or class header starting at `i`: a signature
 * whose brackets span lines ends where they close, so a closing `):` at the
 * definition's own indent is not taken for the end of its body.
 */
function headerEnd(lines: string[], i: number, end: number): number {
  let depth = 0
  for (let k = i; k <= end; k++) {
    // Brackets in strings and comments are rare in signatures; drop them roughly.
    const code = lines[k]!.replace(/(["'])(?:\\.|(?!\1).)*\1/g, '').replace(/#.*/, '')
    for (const c of code) {
      if ('([{'.includes(c)) depth++
      else if (')]}'.includes(c)) depth--
    }
    if (depth <= 0) return k
  }
  return i
}

/** A run of lines in an excerpt's range: a def or class with its decorators and body, or the code between them. */
interface Part {
  start: number
  end: number
  /** "class Foo" or "foo()"; none for the code between definitions. */
  label?: string
  name?: string
}

/** Splits lines [start, end] into the definitions at indent `base` and the runs of code between them. */
function partsOf(lines: string[], start: number, end: number, base: number): Part[] {
  const out: Part[] = []
  let gap = -1
  const close = (to: number) => {
    if (gap !== -1 && gap <= to) out.push({ start: gap, end: to })
    gap = -1
  }
  for (let i = start; i <= end; ) {
    let j = i
    while (j <= end && indentOf(lines[j]!) === base && lines[j]!.trimStart().startsWith('@')) j++
    const m = j <= end ? DEF.exec(lines[j]!) : null
    if (m && m[1]!.length === base && indentOf(lines[i]!) === base && lines[i]!.trim()) {
      let last = headerEnd(lines, j, end)
      for (let k = last + 1; k <= end; k++) {
        const line = lines[k]!
        if (!line.trim()) continue
        if (indentOf(line) <= base) break
        last = k
      }
      close(i - 1)
      out.push({ start: i, end: last, name: m[3]!, label: m[2] === 'class' ? `class ${m[3]}` : `${m[3]}()` })
      i = last + 1
      continue
    }
    if (gap === -1) gap = i
    i++
  }
  close(end)
  return out
}

/** The chosen parts, numbered; each run of left-out code becomes one line naming what it holds. */
function render(lines: string[], parts: Part[], chosen: Set<number>): { text: string; last: number } {
  const out: string[] = []
  let last = -1
  let skipped: Part[] = []
  const flush = () => {
    const shown = skipped.filter((p) => lines.slice(p.start, p.end + 1).some((l) => l.trim()))
    if (shown.length) {
      const labels = shown.flatMap((p) => (p.label ? [p.label] : []))
      const names = labels.length > 8 ? [...labels.slice(0, 8), '…'] : labels
      out.push(`…| lines ${shown[0]!.start + 1}–${shown.at(-1)!.end + 1} omitted${names.length ? `: ${names.join(', ')}` : ''}`)
    }
    skipped = []
  }
  parts.forEach((p, i) => {
    if (!chosen.has(i)) return void skipped.push(p)
    flush()
    for (let n = p.start; n <= p.end; n++) out.push(numberedLine(lines[n]!, n + 1))
    last = p.end
  })
  flush()
  return { text: out.join('\n'), last }
}

/**
 * Lines [start, end] within `budget` characters. A range that does not fit
 * keeps its header and the definitions that matter most (`weight`), whole,
 * and names the ones left out; code without definitions keeps its first
 * lines. Definitions `weight` gives null, and with `fill` off the code
 * between definitions, are only sent when the whole range fits. Lines in
 * `covered` are already in another excerpt and left out.
 */
function excerptText(lines: string[], start: number, end: number, base: number, budget: number, covered: Set<number>, weight: (name: string) => number | null, fill: boolean) {
  const free = (p: Part) => {
    for (let n = p.start; n <= p.end; n++) if (covered.has(n)) return false
    return true
  }
  const parts = partsOf(lines, start, end, base)
  const all = new Set(parts.keys())
  const whole = render(lines, parts, all)
  const hasDefs = parts.some((p) => p.name)
  if (whole.text.length <= budget && parts.every(free)) return { ...whole, truncated: false, hasDefs, defs: parts.filter((p) => p.name).length }

  // A short header comes first; a long one only after the definitions, so it cannot take their room.
  const header = parts[0] && !parts[0].name && parts[0].start === start
  const headerFirst = header && render(lines, parts, new Set([0])).text.length <= budget * HEADER_SHARE
  const score = (p: Part, i: number) => (p.name ? weight(p.name) : header && i === 0 ? (headerFirst ? Infinity : 0.25) : fill ? 0.5 : null)
  const order = [...parts.keys()]
    .filter((i) => free(parts[i]!) && score(parts[i]!, i) !== null)
    .sort((a, b) => score(parts[b]!, b)! - score(parts[a]!, a)! || a - b)
  const chosen = new Set<number>()
  if (hasDefs) {
    for (const i of order) {
      chosen.add(i)
      if (render(lines, parts, chosen).text.length > budget) chosen.delete(i)
    }
  }
  const defs = [...chosen].filter((i) => parts[i]!.name).length
  if (defs) return { ...render(lines, parts, chosen), truncated: true, hasDefs, defs }
  // No definitions to choose from, or none fits: the range's first lines, which hold its header and more.
  if (!parts.every(free)) return null
  const out: string[] = []
  let size = 0
  let last = -1
  for (let n = start; n <= end; n++) {
    const line = numberedLine(lines[n]!, n + 1)
    if (size + line.length + 1 > budget) break
    out.push(line)
    size += line.length + 1
    last = n
  }
  return last < start ? null : { text: out.join('\n'), last, truncated: true, hasDefs, defs: 0 }
}

/**
 * Source excerpts for the context's file and class nodes, within `limits`.
 * The stop's own code (`anchorId`) comes first and may use the whole budget;
 * in a range too long to keep whole, the definitions the stop uses or the
 * context names are kept over the rest. Every other node gets a share per
 * file, also when the stop's own source cannot be read. No line is given twice.
 */
function excerptsFor(snapshot: AnalysisSnapshot, model: GraphModel, nodeIds: string[], anchorId: string, limits: ExcerptLimits, read: SourceReader): SourceExcerpt[] {
  let budget = limits.sourceChars
  const out: SourceExcerpt[] = []
  const files = new Map<string, string[] | null>()
  const covered = new Map<string, Set<number>>()
  // Characters already given to related excerpts, per file.
  const spent = new Map<string, number>()
  const linesOf = (fileId: string) => {
    if (!files.has(fileId)) files.set(fileId, read(fileId)?.split(/\r?\n/) ?? null)
    return files.get(fileId)!
  }
  const contextNames = new Set(nodeIds.map((id) => model.byId.get(id)?.label ?? ''))
  // The names the stop's own code uses; none when its source cannot be read.
  let used = new Map<string, number>()
  const wordsOf = (text: string) => {
    const words = new Map<string, number>()
    for (const w of text.match(/[A-Za-z_]\w*/g) ?? []) words.set(w, (words.get(w) ?? 0) + 1)
    return words
  }
  for (const nodeId of nodeIds) {
    if (out.length >= limits.excerpts || budget <= 0) break
    const node = model.byId.get(nodeId)
    if (node?.kind !== 'file' && node?.kind !== 'class') continue
    const cls = node.kind === 'class' ? snapshot.classes.find((c) => `class:${c.classId}` === nodeId) : undefined
    const fileId = cls?.fileId ?? nodeId.slice('file:'.length)
    const lines = linesOf(fileId)
    if (!lines) continue
    const range = cls ? classLines(lines, cls.className) : ([0, lines.length - 1] as [number, number])
    if (!range) continue
    const [start, end] = range
    const seen = covered.get(fileId) ?? new Set<number>()
    // The stop's own code sets what counts as related: the names it uses.
    const isAnchor = nodeId === anchorId
    if (isAnchor) used = wordsOf(lines.slice(start, end + 1).join('\n'))
    const inFile = wordsOf(lines.join('\n'))
    const weight = (name: string) => {
      // Related code that is cut keeps only what the stop uses or the context names.
      if (!isAnchor && !used.has(name) && !contextNames.has(name)) return null
      // Used elsewhere in its file, named by the context, used by the stop.
      let w = Math.min(5, (inFile.get(name) ?? 1) - 1)
      if (contextNames.has(name)) w += 3
      if (!isAnchor && used.has(name)) w += 4
      return w
    }
    let base = 0
    if (cls) {
      const body = lines.slice(headerEnd(lines, start, end) + 1, end + 1).find((l) => l.trim())
      base = body ? indentOf(body) : indentOf(lines[start]!) + 4
    }
    // The stop's own code may use the whole budget; each related file gets a share, however many of its nodes are in the context.
    const share = isAnchor ? budget : Math.min(budget, Math.floor(limits.sourceChars * NEIGHBOUR_SHARE) - (spent.get(fileId) ?? 0))
    if (share <= 0) continue
    const picked = excerptText(lines, start, end, base, share, seen, weight, isAnchor)
    if (!picked) continue
    // Related code cut down to none of the definitions it is related by, or, without definitions, to a few lines, is not worth sending.
    if (!isAnchor && picked.truncated && (picked.hasDefs ? !picked.defs : picked.text.length < MIN_NEIGHBOUR_CHARS)) continue
    const numbers = [...picked.text.matchAll(/^(\d+)\| /gm)].map((m) => Number(m[1]))
    if (!numbers.length) continue
    for (const n of numbers) seen.add(n - 1)
    covered.set(fileId, seen)
    budget -= picked.text.length
    if (!isAnchor) spent.set(fileId, (spent.get(fileId) ?? 0) + picked.text.length)
    out.push({ nodeId, fileId, startLine: numbers[0]!, endLine: picked.last + 1, text: picked.text, truncated: picked.truncated })
  }
  return out
}

function classFacts(cls: ClassNode): string[] {
  const facts = [`Defined in ${cls.fileId}`]
  if (cls.package) facts.push(`Package ${cls.package}`)
  const methods = cls.methods.map((m) => `Method ${m.access} ${m.name}(${m.params}): ${m.returnType}`)
  const fields = cls.fields.map((f) => `Field ${f.access} ${f.name}`)
  const members = [...methods, ...fields]
  facts.push(...members.slice(0, MAX_MEMBER_FACTS))
  if (members.length > MAX_MEMBER_FACTS) facts.push(`… and more members`)
  return facts
}

function nodeFacts(snapshot: AnalysisSnapshot, node: GraphNode): string[] {
  if (node.kind === 'class') {
    const cls = snapshot.classes.find((c) => `class:${c.classId}` === node.id)
    return cls ? classFacts(cls) : []
  }
  if (node.kind === 'file') {
    const fileId = node.id.slice('file:'.length)
    const file = snapshot.files.find((f) => f.fileId === fileId)
    const facts = [`Path ${fileId}`]
    if (file) facts.push(`Language ${file.language}`)
    if (node.children.length) facts.push(`Declares ${node.children.map((c) => c.label).join(', ')}`)
    return facts
  }
  return []
}

/** Every code-graph relation, members left out: they are facts on their class. */
function codeRelations(snapshot: AnalysisSnapshot, model: GraphModel): ContextRelation[] {
  const out: ContextRelation[] = []
  for (const node of model.nodes) {
    if (node.parent && node.kind !== 'member') out.push({ source: node.parent.id, target: node.id, kind: 'contains' })
  }
  for (const e of model.edges) {
    if (e.kind === 'import') out.push({ source: e.source.id, target: e.target.id, kind: 'imports' })
  }
  for (const e of snapshot.classEdges) {
    const source = `class:${e.source}`
    const target = `class:${e.target}`
    if (source !== target && model.byId.has(source) && model.byId.has(target)) {
      out.push({ source, target, kind: `class-${e.type}` })
    }
  }
  return out
}

/**
 * The context a question of `difficulty` about `nodeId` may draw on: the
 * nodes within reach, nearest first, and the relations among them.
 */
export function buildQuizContext(snapshot: AnalysisSnapshot, nodeId: string, difficulty: Difficulty, read?: SourceReader, limits?: Partial<ExcerptLimits>): QuizContext {
  const model = buildGraphModel(snapshot, 'Repository')
  const { nodes, relations, learning } = retrieve(snapshot, model, nodeId, difficulty)
  const excerpts = read ? excerptsFor(snapshot, model, nodes.map((n) => n.nodeId), nodeId, { ...SCOPE[difficulty], ...limits }, read) : []
  return { nodeId, difficulty, nodes, relations, excerpts, learning }
}

/**
 * One context for questions of several difficulties about `nodeId`, so the
 * code they share is sent once: the union of their nodes and relations, the
 * source within the widest difficulty's budget, and per difficulty the nodes
 * its questions may cite (`evidence`), the same nodes `buildQuizContext`
 * gives and the quiz validator accepts.
 */
export function buildQuizContexts(snapshot: AnalysisSnapshot, nodeId: string, difficulties: Difficulty[], read?: SourceReader): MultiQuizContext {
  const levels = DIFFICULTIES.filter((d) => difficulties.includes(d))
  if (!levels.length || difficulties.some((d) => !SCOPE[d])) throw new Error(`Unknown difficulties: ${String(difficulties)}`)
  const model = buildGraphModel(snapshot, 'Repository')
  const parts = levels.map((d) => retrieve(snapshot, model, nodeId, d))
  const nodes = new Map<string, ContextNode>()
  const relations = new Map<string, ContextRelation>()
  // The widest context orders the nodes; the narrower ones only add what it cut.
  for (const part of [...parts].reverse()) {
    for (const n of part.nodes) {
      const seen = nodes.get(n.nodeId)
      if (!seen) nodes.set(n.nodeId, n)
      else if (n.hop < seen.hop) nodes.set(n.nodeId, { ...seen, hop: n.hop })
    }
    for (const r of part.relations) relations.set(`${r.source}\0${r.target}\0${r.kind}`, r)
  }
  const ids = [...nodes.keys()]
  const limits = {
    excerpts: Math.max(...levels.map((d) => SCOPE[d].excerpts)),
    sourceChars: Math.max(...levels.map((d) => SCOPE[d].sourceChars)),
  }
  return {
    nodeId,
    difficulties: levels,
    nodes: ids.map((id) => nodes.get(id)!),
    relations: [...relations.values()],
    excerpts: read ? excerptsFor(snapshot, model, ids, nodeId, limits, read) : [],
    learning: parts.at(-1)!.learning,
    evidence: Object.fromEntries(levels.map((d, i) => [d, parts[i]!.nodes.map((n) => n.nodeId)])),
  }
}

function retrieve(snapshot: AnalysisSnapshot, model: GraphModel, nodeId: string, difficulty: Difficulty) {
  const scope = SCOPE[difficulty]
  if (!scope) throw new Error(`Unknown difficulty: ${String(difficulty)}`)
  const anchor = model.byId.get(nodeId)
  if (!anchor || anchor.kind === 'member') {
    // A file path without its `file:` prefix is a common slip; naming the right ID saves the writer a search.
    const hint = !anchor && model.byId.has(`file:${nodeId}`) ? `. Did you mean file:${nodeId}?` : ''
    throw new Error(`Unsupported quiz node: ${nodeId}${hint}`)
  }

  const all = codeRelations(snapshot, model)
  const neighbours = new Map<string, ContextRelation[]>()
  for (const r of all) {
    for (const id of [r.source, r.target]) {
      let list = neighbours.get(id)
      if (!list) neighbours.set(id, (list = []))
      list.push(r)
    }
  }

  // Easy reaches the node's own children, which are part of what it is.
  const hops = new Map<string, number>([[nodeId, 0]])
  if (scope.hops === 0) {
    for (const child of anchor.children) if (child.kind !== 'member') hops.set(child.id, 1)
  }
  let frontier = [nodeId]
  for (let hop = 1; hop <= scope.hops; hop++) {
    const next: string[] = []
    for (const id of frontier) {
      for (const r of neighbours.get(id) ?? []) {
        const other = r.source === id ? r.target : r.source
        if (!hops.has(other)) {
          hops.set(other, hop)
          next.push(other)
        }
      }
    }
    frontier = next
  }

  // Nearest first; at the same distance, what the node is related to by code
  // before what merely sits in the same folder, then by reading order.
  const order = new Map(readingTourOrder(snapshot).map((n, i) => [n.nodeId, i]))
  const linked = new Set(
    (neighbours.get(nodeId) ?? []).filter((r) => r.kind !== 'contains').flatMap((r) => [r.source, r.target]),
  )
  const kept = [...hops.keys()]
    .sort(
      (a, b) =>
        hops.get(a)! - hops.get(b)! ||
        Number(linked.has(b)) - Number(linked.has(a)) ||
        (order.get(a) ?? Infinity) - (order.get(b) ?? Infinity) ||
        a.localeCompare(b),
    )
    .slice(0, scope.maxNodes)
  const inScope = new Set(kept)

  const nodes = kept.map((id) => {
    const node = model.byId.get(id)!
    return {
      nodeId: id,
      kind: node.kind,
      title: node.kind === 'file' ? id.slice('file:'.length) : node.label,
      hop: hops.get(id)!,
      facts: nodeFacts(snapshot, node),
    }
  })
  const relations = all.filter((r) => inScope.has(r.source) && inScope.has(r.target)).slice(0, MAX_RELATIONS)

  const learning: QuizContext['learning'] = { before: [], after: [] }
  const at = order.get(nodeId)
  if (difficulty === 'hard' && at != null) {
    const path = readingTourOrder(snapshot).map((n) => n.nodeId)
    learning.before = path.slice(0, at).filter((id) => linked.has(id) && inScope.has(id))
    learning.after = path.slice(at + 1, at + 4)
  }
  return { nodes, relations, learning }
}

/** What the agent submits for one question. */
export interface QuestionInput {
  type: QuestionType
  difficulty: Difficulty
  /** The idea the question checks, in a few words: what a later tour adapts to. */
  concept: string
  question: string
  /** The correct choice. For true/false, "True" or "False". */
  answer: string
  /** Wrong but plausible choices; 2–4 for multiple choice, none for true/false. */
  distractors: string[]
  /** Why the answer is right, from the evidence. Shown after answering. */
  explanation: string
  /** The context nodes the answer rests on; must include the step's node. */
  evidenceNodeIds: string[]
}

export interface QuizEvidence {
  nodeId: string
  title: string
}

export interface QuizQuestion extends Omit<QuestionInput, 'evidenceNodeIds'> {
  /** The quiz_id that responses refer to. */
  id: string
  /** The answer and distractors in the order shown. */
  choices: string[]
  evidence: QuizEvidence[]
}

/** Where in a tour a quiz was asked: what responses are recorded against. */
export interface TourContext {
  tourId: string
  tourRevision: number
  stopIndex: number
  nodeId: string
}

/** The quizzes written for one tour step. */
export interface QuizSet extends TourContext {
  schemaVersion: 1
  id: string
  language: 'ko' | 'en'
  snapshotHash: string
  questions: QuizQuestion[]
  createdMs: number
}

const TRUE_FALSE = ['True', 'False']
export const MAX_QUESTIONS_PER_DIFFICULTY = 3

/** Limits `makeQuestions` enforces, published so a question writer can follow them up front. */
export const QUESTION_LIMITS = {
  perDifficulty: MAX_QUESTIONS_PER_DIFFICULTY,
  minEvidence: Object.fromEntries(DIFFICULTIES.map((d) => [d, SCOPE[d].minEvidence])) as Record<Difficulty, number>,
  distractors: { min: 2, max: 4 },
  trueFalseAnswers: TRUE_FALSE,
  chars: { concept: 80, question: 500, answer: 300, distractor: 300, explanation: 900 },
} as const

/** How much longer than its longest distractor a multiple-choice answer may be before its length gives it away. */
export const ANSWER_LENGTH_RATIO = 1.25

/**
 * The multiple-choice questions whose answer is much longer than every
 * distractor, so its length alone would give it away. Reported, not refused:
 * a refusal would cost the writer another turn, so this only measures how
 * often it happens.
 */
export function longAnswers(inputs: QuestionInput[]): string[] {
  if (!Array.isArray(inputs)) return []
  return inputs.flatMap((q, i) => {
    if (q?.type !== 'multiple_choice' || typeof q.answer !== 'string' || !Array.isArray(q.distractors) || !q.distractors.length) return []
    const answer = q.answer.trim().length
    const longest = Math.max(...q.distractors.map((d) => (typeof d === 'string' ? d.trim().length : 0)))
    // Short answers such as identifiers may differ by a few characters without giving anything away.
    return answer > longest * ANSWER_LENGTH_RATIO && answer - longest > 20
      ? [`Question ${i + 1}: answer of ${answer} characters, longest distractor ${longest}`]
      : []
  })
}

function text(value: unknown, what: string, max: number): string {
  if (typeof value !== 'string' || !value.trim() || value.length > max || [...value].some((c) => c.charCodeAt(0) < 9)) {
    throw new Error(`${what} must contain 1–${max} characters`)
  }
  return value.trim()
}

/** A deterministic shuffle, so a question always shows its choices in the same order. */
function shuffled<T>(items: T[], seed: string): T[] {
  let h = 2166136261
  for (const c of seed) h = Math.imul(h ^ c.charCodeAt(0), 16777619)
  const out = [...items]
  for (let i = out.length - 1; i > 0; i--) {
    h = Math.imul(h ^ (h >>> 15), 2246822507) >>> 0
    const j = h % (i + 1)
    ;[out[i], out[j]] = [out[j]!, out[i]!]
  }
  return out
}

/**
 * Checks the agent's questions against the context each one's difficulty
 * allows and returns them ready to save. A question citing a node outside
 * that context, or not citing the step's node, is refused: that is what keeps
 * the questions about this code.
 */
export function makeQuestions(snapshot: AnalysisSnapshot, setId: string, nodeId: string, inputs: QuestionInput[]): QuizQuestion[] {
  // Evidence is checked against the nodes in reach, which do not depend on source text.
  if (!Array.isArray(inputs) || inputs.length < 1 || inputs.length > DIFFICULTIES.length * MAX_QUESTIONS_PER_DIFFICULTY) {
    throw new Error(`A quiz requires 1–${DIFFICULTIES.length * MAX_QUESTIONS_PER_DIFFICULTY} questions`)
  }
  const contexts = new Map<Difficulty, QuizContext>()
  const perDifficulty = new Map<Difficulty, number>()
  return inputs.map((input, i) => {
    const where = `Question ${i + 1}`
    if (!DIFFICULTIES.includes(input.difficulty)) throw new Error(`${where}: unknown difficulty`)
    const count = (perDifficulty.get(input.difficulty) ?? 0) + 1
    if (count > MAX_QUESTIONS_PER_DIFFICULTY) throw new Error(`At most ${MAX_QUESTIONS_PER_DIFFICULTY} ${input.difficulty} questions`)
    perDifficulty.set(input.difficulty, count)

    let context = contexts.get(input.difficulty)
    if (!context) contexts.set(input.difficulty, (context = buildQuizContext(snapshot, nodeId, input.difficulty)))
    const titles = new Map(context.nodes.map((n) => [n.nodeId, n.title]))

    const evidenceIds = [...new Set(Array.isArray(input.evidenceNodeIds) ? input.evidenceNodeIds : [])]
    const outside = evidenceIds.filter((id) => !titles.has(id))
    // Naming what may be cited lets the writer correct the question in one go.
    if (outside.length) throw new Error(`${where}: evidence outside the ${input.difficulty} context: ${outside.join(', ')}. For ${nodeId} at ${input.difficulty} it may cite: ${[...titles.keys()].join(', ')}`)
    if (!evidenceIds.includes(nodeId)) throw new Error(`${where}: evidence must include the step's node ${nodeId}`)
    const min = QUESTION_LIMITS.minEvidence[input.difficulty]
    if (evidenceIds.length < min) throw new Error(`${where}: ${input.difficulty} questions rest on at least ${min} nodes`)

    const { chars, distractors: range } = QUESTION_LIMITS
    const answer = text(input.answer, `${where} answer`, chars.answer)
    let choices: string[]
    let distractors: string[]
    if (input.type === 'true_false') {
      if (!TRUE_FALSE.includes(answer)) throw new Error(`${where}: a true/false answer is "True" or "False"`)
      distractors = TRUE_FALSE.filter((c) => c !== answer)
      choices = TRUE_FALSE
    } else if (input.type === 'multiple_choice') {
      if (!Array.isArray(input.distractors) || input.distractors.length < range.min || input.distractors.length > range.max) {
        throw new Error(`${where}: multiple choice needs ${range.min}–${range.max} distractors`)
      }
      distractors = input.distractors.map((d, j) => text(d, `${where} distractor ${j + 1}`, chars.distractor))
      const all = [answer, ...distractors]
      if (new Set(all.map((c) => c.toLowerCase())).size !== all.length) throw new Error(`${where}: choices must all differ`)
      choices = shuffled(all, `${setId}:${i}`)
    } else {
      throw new Error(`${where}: unsupported question type`)
    }

    return {
      id: `${setId}:${i + 1}`,
      type: input.type,
      difficulty: input.difficulty,
      concept: text(input.concept, `${where} concept`, chars.concept),
      question: text(input.question, `${where} question`, chars.question),
      answer,
      distractors,
      choices,
      explanation: text(input.explanation, `${where} explanation`, chars.explanation),
      evidence: evidenceIds.map((id) => ({ nodeId: id, title: titles.get(id)! })),
    }
  })
}

/** One answer to one question, as saved for later estimates of what the user knows. */
export interface QuizResponse {
  schemaVersion: 1
  id: string
  /** The question answered. */
  quizId: string
  quizSetId: string
  concept: string
  difficulty: Difficulty
  type: QuestionType
  result: 'correct' | 'incorrect'
  selectedAnswer: string
  /** 1 for the first answer to this question, counting every earlier one. */
  attempt: number
  tourContext: TourContext
  snapshotHash: string
  /** The nodes the question rests on. */
  relatedNodes: string[]
  /** From the question being shown (or retried) to the answer, when the client measured it. */
  responseTimeMs: number | null
  /** ISO 8601. */
  timestamp: string
}

/** What the user sees after answering. */
export interface QuizFeedback {
  correct: boolean
  answer: string
  explanation: string
  evidence: QuizEvidence[]
  attempt: number
}

export function feedbackFor(question: QuizQuestion, response: Pick<QuizResponse, 'result' | 'attempt'>): QuizFeedback {
  return {
    correct: response.result === 'correct',
    answer: question.answer,
    explanation: question.explanation,
    evidence: question.evidence,
    attempt: response.attempt,
  }
}
