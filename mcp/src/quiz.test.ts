import { test } from 'node:test'
import assert from 'node:assert/strict'
import { mkdtemp, readFile } from 'node:fs/promises'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { Client } from '@modelcontextprotocol/sdk/client/index.js'
import { StdioClientTransport } from '@modelcontextprotocol/sdk/client/stdio.js'
import { TourStore } from './store.ts'
import { QuizStore, RESPONSES_FILE } from './quiz.ts'
import { buildQuizContext, type QuestionInput } from '../../frontend/src/quiz/model.ts'

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
  assert.throws(() => buildQuizContext(snapshot, 'member:x', 'easy'), /Unsupported quiz node/)
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
    const saved = await call('create_quiz', { projectId, tourId: draft.plan.id, tourRevision: 1, stopIndex: 0, nodeId: 'root', questions: [easy] })
    const [set] = await call('list_quizzes', {})
    assert.equal(set.id, saved.quizSetId)
    const answer = await call('record_quiz_response', { quizSetId: set.id, quizId: set.questions[0].id, selectedIndex: 0, responseTimeMs: 1500 })
    assert.equal(answer.feedback.correct, true)
    assert.equal((await call('list_quiz_responses', { quizSetId: set.id })).length, 1)
  } finally { await client.close() }
})
