// @vitest-environment jsdom
import { clearMocks, mockIPC } from '@tauri-apps/api/mocks'
import { afterEach, beforeEach, expect, it, vi } from 'vitest'
import { createLearningLibrary } from './api'
beforeEach(() => vi.stubGlobal('isTauri', true))
afterEach(() => { clearMocks(); vi.unstubAllGlobals() })

it('forwards selected Codex/Claude provider only to preparation and preserves explicit goal data', async () => {
  const invoke = vi.fn(async (..._args: unknown[]) => ({}))
  mockIPC((command, args) => invoke(command, args))
  const goal = { goalId: 'q', goalRevision: 3, rawQuery: '중복 요청 처리 이해', retrievalQuery: 'idempotent request deduplication',
    targetConcepts: [{ conceptId: 'dedup', level: 'hard' as const, weight: 2 }], budget: 7 }
  for (const provider of ['codex', 'claude'] as const) {
    const library = createLearningLibrary(provider)
    await library.start({ dbPath: '/repo with spaces/project.db', goal })
    expect(invoke).toHaveBeenLastCalledWith('start_learning_session', { dbPath: '/repo with spaces/project.db', goal })
    await library.prepare({ sessionId: 's', revision: 4 })
    expect(invoke).toHaveBeenLastCalledWith('prepare_learning_session', { sessionId: 's', revision: 4, provider })
  }
})

it('passes response identity and exact revision through native IPC without grading in the client', async () => {
  const invoke = vi.fn(async (..._args: unknown[]) => ({}))
  mockIPC((command, args) => invoke(command, args))
  const library = createLearningLibrary('codex')
  const answer = { sessionId: 's', revision: 9, stopId: 'stable-stop', submissionId: 'transport-retry', questionId: 'q', selectedIndex: 2 }
  await library.respond(answer)
  expect(invoke).toHaveBeenLastCalledWith('record_learning_response', { answer })
  await library.advance({ sessionId: 's', revision: 10, stopId: 'stable-stop', outcome: 'skip' })
  expect(invoke).toHaveBeenLastCalledWith('advance_learning_session', { input: { sessionId: 's', revision: 10, stopId: 'stable-stop', outcome: 'skip' } })
})
