// Comprehension quizzes on a tour step.
//
// The quiz is grounded in the code graph the core already computed, never in
// anything the model was not shown. This module does the retrieval half of
// GraphRAG: from the step's node it collects the nodes, relations and learning
// order a question of a given difficulty may draw on (`buildQuizContext`). An
// agent writes the questions from that context through the Telescode MCP
// server, which checks them against it again (`makeQuestions`) before saving.
//
// Two graphs are kept apart. `relations` is the code graph: containment,
// imports and class edges, as the core reports them. `learning` is reading
// order, from the core's reading sequence: what to understand before what. An
// import is not a prerequisite, and the context never presents it as one.
//
// Shared with mcp/: plain data in and out, no browser or Node APIs.

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
 */
export const SCOPE: Record<Difficulty, { hops: number; maxNodes: number; minEvidence: number }> = {
  easy: { hops: 0, maxNodes: 12, minEvidence: 1 },
  medium: { hops: 1, maxNodes: 24, minEvidence: 2 },
  hard: { hops: 2, maxNodes: 40, minEvidence: 3 },
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

export interface QuizContext {
  nodeId: string
  difficulty: Difficulty
  nodes: ContextNode[]
  relations: ContextRelation[]
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

function classFacts(cls: ClassNode): string[] {
  const facts = [`Defined in ${cls.fileId}`]
  if (cls.package) facts.push(`Package ${cls.package}`)
  const methods = cls.methods.map((m) => `Method ${m.access} ${m.name}(${m.params}): ${m.returnType}`)
  const fields = cls.fields.map((f) => `Field ${f.access} ${f.name}`)
  const members = [...methods, ...fields]
  facts.push(...members.slice(0, MAX_MEMBER_FACTS))
  if (members.length > MAX_MEMBER_FACTS) facts.push(`… and ${members.length - MAX_MEMBER_FACTS} more members`)
  if (members.length === 0) facts.push('No methods or fields')
  return facts
}

function nodeFacts(snapshot: AnalysisSnapshot, node: GraphNode): string[] {
  if (node.kind === 'root' || node.kind === 'dir') {
    return [`Contains ${node.fileCount} files and ${node.classCount} classes`]
  }
  if (node.kind === 'class') {
    const cls = snapshot.classes.find((c) => `class:${c.classId}` === node.id)
    const facts = cls ? classFacts(cls) : []
    const rank = snapshot.readingSequence.find((s) => s.entityType === 'class' && `class:${s.entityId}` === node.id)
    if (rank?.localRank != null) facts.push(`Reading rank within its file: ${rank.localRank}`)
    return facts
  }
  if (node.kind === 'file') {
    const fileId = node.id.slice('file:'.length)
    const file = snapshot.files.find((f) => f.fileId === fileId)
    const graph = snapshot.fileGraph.nodes.find((f) => f.fileId === fileId)
    const rank = snapshot.readingSequence.find((s) => s.entityType === 'file' && s.fileId === fileId)
    const facts = [`Path ${fileId}`]
    if (file) {
      facts.push(`Language ${file.language}`, `${file.metrics.rawLoc} lines`, `Max cyclomatic complexity ${file.metrics.maxCyclomaticComplexity}`)
      if (file.metrics.complexityScore != null) facts.push(`Complexity score ${file.metrics.complexityScore.toFixed(2)}`)
    }
    if (graph) facts.push(`Imported by ${graph.inbound} files`, `Imports ${graph.outbound} files`)
    if (rank?.fileRank != null) facts.push(`Reading rank ${rank.fileRank}`)
    facts.push(`Declares ${node.children.length} classes`)
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
export function buildQuizContext(snapshot: AnalysisSnapshot, nodeId: string, difficulty: Difficulty): QuizContext {
  const scope = SCOPE[difficulty]
  if (!scope) throw new Error(`Unknown difficulty: ${String(difficulty)}`)
  const model = buildGraphModel(snapshot, 'Repository')
  const anchor = model.byId.get(nodeId)
  if (!anchor || anchor.kind === 'member') throw new Error(`Unsupported quiz node: ${nodeId}`)

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
  return { nodeId, difficulty, nodes, relations, learning }
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
    if (outside.length) throw new Error(`${where}: evidence outside the ${input.difficulty} context: ${outside.join(', ')}`)
    if (!evidenceIds.includes(nodeId)) throw new Error(`${where}: evidence must include the step's node ${nodeId}`)
    const min = SCOPE[input.difficulty].minEvidence
    if (evidenceIds.length < min) throw new Error(`${where}: ${input.difficulty} questions rest on at least ${min} nodes`)

    const answer = text(input.answer, `${where} answer`, 200)
    let choices: string[]
    let distractors: string[]
    if (input.type === 'true_false') {
      if (!TRUE_FALSE.includes(answer)) throw new Error(`${where}: a true/false answer is "True" or "False"`)
      distractors = TRUE_FALSE.filter((c) => c !== answer)
      choices = TRUE_FALSE
    } else if (input.type === 'multiple_choice') {
      if (!Array.isArray(input.distractors) || input.distractors.length < 2 || input.distractors.length > 4) {
        throw new Error(`${where}: multiple choice needs 2–4 distractors`)
      }
      distractors = input.distractors.map((d, j) => text(d, `${where} distractor ${j + 1}`, 200))
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
      concept: text(input.concept, `${where} concept`, 80),
      question: text(input.question, `${where} question`, 400),
      answer,
      distractors,
      choices,
      explanation: text(input.explanation, `${where} explanation`, 600),
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
