import { test } from 'node:test'
import assert from 'node:assert/strict'
import { mkdtemp } from 'node:fs/promises'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { Client } from '@modelcontextprotocol/sdk/client/index.js'
import { StdioClientTransport } from '@modelcontextprotocol/sdk/client/stdio.js'
import { exposedTools, TOOLS_ENV } from './exposed-tools.ts'

/** The tool names a server started with `tools` (or without the variable) offers, and what it logged. */
async function serverTools(tools?: string) {
  const cwd = fileURLToPath(new URL('..', import.meta.url))
  const env = Object.fromEntries(Object.entries(process.env).filter((v): v is [string, string] => v[1] !== undefined && v[0] !== TOOLS_ENV))
  const transport = new StdioClientTransport({ command: process.execPath, args: ['--import', 'tsx', 'src/server.ts'], cwd,
    env: { ...env, TELESCODE_TOUR_OUTPUT: await mkdtemp(join(tmpdir(), 'telescode-server-tools-')), ...(tools === undefined ? {} : { [TOOLS_ENV]: tools }) }, stderr: 'pipe' })
  let stderr = ''; transport.stderr?.on('data', chunk => { stderr += chunk.toString() })
  const client = new Client({ name: 'tools-test', version: '1' })
  try { await client.connect(transport) } catch (error) { throw new Error(`${String(error)}\n${stderr}`) }
  try { return { names: (await client.listTools()).tools.map(t => t.name).sort(), stderr } } finally { await client.close() }
}

test('without the variable every tool is exposed; with it, exactly the named ones', () => {
  assert.equal(exposedTools({})('open_project'), true)
  const only = exposedTools({ [TOOLS_ENV]: ' get_quiz_context, create_quiz ,' })
  assert.deepEqual(['get_quiz_context', 'create_quiz', 'open_project'].map(only), [true, true, false])
  // A run with no tools gets none, not all.
  assert.equal(exposedTools({ [TOOLS_ENV]: '' })('open_project'), false)
})

test("an agent's server registers only the tools its run may call", { timeout: 60000 }, async () => {
  const all = (await serverTools()).names
  for (const name of ['open_project', 'get_quiz_context', 'preview_tour_frame', 'start_learning_session', 'save_learning_material']) assert.ok(all.includes(name), name)

  // The material agent's two tools, from the learning tools.
  assert.deepEqual((await serverTools('get_learning_material_context,save_learning_material')).names, ['get_learning_material_context', 'save_learning_material'])
  // The quiz agent's tools, from the server's own; the tour preview is registered apart from them.
  assert.deepEqual((await serverTools('open_project,inspect_node,get_quiz_context,create_quiz')).names, ['create_quiz', 'get_quiz_context', 'inspect_node', 'open_project'])
  assert.deepEqual((await serverTools('preview_tour_frame')).names, ['preview_tour_frame'])

  // A name no tool has is reported, so a renamed tool does not silently leave an agent without it.
  const typo = await serverTools('create_quiz,create_quizz')
  assert.deepEqual(typo.names, ['create_quiz'])
  assert.match(typo.stderr, /create_quizz/)
})
