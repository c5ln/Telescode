import { createHash, randomUUID } from 'node:crypto'
import { mkdirSync } from 'node:fs'
import { resolve } from 'node:path'
import { DatabaseSync } from 'node:sqlite'
import type { TourStore, Project } from './store.ts'
import { sourceReader, type QuizStore } from './quiz.ts'
import { buildQuizContext, feedbackFor, makeQuestions, DIFFICULTIES, QUESTION_LIMITS, type QuizQuestion, type SourceExcerpt } from '../../frontend/src/quiz/model.ts'
import { chooseAction, goalScore, mastery, observe, predict, MODEL_VERSION, PRIOR, validateGoal, type LearningGoal, type Knowledge, type LearningAction } from '../../frontend/src/learning/model.ts'
import type { Curriculum, LearningAnswer, LearningResult, LearningView, PreparationJob, LearningSession, PreparedItem } from '../../frontend/src/learning/contracts.ts'

/** Upper bound, in JSON characters, for the material-writing agent's preparation context (one tool result). */
export const PREPARATION_CONTEXT_CHARS = 40_000
const PREPARATION_CANDIDATES = 6
const CANDIDATE_SOURCE_CHARS = 5_000
/** Curriculum limits enforced by `prepare` and the save_learning_curriculum schema. */
export const CURRICULUM_LIMITS = {
  concepts: 8, actions: 24, relations: 32,
  // Questions must match their action's level, so the per-difficulty quiz limit is the per-action limit.
  questionsPerAction: QUESTION_LIMITS.perDifficulty,
  chars: { conceptName: 100, definition: 1000, caption: 1600, relationReason: 1000 },
} as const
/** Told to the material-writing agent up front; the same constants are what the validators enforce. */
export const PREPARATION_RULES = {
  curriculum: CURRICULUM_LIMITS,
  questions: QUESTION_LIMITS,
  evidence: 'Choose each action nodeId from contexts[].nodeId. A question of level L on that action may cite only contexts[i].evidence[L], must cite the action node, '
    + 'and needs at least questions.minEvidence[L] distinct nodes. Relation evidenceNodeIds must be graph nodes, e.g. ones listed in contexts.',
  text: 'Answer and distractors must all differ ignoring case; text fields are non-empty after trimming and contain no characters below U+0009 (tab).',
} as const
/** Room reserved for the job summary that jobContext adds around the context. */
const JOB_SUMMARY_CHARS = 120
/** Cuts an excerpt at a line boundary to at most `chars` characters, keeping its line numbers right. */
function clip(excerpt: SourceExcerpt, chars: number): SourceExcerpt {
  if (excerpt.text.length <= chars) return excerpt
  const cut = excerpt.text.slice(0, chars)
  const text = cut.slice(0, Math.max(0, cut.lastIndexOf('\n')))
  const lines = text ? text.split('\n').length : 0
  return { ...excerpt, text, endLine: excerpt.startLine + lines - 1, truncated: true }
}
const hash = (text: string) => createHash('sha256').update(text).digest('hex')
const norm = (text: string) => text.normalize('NFKC').trim().replace(/\s+/g, ' ').toLowerCase()
const fingerprint = (q: QuizQuestion) => hash(JSON.stringify([norm(q.question), norm(q.answer), q.choices.map(norm).sort(), q.difficulty]))
const levels = ['easy', 'medium', 'hard'] as const
const dim = (id: string, level: string) => `${id}\0${level}`
type CompiledItem = PreparedItem & { compiled: QuizQuestion[]; evidence: Record<string, string> }
type Stored = { session: LearningSession; project: Project; curriculum: Curriculum | null; items: CompiledItem[];
  knowledge: Knowledge[]; evidenceKeys: Record<string, string>; questionId?: string; responseId?: string
  /** Per concept/level: source excerpts (node -> content hash) of the actions that produced the stored estimate. */
  knowledgeEvidence: Record<string, Record<string, string>> }
type Job = PreparationJob & { provider: string; createdMs: number; baseVersion: number; sourceEvidence: Record<string, string>; ownerPid: number }
/** Whether the app process running a job's CLI worker still exists (EPERM means it exists under another user). */
function alive(pid: number) {
  try { process.kill(pid, 0); return true } catch (error) { return (error as NodeJS.ErrnoException).code === 'EPERM' }
}

/** Short-lived desktop/MCP processes share transactions, source-versioned evidence,
 * immutable goals and an append-only event trail. No model API is called here. */
export class LearningStore {
  readonly db: DatabaseSync
  private closed = false
  private legacyExposed = new Set<string>()
  constructor(readonly outputRoot: string, readonly tours: TourStore, readonly quizzes: QuizStore) {
    mkdirSync(outputRoot, { recursive: true })
    this.db = new DatabaseSync(resolve(outputRoot, 'learning.db'))
    this.db.exec(`PRAGMA busy_timeout=5000; PRAGMA journal_mode=WAL;
      CREATE TABLE IF NOT EXISTS learning_sessions(id TEXT PRIMARY KEY,data TEXT NOT NULL);
      CREATE TABLE IF NOT EXISTS learning_submissions(id TEXT PRIMARY KEY,request TEXT NOT NULL,result TEXT NOT NULL);
      CREATE TABLE IF NOT EXISTS learning_exposures(learner TEXT,repository TEXT,fingerprint TEXT,PRIMARY KEY(learner,repository,fingerprint));
      CREATE TABLE IF NOT EXISTS learning_knowledge(learner TEXT,repository TEXT,source TEXT,concept TEXT,level TEXT,definition TEXT,data TEXT NOT NULL,
        PRIMARY KEY(learner,repository,source,concept,level,definition));
      CREATE TABLE IF NOT EXISTS learning_concepts(repository TEXT,id TEXT,version INTEGER,definition TEXT,PRIMARY KEY(repository,id,version));
      CREATE TABLE IF NOT EXISTS learning_events(seq INTEGER PRIMARY KEY,session TEXT NOT NULL,data TEXT NOT NULL);
      CREATE TABLE IF NOT EXISTS learning_jobs(id TEXT PRIMARY KEY,data TEXT NOT NULL);
      CREATE TABLE IF NOT EXISTS learning_material_cache(key TEXT PRIMARY KEY,data TEXT NOT NULL);
    `)
  }
  close() { if (!this.closed) { this.db.close(); this.closed = true } }
  private transaction<T>(work: () => T): T {
    this.db.exec('BEGIN IMMEDIATE')
    try { const out = work(); this.db.exec('COMMIT'); return out } catch (error) { this.db.exec('ROLLBACK'); throw error }
  }
  private load(id: string): Stored {
    const row = this.db.prepare('SELECT data FROM learning_sessions WHERE id=?').get(id)
    if (!row) throw new Error('Learning session not found')
    return JSON.parse(row.data as string)
  }
  private save(stored: Stored, event: unknown) {
    this.db.prepare('INSERT OR REPLACE INTO learning_sessions VALUES(?,?)').run(stored.session.id, JSON.stringify(stored))
    this.db.prepare('INSERT INTO learning_events(session,data) VALUES(?,?)').run(stored.session.id, JSON.stringify({ event, revision: stored.session.revision, knowledge: stored.knowledge }))
  }
  private assertRevision(s: Stored, revision: number) {
    if (s.session.revision !== revision) throw new Error('Stale learning revision; reload the session')
  }
  private fileOf(project: Project, nodeId: string) {
    return nodeId.startsWith('file:') ? nodeId.slice(5) : nodeId.startsWith('class:')
      ? project.snapshot.classes.find(c => `class:${c.classId}` === nodeId)?.fileId : undefined
  }
  private evidence(project: Project, nodes: string[]) {
    const read = sourceReader(project.sourceRoot)
    return Object.fromEntries([...new Set(nodes)].map(nodeId => {
      const file = this.fileOf(project, nodeId)
      const content = file && read ? read(file) : undefined
      if (file && read && content === null) throw new Error(`Source evidence unavailable: ${file}`)
      return [nodeId, hash(content ?? JSON.stringify([project.sourceSnapshotHash ?? project.hash, nodeId]))]
    }))
  }
  private assertEvidence(s: Stored, item: CompiledItem) {
    const current = this.evidence(s.project, Object.keys(item.evidence))
    if (JSON.stringify(current) !== JSON.stringify(item.evidence)) throw new Error('Source evidence changed; prepare a new session')
  }
  private async legacy() {
    const [sets, responses] = await Promise.all([this.quizzes.list(), this.quizzes.responses()])
    const ids = new Set(responses.map(r => r.quizId))
    this.legacyExposed = new Set(sets.flatMap(s => s.questions.filter(q => ids.has(q.id)).map(fingerprint)))
  }
  /** Shared estimate for an immutable concept definition, valid only while the source it was learned from is unchanged.
   * Adding material elsewhere keeps it; editing any recorded source excerpt invalidates it. */
  private stored(s: Stored, conceptId: string, level: string): Knowledge | null {
    const row = this.db.prepare('SELECT data FROM learning_knowledge WHERE learner=? AND repository=? AND source=? AND concept=? AND level=? AND definition=?')
      .get(s.session.learnerId, s.session.goal.repositoryId, s.session.goal.sourceSnapshotHash, conceptId, level, s.evidenceKeys[conceptId]!)
    if (!row) return null
    const { knowledge, evidence } = JSON.parse(row.data as string) as { knowledge: Knowledge; evidence: Record<string, string> }
    let current: Record<string, string>
    try { current = this.evidence(s.project, Object.keys(evidence)) } catch { return null }
    if (Object.entries(evidence).some(([node, digest]) => current[node] !== digest)) return null
    s.knowledgeEvidence[dim(conceptId, level)] = evidence
    return knowledge
  }
  private refresh(s: Stored) { s.knowledge = s.knowledge.map(k => this.stored(s, k.conceptId, k.level) ?? k) }
  /** Estimates changed by `item` now also depend on its source excerpts. */
  private persistKnowledge(s: Stored, item: CompiledItem, before: Knowledge[]) {
    for (const k of s.knowledge) {
      const key = dim(k.conceptId, k.level)
      const previous = before.find(b => b.conceptId === k.conceptId && b.level === k.level)
      if (JSON.stringify(previous) !== JSON.stringify(k)) s.knowledgeEvidence[key] = { ...s.knowledgeEvidence[key], ...item.evidence }
      this.db.prepare('INSERT OR REPLACE INTO learning_knowledge VALUES(?,?,?,?,?,?,?)').run(s.session.learnerId, s.session.goal.repositoryId,
        s.session.goal.sourceSnapshotHash, k.conceptId, k.level, s.evidenceKeys[k.conceptId]!, JSON.stringify({ knowledge: k, evidence: s.knowledgeEvidence[key] ?? {} }))
    }
  }
  private jobEvidence(s: Stored) {
    const read = sourceReader(s.project.sourceRoot)
    return Object.fromEntries(s.project.snapshot.files.map(f => [f.fileId, hash(read?.(f.fileId) ?? '<source-unavailable>')]))
  }
  private assertJobEvidence(s: Stored, job: Job) {
    if (JSON.stringify(this.jobEvidence(s)) !== JSON.stringify(job.sourceEvidence)) throw new Error('Source evidence changed after preparation was requested')
  }
  private exposed(s: Stored, q: QuizQuestion) {
    return (s.session.learnerId === 'local' && this.legacyExposed.has(fingerprint(q))) ||
      Boolean(this.db.prepare('SELECT 1 FROM learning_exposures WHERE learner=? AND repository=? AND fingerprint=?')
        .get(s.session.learnerId, s.session.goal.repositoryId, fingerprint(q)))
  }
  private fresh(s: Stored, item: CompiledItem) { return item.compiled.find(q => !this.exposed(s, q)) }
  private candidates(s: Stored, completingCurrent = false): CompiledItem[] {
    const needed = new Set(s.session.goal.targetConcepts.map(t => t.conceptId))
    for (let i = 0; i < 8; i++) for (const r of s.curriculum?.relations ?? []) if (needed.has(r.to)) needed.add(r.from)
    for (let i = 0; i < 8; i++) for (const a of s.items) if (a.effects.some(e => needed.has(e.conceptId)))
      for (const e of a.effects) for (const p of e.prerequisites ?? []) needed.add(p.conceptId)
    const skipped = new Set(s.session.history.filter(h => h.outcome === 'skip').map(h => h.step.conceptId))
    return s.items.filter(a => needed.has(a.conceptId) && !skipped.has(a.conceptId) &&
      s.session.history.filter(h => h.step.actionId === a.id).length + Number(completingCurrent && s.session.current?.actionId === a.id) < 2 &&
      (a.compiled.length === 0 || this.fresh(s, a)))
      .sort((a, b) => Number(s.session.goal.targetConcepts.some(t => t.conceptId === b.conceptId)) - Number(s.session.goal.targetConcepts.some(t => t.conceptId === a.conceptId)))
  }
  private decision(s: Stored, completingCurrent = false) {
    const current = s.items.find(a => a.id === s.session.current?.actionId)
    const budget = Math.max(0, s.session.goal.budget - s.session.spent - (completingCurrent ? current?.cost ?? 0 : 0))
    const candidates = this.candidates(s, completingCurrent)
    const selected = candidates.slice(0, 5).map(item => {
      const question = this.fresh(s, item)
      return { ...item, ...(question ? { observation: { conceptId: item.conceptId, level: item.level, guess: 1 / question.choices.length, slip: .1 } } : {}) }
    })
    const decision = chooseAction(s.session.goal, s.knowledge, selected, { depth: 3, maxNodes: 2000, maxMs: 150, remainingBudget: budget })
    if (candidates.length > 5) decision.truncated = true
    return decision
  }
  private move(s: Stored) {
    const decision = this.decision(s)
    const item = s.items.find(a => a.id === decision.actionId)
    if (!item) {
      s.session.current = null; delete s.questionId; delete s.responseId
      const skipped = new Set(s.session.history.filter(h => h.outcome === 'skip').map(h => h.step.conceptId))
      const unresolved = s.session.goal.targetConcepts.some(t => t.weight > 0 && !skipped.has(t.conceptId) &&
        (mastery(s.knowledge, t.conceptId, t.level) < .8 || (s.knowledge.find(k => k.conceptId === t.conceptId && k.level === t.level)?.observations ?? 0) < 2))
      s.session.status = s.session.spent >= s.session.goal.budget ? 'budget-exhausted' : unresolved ? 'generation-needed' : 'finished'
      return
    }
    s.session.current = { id: randomUUID(), actionId: item.id, nodeId: item.nodeId, conceptId: item.conceptId, level: item.level, kind: item.kind, caption: item.caption }
    s.questionId = this.fresh(s, item)?.id
    delete s.responseId
    s.session.status = 'active'
  }
  private view(s: Stored): LearningView {
    const item = s.items.find(i => i.id === s.session.current?.actionId)
    const question = !s.responseId && item?.compiled.find(q => q.id === s.questionId && !this.exposed(s, q))
    return { session: structuredClone(s.session), knowledge: structuredClone(s.knowledge), concepts: structuredClone(s.curriculum?.concepts ?? []),
      decision: s.curriculum ? this.decision(s, Boolean(s.session.current)) : null,
      question: question ? { id: question.id, question: question.question, choices: question.choices, difficulty: question.difficulty, concept: question.concept, evidence: question.evidence } : null,
      score: goalScore(s.session.goal, s.knowledge),
      reason: s.session.status === 'preparing' ? 'Preparing grounded learning materials.' :
        s.session.status === 'generation-needed' ? 'More learning material is needed; recorded answers are preserved.' :
        'Expected goal improvement under a provisional action-learning model; understanding needs independent evidence.' }
  }
  async start(project: Project, goal: LearningGoal, learnerId = 'local'): Promise<LearningView> {
    validateGoal(goal)
    if (goal.targetConcepts.length > 8 || goal.budget > 30 || !learnerId.trim() || learnerId.length > 200) throw new Error('Learning session exceeds supported limits')
    if (goal.sourceSnapshotHash !== (project.sourceSnapshotHash ?? project.hash)) throw new Error('Goal source snapshot does not match analysis')
    const stored: Stored = { project: structuredClone(project), curriculum: null, items: [], evidenceKeys: {}, knowledgeEvidence: {}, knowledge: [],
      session: { id: randomUUID(), revision: 1, learnerId, goal: structuredClone(goal), spent: 0, status: 'preparing', current: null, history: [],
        policyVersion: 'bounded-belief-search-v1', modelVersion: MODEL_VERSION } }
    return this.transaction(() => { this.save(stored, { type: 'started' }); return this.view(stored) })
  }
  async get(id: string): Promise<LearningView> { await this.legacy(); const s = this.load(id); this.refresh(s); return this.view(s) }
  async prepare(id: string, revision: number, curriculum: Curriculum, jobId?: string): Promise<LearningView> {
    await this.legacy()
    return this.transaction(() => {
      const s = this.load(id); this.assertRevision(s, revision)
      if (!['preparing', 'generation-needed'].includes(s.session.status)) throw new Error('Curriculum is frozen while the session is active')
      if (jobId) {
        const job = this.job(jobId)
        if (job.sessionId !== id || job.revision !== revision || job.status !== 'running') throw new Error('Stale preparation job')
        this.assertJobEvidence(s, job)
      }
      if (!curriculum || Object.keys(curriculum).some(k => !['version', 'concepts', 'items', 'relations'].includes(k)) ||
        curriculum.version !== (s.curriculum?.version ?? 0) + 1 || !curriculum.concepts?.length || curriculum.concepts.length > CURRICULUM_LIMITS.concepts || !curriculum.items?.length || curriculum.items.length > CURRICULUM_LIMITS.actions || !Array.isArray(curriculum.relations)) throw new Error('Invalid curriculum or evaluation data')
      if (s.curriculum) for (const item of s.items) this.assertEvidence(s, item)
      const ids = new Set<string>()
      for (const c of curriculum.concepts) {
        if (!c.id?.trim() || !c.name?.trim() || !c.definition?.trim() || !Number.isInteger(c.version) || c.version < 1 || ids.has(c.id)) throw new Error('Invalid/duplicate concept')
        if (s.curriculum && s.session.goal.targetConcepts.some(t => t.conceptId === c.id)) {
          const previous = s.curriculum.concepts.find(p => p.id === c.id)
          if (previous && (previous.version !== c.version || previous.definition !== c.definition)) throw new Error('Target definition is frozen for this goal revision')
        }
        ids.add(c.id)
        const existing = this.db.prepare('SELECT definition FROM learning_concepts WHERE repository=? AND id=? AND version=?').get(s.session.goal.repositoryId, c.id, c.version)
        if (existing && existing.definition !== c.definition) throw new Error('Concept definition changed without changing version')
        this.db.prepare('INSERT OR IGNORE INTO learning_concepts VALUES(?,?,?,?)').run(s.session.goal.repositoryId, c.id, c.version, c.definition)
      }
      if (s.session.goal.targetConcepts.some(t => !ids.has(t.conceptId))) throw new Error('Goal concept missing from curriculum')
      const itemIds = new Set<string>()
      const items: CompiledItem[] = curriculum.items.map(item => {
        if (!item.id?.trim() || itemIds.has(item.id) || !ids.has(item.conceptId) || !levels.includes(item.level) ||
          !['read', 'explain', 'compare', 'diagnose'].includes(item.kind) || !item.caption?.trim() || item.caption.length > CURRICULUM_LIMITS.chars.caption || !Number.isFinite(item.cost) || item.cost <= 0 || !Array.isArray(item.effects)) throw new Error('Invalid curriculum action/concept/level/cost')
        itemIds.add(item.id)
        buildQuizContext(s.project.snapshot, item.nodeId, item.level)
        const dimensions = item.effects.map(e => dim(e.conceptId, e.level))
        if (new Set(dimensions).size !== dimensions.length) throw new Error('Duplicate effect dimension')
        if (item.effects.some(e => !ids.has(e.conceptId) || !levels.includes(e.level) || (e.prerequisites ?? []).some(p => !ids.has(p.conceptId) || !levels.includes(p.level)))) throw new Error('Invalid effect concept/level')
        if (item.observation && (item.observation.conceptId !== item.conceptId || item.observation.level !== item.level)) throw new Error('Observation must match the action primary concept and level')
        if (!Array.isArray(item.questions) || item.questions.some(q => q.difficulty !== item.level)) throw new Error('Question level must match the action')
        const compiled = item.questions.length ? makeQuestions(s.project.snapshot, randomUUID(), item.nodeId, item.questions) : []
        const evidence = this.evidence(s.project, [item.nodeId, ...compiled.flatMap(q => q.evidence.map(e => e.nodeId))])
        // Reuse the optimizer's probability/cost validation on a single trusted-size input.
        chooseAction(s.session.goal, [], [item], { depth: 0, maxNodes: 1 })
        return { ...structuredClone(item), compiled, evidence }
      })
      for (const r of curriculum.relations) {
        if (!ids.has(r.from) || !ids.has(r.to) || r.from === r.to || !['prerequisite', 'similar'].includes(r.kind) || !r.reason?.trim() || !r.evidenceNodeIds?.length) throw new Error('Invalid learning relation')
        for (const n of r.evidenceNodeIds) buildQuizContext(s.project.snapshot, n, 'easy')
        this.evidence(s.project, r.evidenceNodeIds)
      }
      s.curriculum = structuredClone(curriculum); s.items = items
      for (const c of curriculum.concepts) s.evidenceKeys[c.id] = hash(JSON.stringify([c.id, c.version, c.definition]))
      s.knowledge = curriculum.concepts.flatMap(c => levels.map(level =>
        this.stored(s, c.id, level) ?? { conceptId: c.id, level, probability: PRIOR, observations: 0 }))
      s.session.revision++; this.move(s)
      this.save(s, { type: 'prepared', curriculumVersion: curriculum.version, jobId })
      if (jobId) { const job = this.job(jobId); job.status = 'ready'; this.saveJob(job)
        this.db.prepare('INSERT OR REPLACE INTO learning_material_cache VALUES(?,?)').run(this.materialKey(s, job), JSON.stringify(curriculum)) }
      return this.view(s)
    })
  }
  async respond(input: LearningAnswer): Promise<LearningResult> {
    await this.legacy()
    return this.transaction(() => {
      if (!/^[\w-]{1,100}$/.test(input.submissionId)) throw new Error('Invalid submission ID')
      const earlier = this.db.prepare('SELECT request,result FROM learning_submissions WHERE id=?').get(input.submissionId)
      const request = JSON.stringify([input.sessionId, input.revision, input.stopId, input.questionId, input.selectedIndex])
      if (earlier) {
        if (earlier.request !== request) throw new Error('Submission ID is already bound to a different answer')
        return JSON.parse(earlier.result as string)
      }
      const s = this.load(input.sessionId); this.assertRevision(s, input.revision)
      if (s.session.status !== 'active' || s.session.current?.id !== input.stopId || s.responseId) throw new Error('Stale learning step')
      const item = s.items.find(a => a.id === s.session.current!.actionId)!
      this.assertEvidence(s, item)
      const q = item.compiled.find(q => q.id === input.questionId && q.id === s.questionId)
      if (!q || this.exposed(s, q)) throw new Error('Question was exposed or is not the current diagnostic')
      if (!Number.isInteger(input.selectedIndex) || q.choices[input.selectedIndex] === undefined) throw new Error('Selected choice out of range')
      const correct = q.choices[input.selectedIndex] === q.answer
      const responseId = randomUUID()
      const observation = { conceptId: item.conceptId, level: item.level, guess: 1 / q.choices.length, slip: .1 }
      this.refresh(s)
      const before = s.knowledge
      // Acquisition is a provisional model estimate; only this answer adds an observation.
      s.knowledge = observe(predict(s.knowledge, item), observation, correct)
      this.persistKnowledge(s, item, before)
      this.db.prepare('INSERT INTO learning_exposures VALUES(?,?,?)').run(s.session.learnerId, s.session.goal.repositoryId, fingerprint(q))
      s.responseId = responseId; s.session.revision++
      this.save(s, { type: 'answered', responseId, stopId: input.stopId, questionId: q.id, fingerprint: fingerprint(q), correct, observation })
      const result: LearningResult = { view: this.view(s), responseId, counted: true,
        feedback: feedbackFor(q, { result: correct ? 'correct' : 'incorrect', attempt: 1 }) }
      this.db.prepare('INSERT INTO learning_submissions VALUES(?,?,?)').run(input.submissionId, request, JSON.stringify(result))
      return result
    })
  }
  async advance(id: string, revision: number, stopId: string, outcome: 'continue' | 'skip' | 'stop'): Promise<LearningView> {
    await this.legacy()
    return this.transaction(() => {
      const s = this.load(id); this.assertRevision(s, revision)
      if (outcome !== 'stop' && (s.session.status !== 'active' || s.session.current?.id !== stopId)) throw new Error('Stale learning step')
      if (outcome === 'stop' && stopId && s.session.current?.id !== stopId) throw new Error('Stale learning step')
      this.refresh(s)
      if (outcome === 'stop') {
        s.session.status = 'stopped'; s.session.current = null
        for (const row of this.db.prepare('SELECT data FROM learning_jobs').all()) {
          const job = JSON.parse(row.data as string) as Job
          if (job.sessionId === id && job.status === 'running') { job.status = 'failed'; job.error = 'Session ended'; this.saveJob(job) }
        }
      }
      else {
        if (!['continue', 'skip'].includes(outcome)) throw new Error('Invalid learning outcome')
        const item = s.items.find(a => a.id === s.session.current!.actionId)!
        this.assertEvidence(s, item)
        if (outcome === 'continue' && !s.responseId) { const before = s.knowledge; s.knowledge = predict(s.knowledge, item); this.persistKnowledge(s, item, before) }
        s.session.spent += item.cost
        s.session.history.push({ step: s.session.current!, outcome, ...(s.responseId ? { responseId: s.responseId } : {}) })
        s.session.current = null; delete s.responseId; delete s.questionId
        this.move(s)
      }
      s.session.revision++; this.save(s, { type: outcome, stopId })
      return this.view(s)
    })
  }
  private job(id: string): Job {
    const row = this.db.prepare('SELECT data FROM learning_jobs WHERE id=?').get(id)
    if (!row) throw new Error('Preparation job not found')
    return JSON.parse(row.data as string)
  }
  private saveJob(job: Job) { this.db.prepare('INSERT OR REPLACE INTO learning_jobs VALUES(?,?)').run(job.id, JSON.stringify(job)) }
  private materialKey(s: Stored, job: Job) {
    const g = s.session.goal
    return hash(JSON.stringify([g.repositoryId, g.sourceSnapshotHash, job.sourceEvidence, g.rawQuery, g.retrievalQuery,
      g.goalRevision, g.targetConcepts, g.budget, job.provider, 'learning-prompt-v1', job.baseVersion]))
  }
  /** `ownerPid` is the app process that will run the CLI when `shouldRun`; a crashed owner's job is expired rather than joined. */
  async requestPreparation(sessionId: string, revision: number, provider: string, ownerPid: number): Promise<PreparationJob> {
    if (!Number.isSafeInteger(ownerPid) || ownerPid <= 0) throw new Error('Invalid preparation owner pid')
    const result = this.transaction<PreparationJob>(() => {
      const s = this.load(sessionId)
      if (!['preparing', 'generation-needed', 'active'].includes(s.session.status)) throw new Error('Cannot prepare an ended learning session')
      if (!['codex', 'claude'].includes(provider)) throw new Error('Unknown AI provider')
      const jobs = this.db.prepare('SELECT data FROM learning_jobs ORDER BY rowid').all().map(r => JSON.parse(r.data as string) as Job)
        .filter(j => j.sessionId === sessionId)
      const baseVersion = s.curriculum?.version ?? 0
      const previous = jobs.filter(j => j.baseVersion === baseVersion).at(-1)
      if (previous?.status === 'running' && previous.revision === s.session.revision &&
        [previous.revision, previous.revision - 1].includes(revision) && Date.now() - previous.createdMs < 10 * 60_000 && alive(previous.ownerPid)) return { ...previous, shouldRun: false }
      // A duplicate queued behind the worker arrives after it finished; it shares that outcome instead of running again.
      // A ready job has already advanced the curriculum version, so look at the session's latest job.
      const last = jobs.at(-1)
      if ((last?.status === 'ready' || last?.status === 'failed') && [last.revision, last.revision - 1].includes(revision)) return { ...last, shouldRun: false }
      this.assertRevision(s, revision)
      if (s.session.status === 'active') return { ...(previous ?? { id: randomUUID(), sessionId, revision, attempt: 0 }), status: 'ready', shouldRun: false }
      if (jobs.length >= 3 || baseVersion >= 2 || (previous && previous.attempt >= 2)) {
        if (previous?.status === 'running') { previous.status = 'failed'; previous.error = 'Worker lease expired'; this.saveJob(previous) }
        if (s.session.status === 'preparing') { s.session.status = 'generation-needed'; s.session.revision++; this.save(s, { type: 'generation-recovery-exhausted' }) }
        return { ...(previous ?? { id: randomUUID(), sessionId, revision, attempt: 0 }), status: 'failed', shouldRun: false, error: 'AI preparation budget exhausted' }
      }
      if (previous?.status === 'running') { previous.status = 'failed'; previous.error = 'Worker lease expired'; this.saveJob(previous) }
      s.session.status = 'preparing'; s.session.revision++
      const job: Job = { id: randomUUID(), sessionId, revision: s.session.revision, status: 'running', attempt: (previous?.attempt ?? 0) + 1,
        shouldRun: true, provider, createdMs: Date.now(), ownerPid, baseVersion, sourceEvidence: this.jobEvidence(s) }
      this.saveJob(job); this.save(s, { type: 'generation-requested', jobId: job.id })
      return job
    })
    if (result.shouldRun) {
      const s = this.load(sessionId), job = this.job(result.id)
      const cached = this.db.prepare('SELECT data FROM learning_material_cache WHERE key=?').get(this.materialKey(s, job))
      if (cached) {
        try {
          await this.prepare(sessionId, job.revision, JSON.parse(cached.data as string), job.id)
          return { ...job, status: 'ready', shouldRun: false }
        } catch {
          // The job is already reserved for this caller; an unusable cache entry must not strand it.
          // Drop the entry and let the CLI generate fresh material for the same job.
          this.db.prepare('DELETE FROM learning_material_cache WHERE key=?').run(this.materialKey(s, job))
        }
      }
    }
    return result
  }
  async finishPreparation(id: string, error?: string): Promise<void> {
    this.transaction(() => {
      const job = this.job(id)
      if (job.status === 'ready') return
      if (job.status !== 'running') return
      const s = this.load(job.sessionId)
      job.status = 'failed'; job.error = error ?? 'AI finished without saving a curriculum'; this.saveJob(job)
      if (s.session.revision === job.revision) { s.session.status = 'generation-needed'; s.session.revision++; this.save(s, { type: 'generation-failed', jobId: id, error: job.error }) }
    })
  }
  /** Goal-ranked candidate nodes with their own source and, per level, exactly the evidence nodes the validator accepts. */
  private candidateContexts(s: Stored) {
    const { snapshot } = s.project
    const read = sourceReader(s.project.sourceRoot)
    // The session's reading order is the goal query's ranking; unranked files only fill up a short list.
    const ranked = snapshot.readingSequence.filter(r => r.entityType === 'file').map(r => `file:${r.fileId}`)
    const candidates = [...new Set([...ranked, ...snapshot.files.map(f => `file:${f.fileId}`)])].slice(0, PREPARATION_CANDIDATES)
    return candidates.map(nodeId => {
      const own = buildQuizContext(snapshot, nodeId, 'easy', read)
      const evidence = Object.fromEntries(DIFFICULTIES.map(level => [level, buildQuizContext(snapshot, nodeId, level).nodes.map(n => n.nodeId)]))
      const anchor = own.nodes.find(n => n.nodeId === nodeId)!
      return { nodeId, title: anchor.title, facts: anchor.facts, excerpts: own.excerpts.map(e => clip(e, CANDIDATE_SOURCE_CHARS)), evidence }
    })
  }
  async context(id: string) {
    const s = this.load(id)
    const contexts = this.candidateContexts(s)
    const out = { sessionId: s.session.id, revision: s.session.revision, goal: s.session.goal, rules: PREPARATION_RULES, contexts,
      priorConcepts: s.curriculum?.concepts ?? s.session.goal.targetConcepts.flatMap(t => {
        const row = this.db.prepare('SELECT id,version,definition FROM learning_concepts WHERE repository=? AND id=? ORDER BY version DESC LIMIT 1')
          .get(s.session.goal.repositoryId, t.conceptId)
        return row ? [{ id: row.id as string, name: row.id as string, version: row.version as number, definition: row.definition as string }] : []
      }), nextCurriculumVersion: (s.curriculum?.version ?? 0) + 1, modelAssumptions: MODEL_VERSION }
    // Fit the budget by shortening, then dropping, the least relevant source; evidence lists always stay with their node.
    const size = () => JSON.stringify(out).length + JOB_SUMMARY_CHARS
    for (let i = contexts.length - 1; size() > PREPARATION_CONTEXT_CHARS && i >= 0; i--) {
      const over = size() - PREPARATION_CONTEXT_CHARS
      const own = contexts[i]!.excerpts.reduce((n, e) => n + e.text.length, 0)
      contexts[i]!.excerpts = contexts[i]!.excerpts.map(e => clip(e, Math.max(0, Math.floor(e.text.length * Math.max(0, own - over) / Math.max(1, own)) - 200))).filter(e => e.text)
    }
    while (size() > PREPARATION_CONTEXT_CHARS && contexts.length > 1) contexts.pop()
    return out
  }
  async jobContext(id: string) {
    const job = this.job(id)
    const s = this.load(job.sessionId)
    if (job.status !== 'running' || job.revision !== s.session.revision) throw new Error('Stale preparation job')
    this.assertJobEvidence(s, job)
    // Per-file source hashes stay internal; the agent only needs to know which job it is writing for.
    return { job: { id: job.id, status: job.status, attempt: job.attempt }, ...await this.context(job.sessionId) }
  }
  async saveJobCurriculum(jobId: string, curriculum: Curriculum) {
    const job = this.job(jobId)
    return this.prepare(job.sessionId, job.revision, curriculum, jobId)
  }
}
