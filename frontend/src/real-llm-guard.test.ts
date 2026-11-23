// scripts/build-examples.ts has Claude Code write every example's tour,
// quizzes and learning path: minutes and real tokens per run. It must stay a
// script run by hand, never reached by the tests or the build.
import { readFileSync } from 'node:fs'
import { resolve } from 'node:path'
import { describe, expect, it } from 'vitest'

import config from '../vite.config'

const root = resolve(__dirname, '..')
const scripts = (dir: string): Record<string, string> => JSON.parse(readFileSync(resolve(dir, 'package.json'), 'utf8')).scripts

describe('real agent runs stay out of the default checks', () => {
  it('no npm script runs the example generator', () => {
    for (const [name, command] of Object.entries({ ...scripts(root), ...scripts(resolve(root, '../mcp')) })) {
      expect(command, name).not.toMatch(/build-examples/)
    }
  })

  it('the tests are found under src only, not among the scripts', () => {
    const include = config.test?.include ?? []
    expect(include.length).toBeGreaterThan(0)
    for (const pattern of include) expect(pattern.startsWith('src/'), pattern).toBe(true)
  })

  it('the MCP tests do not start the live smoke check', () => {
    expect(scripts(resolve(root, '../mcp')).test).not.toMatch(/smoke/)
  })
})
