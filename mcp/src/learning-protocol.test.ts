import { test } from 'node:test'
import assert from 'node:assert/strict'
import { mkdtemp, mkdir, writeFile } from 'node:fs/promises'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { Client } from '@modelcontextprotocol/sdk/client/index.js'
import { StdioClientTransport } from '@modelcontextprotocol/sdk/client/stdio.js'

test('learning MCP protocol persists scoped AI preparation, server feedback and revision-checked stop', { timeout: 30000 }, async () => {
  const cwd = fileURLToPath(new URL('..', import.meta.url))
  const dir = await mkdtemp(join(tmpdir(), 'telescode-learning-protocol-'))
  const source = join(dir, 'source'); await mkdir(join(source, 'sherlock_project'), { recursive: true })
  await writeFile(join(source, 'sherlock_project/result.py'), 'class QueryResult:\n    def __init__(self, status):\n        self.status = status\n')
  const client = new Client({ name: 'learning-test', version: '1' })
  const env = Object.fromEntries(Object.entries(process.env).filter((v): v is [string, string] => v[1] !== undefined))
  const transport = new StdioClientTransport({ command: process.execPath, args: ['--import', 'tsx', 'src/server.ts'], cwd,
    env: { ...env, TELESCODE_TOUR_OUTPUT: join(dir, 'artifacts') }, stderr: 'pipe' })
  let stderr = ''; transport.stderr?.on('data', chunk => { stderr += chunk.toString() })
  try { await client.connect(transport) } catch (error) { throw new Error(`${String(error)}\n${stderr}`) }
  try {
    async function call(name: string, args: Record<string, unknown>) {
      const result = await client.callTool({ name, arguments: args })
      assert.ok(!result.isError, JSON.stringify(result))
      return JSON.parse((result.content as { text: string }[])[0]!.text)
    }
    const project = await call('open_project', { path: fileURLToPath(new URL('../../frontend/src/graph/fixtures/sherlock.graph.json', import.meta.url)), sourceRoot: source })
    const seed = { goalId: 'results', goalRevision: 1, rawQuery: '검색 결과 전달 이해', retrievalQuery: 'query result status',
      targetConcepts: [{ conceptId: 'result-state', level: 'easy', weight: 1 }], budget: 4 }
    const started = await call('start_learning_session', { projectId: project.projectId, goal: seed })
    assert.equal(started.session.status, 'preparing')
    assert.equal(started.session.goal.rawQuery, seed.rawQuery)
    const job = await call('request_learning_preparation', { sessionId: started.session.id, revision: started.session.revision, provider: 'codex', ownerPid: process.pid })
    const context = await call('get_learning_preparation_context', { jobId: job.id })
    assert.equal(context.goal.retrievalQuery, seed.retrievalQuery)
    assert.ok(!Object.hasOwn(context, 'evaluationQuestions'))
    const nodeId = 'file:sherlock_project/result.py'
    const q = (suffix: string) => ({ type: 'multiple_choice', difficulty: 'easy', concept: 'Result state',
      question: `Why preserve the result status? ${suffix}`, answer: 'To let the consumer distinguish outcomes',
      distractors: ['To restart the request', 'To configure the remote site'], explanation: 'The result keeps the status for its consumer.', evidenceNodeIds: [nodeId] })
    const curriculum = { version: 1, concepts: [{ id: 'result-state', name: 'Result state', definition: 'Explain why the result retains its outcome', version: 1 }],
      items: [{ id: 'read-result', kind: 'read', nodeId, conceptId: 'result-state', level: 'easy', caption: 'Follow the result status.', questions: [q('success'), q('failure')] }], relations: [] }
    const ready = await call('save_learning_curriculum', { jobId: job.id, curriculum })
    assert.equal(ready.session.status, 'active')
    assert.ok(ready.question)
    assert.equal(Object.hasOwn(ready.question, 'answer'), false)
    const input = { sessionId: ready.session.id, revision: ready.session.revision, stopId: ready.session.current.id,
      submissionId: 'wire-answer', questionId: ready.question.id, selectedIndex: 0 }
    const answered = await call('record_learning_response', input)
    assert.equal(answered.feedback.correct, ready.question.choices[0] === answered.feedback.answer)
    assert.equal(answered.view.session.current.id, ready.session.current.id)
    assert.deepEqual(await call('record_learning_response', input), answered)
    const read = await call('get_learning_session', { sessionId: ready.session.id })
    assert.equal(read.knowledge.find((k: { conceptId: string; level: string }) => k.conceptId === 'result-state' && k.level === 'easy').observations, 1)
    const stopped = await call('advance_learning_session', { sessionId: read.session.id, revision: read.session.revision, stopId: read.session.current.id, outcome: 'stop' })
    assert.equal(stopped.session.status, 'stopped')
    const pending = await call('start_learning_session', { projectId: project.projectId, goal: { ...seed, goalId: 'pending', rawQuery: 'different task', retrievalQuery: 'different task' } })
    const pendingJob = await call('request_learning_preparation', { sessionId: pending.session.id, revision: pending.session.revision, provider: 'codex', ownerPid: process.pid })
    const latest = await call('get_learning_session', { sessionId: pending.session.id })
    const ended = await call('advance_learning_session', { sessionId: latest.session.id, revision: latest.session.revision, outcome: 'stop' })
    assert.equal(ended.session.status, 'stopped')
    const stale = await client.callTool({ name: 'save_learning_curriculum', arguments: { jobId: pendingJob.id, curriculum } })
    assert.equal(stale.isError, true)
  } finally { await client.close() }
})
