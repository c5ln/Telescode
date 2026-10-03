import { test } from 'node:test'
import assert from 'node:assert/strict'
import { mkdtemp, readFile } from 'node:fs/promises'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { Client } from '@modelcontextprotocol/sdk/client/index.js'
import { StdioClientTransport } from '@modelcontextprotocol/sdk/client/stdio.js'

test('MCP protocol: draft, screenshot, user approval, render and artifact playback', { timeout: 60000 }, async () => {
  const cwd = fileURLToPath(new URL('..', import.meta.url))
  const output = await mkdtemp(join(tmpdir(), 'telescode-protocol-'))
  const env = Object.fromEntries(Object.entries(process.env).filter((pair): pair is [string, string] => pair[1] !== undefined))
  const transport = new StdioClientTransport({ command: process.execPath, args: ['--import', 'tsx', 'src/server.ts'], cwd, env: { ...env, TELESCODE_TOUR_OUTPUT: output }, stderr: 'pipe' })
  const client = new Client({ name: 'tour-test', version: '1.0.0' })
  let stderr = ''
  transport.stderr?.on('data', data => { stderr += data.toString() })
  try {
    await client.connect(transport)
    const list = await client.listTools()
    assert.ok(list.tools.some(t => t.name === 'preview_tour_frame'))
    assert.ok(!list.tools.some(t => /approve/.test(t.name) && t.name !== 'render_approved_tour'))
    async function call(name: string, args: Record<string, unknown>) {
      const response = await client.callTool({ name, arguments: args })
      assert.ok(!response.isError, JSON.stringify(response))
      const content = response.content as { type: string; text?: string }[]
      return JSON.parse(content.find(c => c.type === 'text')!.text!)
    }
    const project = await call('open_project', { path: fileURLToPath(new URL('../../frontend/src/graph/fixtures/sherlock.graph.json', import.meta.url)) })
    const draft = await call('create_tour_draft', { projectId: project.projectId, title: '프로토콜 검증', language: 'ko', stops: [{ nodeId: 'root', caption: '저장소 전체 구조입니다.', transitionMs: 0, holdMs: 2000 }] })
    const rejected = await client.callTool({ name: 'render_approved_tour', arguments: { tourId: draft.plan.id, approvedRevision: 1 } })
    assert.equal(rejected.isError, true)
    const preview = await client.callTool({ name: 'preview_tour_frame', arguments: { tourId: draft.plan.id, timeMs: 1000 } })
    assert.ok(!preview.isError, JSON.stringify(preview))
    assert.equal((preview.content as { type: string }[])[0].type, 'image')
    const url = new URL(draft.reviewUrl)
    const approval = await fetch(url, { method: 'POST', headers: { origin: url.origin } })
    assert.equal(approval.status, 200)
    const rendering = await call('render_approved_tour', { tourId: draft.plan.id, approvedRevision: 1 })
    assert.equal(rendering.state, 'rendering')
    let status
    for (let i = 0; i < 100; i++) {
      status = await call('get_tour_status', { tourId: draft.plan.id })
      if (status.state === 'completed' || status.state === 'failed') break
      await new Promise(resolve => setTimeout(resolve, 200))
    }
    assert.equal(status.state, 'completed', `${status.error ?? ''}\n${stderr}`)
    const manifest = JSON.parse(await readFile(join(status.outputDirectory, 'manifest.json'), 'utf8'))
    assert.equal(manifest.video.durationMs, 2000)
    assert.equal(manifest.video.audio, false)
    assert.equal(manifest.video.fps, 30)
    const playerResponse = await fetch(status.playerUrl)
    assert.equal(playerResponse.status, 200)
    assert.match(await playerResponse.text(), /video::cue\{font-size:50%\}/)
    const vttUrl = new URL(status.playerUrl)
    vttUrl.pathname = vttUrl.pathname.replace('player.html', 'tour.vtt')
    const vttResponse = await fetch(vttUrl)
    assert.match(vttResponse.headers.get('content-type')!, /text\/vtt/)
    assert.match(await vttResponse.text(), /00:00:00\.000 --> 00:00:02\.000/)
  } finally { await client.close() }
})
