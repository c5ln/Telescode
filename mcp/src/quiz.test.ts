import { test } from 'node:test'
import assert from 'node:assert/strict'
import { copyFile, mkdir, mkdtemp, readFile, writeFile } from 'node:fs/promises'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { Client } from '@modelcontextprotocol/sdk/client/index.js'
import { StdioClientTransport } from '@modelcontextprotocol/sdk/client/stdio.js'
import { TourStore } from './store.ts'
import { QuizStore, RESPONSES_FILE, sourceReader } from './quiz.ts'
import { buildQuizContext, buildQuizContexts, classLines, longAnswers, SCOPE, type QuestionInput, type SourceExcerpt } from '../../frontend/src/quiz/model.ts'

const fixture = fileURLToPath(new URL('../../frontend/src/graph/fixtures/sherlock.graph.json', import.meta.url))
const snapshot = { ...JSON.parse(await readFile(fixture, 'utf8')), readingSequence: [] }
const notifyPrint = 'class:sherlock_project/notify.py::QueryNotifyPrint'

test('quiz context widens with difficulty and keeps members as facts', () => {
  const easy = buildQuizContext(snapshot, notifyPrint, 'easy')
  assert.deepEqual(easy.nodes.map(n => n.nodeId), [notifyPrint])
  assert.ok(easy.nodes[0]!.facts.some(f => f.startsWith('Method ')))
  const medium = buildQuizContext(snapshot, notifyPrint, 'medium')
  const ids = medium.nodes.map(n => n.nodeId)
  assert.ok(ids.includes('class:sherlock_project/notify.py::QueryNotify'))
  assert.ok(ids.includes('file:sherlock_project/notify.py'))
  assert.ok(medium.relations.some(r => r.kind === 'class-dependency' && r.source === notifyPrint))
  const hard = buildQuizContext(snapshot, notifyPrint, 'hard')
  assert.ok(hard.nodes.length > medium.nodes.length)
  assert.ok(hard.nodes.every(n => n.hop <= 2))
  assert.deepEqual(hard.learning, { before: [], after: [] }) // No reading sequence in a graph-only snapshot.
  assert.throws(() => buildQuizContext(snapshot, 'member:x', 'easy'), /Unsupported quiz node: member:x$/)
  // A file path without its prefix names the ID it was meant to be, so the writer fixes it in one turn.
  assert.throws(() => buildQuizContext(snapshot, 'sherlock_project/notify.py', 'easy'), /Did you mean file:sherlock_project\/notify\.py\?/)
  // Questions are about what code does, so sizes, metrics and ranks are not offered.
  const facts = hard.nodes.flatMap(n => n.facts).join('\n')
  assert.doesNotMatch(facts, /complexity|lines|rank|imported by|contains \d+/i)
})

const notify = [
  'from result import QueryStatus',
  '',
  'class QueryNotify:',
  '    def __init__(self, result=None):',
  '        self.result = result',
  '',
  '    def update(self, result):',
  '        self.result = result',
  '',
  'class QueryNotifyPrint(QueryNotify):',
  '    def update(self, result):',
  '        if result.status == QueryStatus.CLAIMED:',
  '            print(result.site_name)',
  '',
  'def helper():',
  '    return 1',
].join('\n')

test('quiz context carries numbered source: the class body, then the code it relates to', async () => {
  const root = await mkdtemp(join(tmpdir(), 'telescode-quiz-source-'))
  await mkdir(join(root, 'sherlock_project'))
  await writeFile(join(root, 'sherlock_project/notify.py'), notify)
  const read = sourceReader(root)!

  const easy = buildQuizContext(snapshot, notifyPrint, 'easy', read)
  assert.deepEqual(easy.excerpts.map(e => [e.nodeId, e.startLine, e.endLine, e.truncated]), [[notifyPrint, 10, 13, false]])
  assert.equal(easy.excerpts[0]!.text.split('\n')[0], '10| class QueryNotifyPrint(QueryNotify):')
  assert.doesNotMatch(easy.excerpts[0]!.text, /def helper/)

  const medium = buildQuizContext(snapshot, notifyPrint, 'medium', read)
  assert.ok(medium.excerpts.some(e => e.nodeId === 'class:sherlock_project/notify.py::QueryNotify' || e.nodeId === 'file:sherlock_project/notify.py'))
  assert.ok(medium.excerpts.reduce((n, e) => n + e.text.length, 0) <= 20_000)

  // Files that are missing or outside the source folder are never read.
  assert.equal(read('../outside.py'), null)
  assert.equal(read('sherlock_project/missing.py'), null)
  assert.deepEqual(buildQuizContext(snapshot, notifyPrint, 'easy').excerpts, [])
})

/** Every numbered excerpt line, as [fileId, line number, text], checked against the source. */
function excerptLines(excerpts: SourceExcerpt[], sources: Record<string, string>) {
  return excerpts.flatMap(e => e.text.split('\n').flatMap(line => {
    const m = /^(\d+)\| (.*)$/.exec(line)
    if (!m) { assert.match(line, /^…\| lines \d+–\d+ omitted/); return [] }
    const n = Number(m[1])
    assert.equal(m[2], sources[e.fileId]!.split('\n')[n - 1]!.trimEnd(), `${e.fileId}:${n} is the real source line`)
    assert.ok(n >= e.startLine && n <= e.endLine)
    return [[e.fileId, n, m[2]] as const]
  }))
}

test('one context for easy and medium: the same evidence per level, source sent once and smaller than two contexts', async () => {
  const root = await mkdtemp(join(tmpdir(), 'telescode-quiz-multi-'))
  await mkdir(join(root, 'sherlock_project'))
  await writeFile(join(root, 'sherlock_project/notify.py'), notify)
  const read = sourceReader(root)!
  for (const nodeId of [notifyPrint, 'file:sherlock_project/notify.py', 'root']) {
    const easy = buildQuizContext(snapshot, nodeId, 'easy', read)
    const medium = buildQuizContext(snapshot, nodeId, 'medium', read)
    const both = buildQuizContexts(snapshot, nodeId, ['medium', 'easy'], read)
    assert.deepEqual(both.difficulties, ['easy', 'medium'])
    // Exactly the nodes the validator accepts per level, and every one of them is described.
    assert.deepEqual(both.evidence.easy, easy.nodes.map(n => n.nodeId))
    assert.deepEqual(both.evidence.medium, medium.nodes.map(n => n.nodeId))
    const described = new Set(both.nodes.map(n => n.nodeId))
    assert.equal(described.size, both.nodes.length)
    for (const id of [...both.evidence.easy!, ...both.evidence.medium!]) assert.ok(described.has(id), id)
    // No source line twice, and every line is the real one.
    const lines = excerptLines(both.excerpts, { 'sherlock_project/notify.py': notify })
    assert.equal(new Set(lines.map(([f, n]) => `${f}:${n}`)).size, lines.length)
    // Baseline: what the agent received before, one context per level.
    const before = JSON.stringify(easy).length + JSON.stringify(medium).length
    const after = JSON.stringify(both).length
    assert.ok(after < before, `${nodeId}: ${after} < ${before}`)
  }
  assert.throws(() => buildQuizContexts(snapshot, notifyPrint, []), /Unknown difficulties/)
})

test('a file too long for the budget keeps its header and the definitions the stop uses, whole, with real line numbers', async () => {
  const root = await mkdtemp(join(tmpdir(), 'telescode-quiz-long-'))
  await mkdir(join(root, 'sherlock_project'))
  const filler = (i: number) => [`def unrelated_${i}(value):`, ...Array.from({ length: 30 }, (_, j) => `    value = value + ${j}  # step ${j} of a long unrelated helper`), '    return value', '']
  const long = [
    'import os',
    'from result import QueryStatus',
    '',
    ...Array.from({ length: 40 }, (_, i) => filler(i)).flat(),
    'def claimed(result):',
    '    return result.status == QueryStatus.CLAIMED',
    '',
    'class QueryNotify:',
    '    def update(self, result):',
    '        self.result = result',
    '',
    'class QueryNotifyPrint(QueryNotify):',
    '    def update(self, result):',
    '        if claimed(result):',
    '            print(result.site_name)',
    '',
  ].join('\n')
  await writeFile(join(root, 'sherlock_project/notify.py'), long)
  const read = sourceReader(root)!
  const sources = { 'sherlock_project/notify.py': long }
  const file = 'file:sherlock_project/notify.py'

  const easy = buildQuizContext(snapshot, file, 'easy', read)
  assert.ok(long.length > SCOPE.easy.sourceChars)
  const text = easy.excerpts.map(e => e.text).join('\n')
  assert.ok(text.length <= SCOPE.easy.sourceChars)
  assert.ok(easy.excerpts[0]!.truncated)
  assert.match(text, /^1\| import os$/m)
  // The classes the context names are kept whole; unrelated helpers are named, not shown.
  assert.match(text, /\| class QueryNotifyPrint\(QueryNotify\):\n\d+\|     def update\(self, result\):\n\d+\|         if claimed\(result\):/)
  assert.match(text, /…\| lines \d+–\d+ omitted: unrelated_\d+\(\)/)
  const lines = excerptLines(easy.excerpts, sources)
  assert.equal(new Set(lines.map(([f, n]) => `${f}:${n}`)).size, lines.length)

  // From the class, its file adds what the class uses (`claimed`) before unrelated code, and never repeats the class.
  const both = buildQuizContexts(snapshot, notifyPrint, ['easy', 'medium'], read)
  assert.equal(both.excerpts[0]!.nodeId, notifyPrint)
  const fileExcerpt = both.excerpts.find(e => e.nodeId === file)!
  assert.match(fileExcerpt.text, /\| def claimed\(result\):/)
  const all = excerptLines(both.excerpts, sources)
  assert.equal(new Set(all.map(([f, n]) => `${f}:${n}`)).size, all.length)
  assert.ok(both.excerpts.reduce((n, e) => n + e.text.length, 0) <= SCOPE.medium.sourceChars)
  // Related code gets a share of the budget per file, not all that is left, and once cut keeps only what the class uses.
  const perFile = new Map<string, number>()
  for (const e of both.excerpts.slice(1)) perFile.set(e.fileId, (perFile.get(e.fileId) ?? 0) + e.text.length)
  for (const [f, n] of perFile) assert.ok(n <= SCOPE.medium.sourceChars / 4, `${f} within its share`)
  assert.doesNotMatch(fileExcerpt.text, /\| def unrelated_/)

  // Adaptive candidates choose within their own, smaller share.
  const small = buildQuizContext(snapshot, file, 'easy', read, { sourceChars: 2_000 })
  assert.ok(small.excerpts.reduce((n, e) => n + e.text.length, 0) <= 2_000)
  assert.match(small.excerpts.map(e => e.text).join('\n'), /^1\| import os$/m)
  excerptLines(small.excerpts, sources)
})

test('code without definitions keeps its first lines; a missing file is skipped', async () => {
  const root = await mkdtemp(join(tmpdir(), 'telescode-quiz-flat-'))
  await mkdir(join(root, 'sherlock_project'))
  const flat = Array.from({ length: 2_000 }, (_, i) => `VALUE_${i} = ${i}`).join('\n')
  await writeFile(join(root, 'sherlock_project/notify.py'), flat)
  const read = sourceReader(root)!
  const easy = buildQuizContext(snapshot, 'file:sherlock_project/notify.py', 'easy', read)
  assert.equal(easy.excerpts[0]!.startLine, 1)
  assert.ok(easy.excerpts[0]!.truncated)
  assert.ok(easy.excerpts[0]!.text.length <= SCOPE.easy.sourceChars)
  excerptLines(easy.excerpts, { 'sherlock_project/notify.py': flat })
  // One definition too long for the budget, and a final newline: its first lines, not an empty line after an omission.
  const one = `class QueryNotify:\n${Array.from({ length: 2_000 }, (_, i) => `    value_${i} = ${i}`).join('\n')}\n`
  const single = buildQuizContext(snapshot, 'file:sherlock_project/notify.py', 'easy', () => one)
  assert.equal(single.excerpts[0]!.text.split('\n')[0], '1| class QueryNotify:')
  assert.ok(single.excerpts[0]!.text.length > SCOPE.easy.sourceChars / 2)
  excerptLines(single.excerpts, { 'sherlock_project/notify.py': one })
  // Only some of the context's files exist: the rest is left out, nothing fails.
  const onlyNotify = (fileId: string) => (fileId === 'sherlock_project/notify.py' ? notify : null)
  const sparse = buildQuizContexts(snapshot, 'file:sherlock_project/notify.py', ['easy', 'medium'], onlyNotify)
  assert.ok(sparse.excerpts.some(e => e.nodeId === 'file:sherlock_project/notify.py'), 'the readable file is excerpted')
  assert.ok(sparse.excerpts.every(e => e.fileId === 'sherlock_project/notify.py'))
  assert.ok(sparse.nodes.some(n => n.nodeId.startsWith('file:') && n.nodeId !== 'file:sherlock_project/notify.py'), 'the context also names unreadable files')
  assert.deepEqual(buildQuizContexts(snapshot, notifyPrint, ['easy', 'medium'], () => null).excerpts, [])
})

test('a signature over several lines, closing at the definition\'s indent, keeps its body', async () => {
  const lines = [
    'class QueryNotifyPrint(',
    '    QueryNotify,',
    '):',
    '    def update(',
    '        self,',
    '        result,',
    '    ) -> None:',
    '        if claimed(result):',
    '            print(result.site_name)',
    '',
    'def claimed(',
    '    result,',
    ') -> bool:',
    '    return result.status == QueryStatus.CLAIMED',
  ]
  assert.deepEqual(classLines(lines, 'QueryNotifyPrint'), [0, 8])

  // A header too long to come first: the class the context names still fits, whole, and the header comes after it.
  const imports = Array.from({ length: 40 }, (_, i) => `from module_${i} import name_${i}`)
  const source = [...imports, '', ...lines].join('\n')
  const read = (fileId: string) => (fileId === 'sherlock_project/notify.py' ? source : null)
  const sources = { 'sherlock_project/notify.py': source }
  const file = 'file:sherlock_project/notify.py'
  const head = imports.map((l, i) => `${i + 1}| ${l}`).join('\n').length
  const cls = lines.slice(0, 9).map((l, i) => `${i + 42}| ${l}`).join('\n').length
  const budget = head + cls - 50
  const small = buildQuizContext(snapshot, file, 'easy', read, { sourceChars: budget })
  const text = small.excerpts.map(e => e.text).join('\n')
  assert.ok(text.length <= budget)
  for (const line of ['class QueryNotifyPrint(', '):', '    def update(', '    ) -> None:', '        if claimed(result):', '            print(result.site_name)']) {
    assert.ok(text.includes(`| ${line}\n`) || text.endsWith(`| ${line}`), `keeps ${JSON.stringify(line)}`)
  }
  excerptLines(small.excerpts, sources)

  // From the class, the multi-line helper it uses comes whole from its file.
  const both = buildQuizContexts(snapshot, notifyPrint, ['easy', 'medium'], read)
  const all = both.excerpts.map(e => e.text).join('\n')
  for (const line of ['def claimed(', ') -> bool:', '    return result.status == QueryStatus.CLAIMED']) assert.ok(all.includes(`| ${line}`), `keeps ${JSON.stringify(line)}`)
  excerptLines(both.excerpts, sources)
})

test('a stop whose own source cannot be found gives no related file more than its share', async () => {
  // The stop's class is missing from its file, so the large class read first is only related code.
  const methods = Array.from({ length: 60 }, (_, i) => [`    def step_${i}(self, value):`, `        return value + ${i}  # one step of a long related class`, ''])
  const source = ['class QueryNotify:', ...methods.flat()].join('\n')
  assert.ok(source.length > SCOPE.medium.sourceChars / 4)
  const read = (fileId: string) => (fileId === 'sherlock_project/notify.py' ? source : null)
  for (const context of [buildQuizContext(snapshot, notifyPrint, 'medium', read), buildQuizContexts(snapshot, notifyPrint, ['easy', 'medium'], read)]) {
    const perFile = new Map<string, number>()
    for (const e of context.excerpts) perFile.set(e.fileId, (perFile.get(e.fileId) ?? 0) + e.text.length)
    for (const [f, n] of perFile) assert.ok(n <= SCOPE.medium.sourceChars / 4, `${f}: ${n} within its share`)
    excerptLines(context.excerpts, { 'sherlock_project/notify.py': source })
  }
})

test('a database scanned by the app has its source folder found beside it', async () => {
  const dir = await mkdtemp(join(tmpdir(), 'telescode-quiz-db-'))
  const db = join(dir, 'sherlock.graph.json')
  await copyFile(fixture, db)
  const tours = new TourStore(dir)
  assert.equal((await tours.open(db)).sourceRoot, undefined)
  await mkdir(join(dir, 'checkout'))
  await writeFile(`${db}.source`, join(dir, 'checkout'))
  assert.equal((await tours.open(db)).sourceRoot, join(dir, 'checkout'))
})

const easy: QuestionInput = { type: 'true_false', difficulty: 'easy', concept: 'Repository layout', question: 'The repository has a tests directory.', answer: 'True', distractors: [], explanation: 'tests is a top-level directory.', evidenceNodeIds: ['root', 'dir:tests'] }
const medium: QuestionInput = { type: 'multiple_choice', difficulty: 'medium', concept: 'Packages', question: 'Which directory holds the sherlock package?', answer: 'sherlock_project', distractors: ['devel', 'tests'], explanation: 'sherlock_project contains sherlock.py.', evidenceNodeIds: ['root', 'dir:sherlock_project'] }

test('quizzes are saved per tour stop, refuse ungrounded evidence, and record graded answers', async () => {
  const output = await mkdtemp(join(tmpdir(), 'telescode-quiz-'))
  const tours = new TourStore(output)
  const quizzes = new QuizStore(output, tours)
  const project = await tours.open(fixture)
  const tour = await tours.create(project.id, 'Quiz tour', 'en', [{ nodeId: 'root', caption: 'Overview', transitionMs: 0, holdMs: 2000 }])
  const at = { tourId: tour.plan.id, tourRevision: 1, stopIndex: 0, nodeId: 'root' }

  await assert.rejects(quizzes.create(project, at, 'en', [{ ...easy, evidenceNodeIds: ['root', 'file:sherlock_project/sherlock.py'] }]), /outside the easy context/)
  await assert.rejects(quizzes.create(project, at, 'en', [{ ...easy, evidenceNodeIds: ['dir:tests'] }]), /must include the step's node/)
  await assert.rejects(quizzes.create(project, at, 'en', [{ ...medium, evidenceNodeIds: ['root'] }]), /at least 2 nodes/)
  await assert.rejects(quizzes.create(project, at, 'en', [{ ...medium, distractors: ['devel', 'Sherlock_Project'] }]), /choices must all differ/)
  // An answer whose length alone gives it away is noted, not refused: a refusal would cost the writer another turn.
  const telling = { ...medium, answer: 'sherlock_project, the package that holds the command line entry point and its site checks', distractors: ['devel, the scripts', 'tests, the unit tests'] }
  assert.deepEqual(longAnswers([telling, medium]), ['Question 1: answer of 89 characters, longest distractor 21'])
  await assert.rejects(quizzes.create(project, { ...at, stopIndex: 1 }, 'en', [easy]), /is not root/)
  await assert.rejects(quizzes.create(project, { ...at, tourRevision: 2 }, 'en', [easy]), /revision 1/)

  const set = await quizzes.create(project, at, 'en', [easy, medium])
  assert.deepEqual((await quizzes.list()).map(s => s.id), [set.id])
  const [tf, mc] = set.questions
  assert.deepEqual(tf!.choices, ['True', 'False'])
  assert.deepEqual([...mc!.choices].sort(), ['devel', 'sherlock_project', 'tests'])
  assert.deepEqual(mc!.evidence, [{ nodeId: 'root', title: 'Repository' }, { nodeId: 'dir:sherlock_project', title: 'sherlock_project' }])

  const wrong = await quizzes.respond(set.id, mc!.id, mc!.choices.indexOf('devel'), 4200.4)
  assert.equal(wrong.feedback.correct, false)
  assert.equal(wrong.feedback.answer, 'sherlock_project')
  const right = await quizzes.respond(set.id, mc!.id, mc!.choices.indexOf('sherlock_project'), null)
  assert.equal(right.feedback.correct, true)
  assert.equal(right.feedback.attempt, 2)
  await assert.rejects(quizzes.respond(set.id, mc!.id, 7, null), /out of range/)

  const log = (await readFile(join(output, RESPONSES_FILE), 'utf8')).trim().split('\n').map(l => JSON.parse(l))
  assert.equal(log.length, 2)
  assert.deepEqual(
    { ...log[0], id: undefined, timestamp: undefined },
    { schemaVersion: 1, id: undefined, quizId: mc!.id, quizSetId: set.id, concept: 'Packages', difficulty: 'medium', type: 'multiple_choice', result: 'incorrect', selectedAnswer: 'devel', attempt: 1, tourContext: at, snapshotHash: project.hash, relatedNodes: ['root', 'dir:sherlock_project'], responseTimeMs: 4200, timestamp: undefined },
  )
  assert.ok(!Number.isNaN(Date.parse(log[0].timestamp)))
  assert.deepEqual((await quizzes.responses(set.id)).map(r => r.result), ['incorrect', 'correct'])
})

test('MCP protocol: quiz context, saving and answering a quiz', { timeout: 60000 }, async () => {
  const cwd = fileURLToPath(new URL('..', import.meta.url))
  const output = await mkdtemp(join(tmpdir(), 'telescode-quiz-protocol-'))
  const env = Object.fromEntries(Object.entries(process.env).filter((pair): pair is [string, string] => pair[1] !== undefined))
  const client = new Client({ name: 'quiz-test', version: '1.0.0' })
  await client.connect(new StdioClientTransport({ command: process.execPath, args: ['--import', 'tsx', 'src/server.ts'], cwd, env: { ...env, TELESCODE_TOUR_OUTPUT: output }, stderr: 'pipe' }))
  try {
    assert.match(client.getInstructions() ?? '', /quiz instructions/)
    async function call(name: string, args: Record<string, unknown>) {
      const response = await client.callTool({ name, arguments: args })
      assert.ok(!response.isError, JSON.stringify(response))
      return JSON.parse((response.content as { text: string }[])[0]!.text)
    }
    const { projectId } = await call('open_project', { path: fixture })
    const draft = await call('create_tour_draft', { projectId, title: 'Quiz tour', language: 'en', stops: [{ nodeId: 'root', caption: 'Overview', transitionMs: 0, holdMs: 2000 }] })
    const context = await call('get_quiz_context', { projectId, nodeId: 'root', difficulty: 'easy' })
    assert.ok(context.nodes.some((n: { nodeId: string }) => n.nodeId === 'dir:tests'))
    const both = await call('get_quiz_context', { projectId, nodeId: 'root', difficulties: ['easy', 'medium'] })
    assert.deepEqual(both.evidence.easy, context.nodes.map((n: { nodeId: string }) => n.nodeId))
    assert.deepEqual(both.difficulties, ['easy', 'medium'])
    const neither = await client.callTool({ name: 'get_quiz_context', arguments: { projectId, nodeId: 'root' } })
    assert.ok(neither.isError)
    const saved = await call('create_quiz', { projectId, tourId: draft.plan.id, tourRevision: 1, stopIndex: 0, nodeId: 'root', questions: [easy] })
    const [set] = await call('list_quizzes', {})
    assert.equal(set.id, saved.quizSetId)
    const answer = await call('record_quiz_response', { quizSetId: set.id, quizId: set.questions[0].id, selectedIndex: 0, responseTimeMs: 1500 })
    assert.equal(answer.feedback.correct, true)
    assert.equal((await call('list_quiz_responses', { quizSetId: set.id })).length, 1)
  } finally { await client.close() }
})
