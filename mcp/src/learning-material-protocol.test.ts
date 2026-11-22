import { test } from 'node:test'
import assert from 'node:assert/strict'
import { mkdtemp, mkdir, writeFile } from 'node:fs/promises'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { Client } from '@modelcontextprotocol/sdk/client/index.js'
import { StdioClientTransport } from '@modelcontextprotocol/sdk/client/stdio.js'

const notify = 'file:sherlock_project/notify.py'
const result = 'file:sherlock_project/result.py'
const ANSWER = 'So the receiver can inspect the outcome'
/** A question as the agent writes it. */
const question = (level: string, suffix: string) => ({ type: 'multiple_choice', difficulty: level, concept: 'Notification state',
  question: `(${level}) Why preserve the result before notifying? ${suffix}`, answer: ANSWER,
  distractors: ['To start another network request', 'To mutate the site configuration'],
  explanation: 'The stored result supplies the outcome to notification consumers.', evidenceNodeIds: level === 'easy' ? [notify] : [notify, result] })
/** An action as the agent writes it: no cost or learning effects, which the server assigns. */
const action = (id: string, level: string, suffixes: string[]) => ({ id, kind: level === 'easy' ? 'read' : 'explain', nodeId: notify, conceptId: 'A', level,
  caption: `Study the notifier at the ${level} level.`, questions: suffixes.map(s => question(level, s)) })

test('the app asks for material while the learner answers and the agent writes just the asked levels', { timeout: 30000 }, async () => {
  const cwd = fileURLToPath(new URL('..', import.meta.url))
  const dir = await mkdtemp(join(tmpdir(), 'telescode-material-protocol-'))
  const source = join(dir, 'source'); await mkdir(join(source, 'sherlock_project'), { recursive: true })
  await writeFile(join(source, 'sherlock_project/notify.py'), 'class QueryNotify:\n    def update(self, result):\n        self.result = result\n')
  await writeFile(join(source, 'sherlock_project/result.py'), 'class QueryResult:\n    def __init__(self, status):\n        self.status = status\n')
  const client = new Client({ name: 'material-test', version: '1' })
  const env = Object.fromEntries(Object.entries(process.env).filter((v): v is [string, string] => v[1] !== undefined))
  const transport = new StdioClientTransport({ command: process.execPath, args: ['--import', 'tsx', 'src/server.ts'], cwd,
    env: { ...env, TELESCODE_TOUR_OUTPUT: join(dir, 'artifacts') }, stderr: 'pipe' })
  let stderr = ''; transport.stderr?.on('data', chunk => { stderr += chunk.toString() })
  try { await client.connect(transport) } catch (error) { throw new Error(`${String(error)}\n${stderr}`) }
  try {
    async function call(name: string, args: Record<string, unknown>) {
      const out = await client.callTool({ name, arguments: args })
      assert.ok(!out.isError, JSON.stringify(out))
      return JSON.parse((out.content as { text: string }[])[0]!.text)
    }
    const project = await call('open_project', { path: fileURLToPath(new URL('../../frontend/src/graph/fixtures/sherlock.graph.json', import.meta.url)), sourceRoot: source })
    const goal = { goalId: 'notify', goalRevision: 1, rawQuery: 'Understand notification flow', retrievalQuery: 'notification result update', budget: 12,
      targetConcepts: ['easy', 'medium', 'hard'].map(level => ({ conceptId: 'A', level, weight: 1 })) }
    const started = await call('start_learning_session', { projectId: project.projectId, goal })
    const prep = await call('request_learning_preparation', { sessionId: started.session.id, revision: started.session.revision, provider: 'claude', ownerPid: process.pid })
    await call('save_learning_curriculum', { jobId: prep.id, curriculum: { version: 1, relations: [],
      concepts: [{ id: 'A', name: 'Notification state', definition: 'Explain how notification consumers receive the search outcome', version: 1 }],
      items: [action('easy-A', 'easy', ['first', 'second'])] } })
    await call('finish_learning_preparation', { jobId: prep.id })
    let view = await call('get_learning_session', { sessionId: started.session.id })

    const respond = async (right: boolean) => {
      const s = view.session
      const correct = view.question.choices.indexOf(ANSWER)
      const answered = await call('record_learning_response', { sessionId: s.id, revision: s.revision, stopId: s.current.id, submissionId: `p-${s.revision}`,
        questionId: view.question.id, selectedIndex: right ? correct : (correct + 1) % view.question.choices.length })
      view = await call('advance_learning_session', { sessionId: s.id, revision: answered.view.session.revision, stopId: s.current.id, outcome: 'continue' })
    }
    // A wrong first answer keeps both outcomes of the next question likely.
    await respond(false)

    // The app: on showing the last prepared question, ask for what either answer needs next.
    const job = await call('request_learning_material', { sessionId: view.session.id, provider: 'claude', ownerPid: process.pid })
    assert.equal(job.shouldRun, true)
    assert.deepEqual(job.slots.map((s: { level: string }) => s.level), ['medium', 'easy'])

    // The agent: read only those levels, then save one action for each.
    const context = await call('get_learning_material_context', { jobId: job.id })
    assert.deepEqual(context.slots.map((s: { level: string }) => s.level), ['medium', 'easy'])
    await call('save_learning_material', { jobId: job.id, items: [action('medium-A', 'medium', ['first', 'second']), action('easy-A-2', 'easy', ['third', 'fourth'])] })
    const finished = await call('finish_learning_material', { jobId: job.id })
    assert.equal(finished.completed, true)

    // A right answer climbs to the new medium material.
    await respond(true)
    assert.equal(view.session.current.level, 'medium')
    assert.equal(view.session.current.actionId, 'medium-A')
  } finally {
    await client.close()
  }
})
