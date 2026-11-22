import { test } from 'node:test'
import assert from 'node:assert/strict'
import { spawn } from 'node:child_process'
import { mkdtemp } from 'node:fs/promises'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { fileURLToPath } from 'node:url'

// The app talks to the server over stdin and stdout. When the app goes, its
// end of the pipe closes, and the server must go too: its review web server
// would otherwise keep the process alive with no one left to talk to.
test('the server exits once the app that started it closes its stdin', { timeout: 30000 }, async () => {
  const cwd = fileURLToPath(new URL('..', import.meta.url))
  const output = await mkdtemp(join(tmpdir(), 'telescode-lifecycle-'))
  const child = spawn(process.execPath, ['--import', 'tsx', 'src/server.ts'], { cwd, env: { ...process.env, TELESCODE_TOUR_OUTPUT: output }, stdio: ['pipe', 'pipe', 'pipe'] })
  try {
    // Wait until it is up: it announces its review server once listening.
    await new Promise<void>((ready, fail) => {
      let stderr = ''
      child.stderr.on('data', (d) => { stderr += d; if (stderr.includes('review server')) ready() })
      child.on('exit', () => fail(new Error(`exited before starting: ${stderr}`)))
    })
    const exited = new Promise<number | null>((done) => child.on('exit', (code) => done(code)))
    child.stdin.end()
    const code = await Promise.race([exited, new Promise<'still running'>((done) => setTimeout(() => done('still running'), 5000))])
    assert.notEqual(code, 'still running', 'the server kept running after its stdin closed')
  } finally {
    child.kill()
  }
})
