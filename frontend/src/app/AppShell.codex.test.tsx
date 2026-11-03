/// <reference types="node" />
// @vitest-environment jsdom

// Test-first contract for the app's Codex UI -> native IPC flow. AppShell,
// hooks and the default tour/quiz libraries are real; only native IPC is mocked.
// Passing them will not replace a desktop test of the real Rust/CLI backend.
import { createHash, webcrypto } from 'node:crypto'
import { act, cleanup, render, screen, within } from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import { clearMocks, mockIPC } from '@tauri-apps/api/mocks'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'

import type { AnalysisSnapshot, GraphResponse } from '../bridge'
import sherlock from '../graph/fixtures/sherlock.graph.json'
import type { QuizSet } from '../quiz/model'
import type { SavedTour } from '../tour/tutorial'
import { AppShell } from './AppShell'

const dbPath = '/projects/sherlock/project.db'
const snapshot: AnalysisSnapshot = { ...(sherlock as GraphResponse), dbPath, readingSequence: [] }
const hash = createHash('sha256').update(JSON.stringify(snapshot)).digest('hex')
const ready = { status: 'ready' as const, dbPath, snapshot }
const nodeId = 'file:sherlock_project/result.py'
const savedTour: SavedTour = {
  plan: { schemaVersion: 1, id: 'tour-codex', revision: 1, title: 'Sherlock onboarding', language: 'en', snapshotHash: 'ranked',
    stops: [{ nodeId, title: 'result.py', caption: 'Results describe the outcome of a username search.',
      evidence: [], transitionMs: 0, holdMs: 60000 }] },
  snapshotHash: 'ranked', sourceSnapshotHash: hash, state: 'draft', updatedMs: 1,
}
const savedQuiz: QuizSet = {
  schemaVersion: 1, id: 'quiz-codex', tourId: savedTour.plan.id, tourRevision: 1,
  stopIndex: 0, nodeId, language: 'en', snapshotHash: hash, createdMs: 1,
  questions: [{ id: 'question-codex', type: 'multiple_choice', difficulty: 'easy', concept: 'Search results',
    question: 'Why does a search result preserve its status?', answer: 'To distinguish search outcomes',
    distractors: ['To start a new search', 'To configure a site'],
    choices: ['To distinguish search outcomes', 'To start a new search', 'To configure a site'],
    explanation: 'The status distinguishes the outcome reported for a search.', evidence: [{ nodeId, title: 'result.py' }] }],
}

function deferred() {
  let resolve!: () => void
  const promise = new Promise<void>(done => { resolve = done })
  return { promise, resolve }
}

function native(options: {
  installed?: boolean; signedIn?: boolean; tour?: boolean; quiz?: boolean
  login?: Promise<void>; generation?: Promise<void>; failure?: string
} = {}) {
  let hasTour = options.tour ?? false
  let hasQuiz = options.quiz ?? false
  const invoke = vi.fn(async (command: string, args?: Record<string, unknown>) => {
    switch (command) {
      case 'list_tours': return hasTour ? [savedTour] : []
      case 'list_quizzes': return hasQuiz ? [savedQuiz] : []
      case 'agent_status':
        expect(args).toEqual({ provider: 'codex' })
        return { installed: options.installed ?? true, signedIn: options.signedIn ?? true }
      case 'agent_sign_in':
        expect(args).toEqual({ provider: 'codex' })
        await options.login
        return
      case 'generate_tour':
        expect(args).toEqual({ dbPath, provider: 'codex' })
        if (options.failure) throw { code: 'tour_generation_failed', message: options.failure }
        await options.generation
        hasTour = true
        return
      case 'generate_quiz':
        expect(args).toEqual({ provider: 'codex', target: { dbPath, tourId: savedTour.plan.id, tourRevision: 1, stopIndex: 0, nodeId } })
        hasQuiz = true
        return
      case 'record_quiz_response':
        expect(args).toMatchObject({ quizSetId: savedQuiz.id, quizId: 'question-codex', selectedIndex: 0 })
        return { response: {}, feedback: { correct: true, answer: savedQuiz.questions[0]!.answer,
          explanation: savedQuiz.questions[0]!.explanation, evidence: savedQuiz.questions[0]!.evidence, attempt: 1 } }
      default: throw new Error(`Unexpected native call: ${command}`)
    }
  })
  mockIPC((command, args) => invoke(command, args as Record<string, unknown>))
  return invoke
}

function agentSelector() {
  const selector = screen.queryByRole('button', { name: 'AI agent' })
  expect(selector, 'The app must let the user choose Claude Code or Codex using the AI agent selector').not.toBeNull()
  return selector as HTMLButtonElement
}

async function chooseCodex() {
  await userEvent.click(agentSelector())
  await userEvent.click(screen.getByRole('menuitemradio', { name: 'Codex' }))
}

async function startTutorial() {
  await userEvent.click(screen.getByRole('button', { name: 'Tutorial' }))
  return screen.findByRole('region', { name: 'Tutorial' })
}

beforeEach(() => {
  localStorage.clear()
  vi.stubGlobal('isTauri', true)
  vi.stubGlobal('crypto', webcrypto)
})

afterEach(() => {
  cleanup()
  clearMocks()
  vi.unstubAllGlobals()
  localStorage.clear()
})

describe('Codex in the app', () => {
  it('defaults to Claude, offers Codex and remembers the selection when the app reopens', async () => {
    const invoke = native()
    const app = render(<AppShell initialState={ready} />)
    const selector = agentSelector()
    expect(selector.textContent).toBe('Claude Code')
    await userEvent.click(selector)
    expect(screen.getByRole('menuitemradio', { name: 'Claude Code', checked: true })).toBeTruthy()
    expect(screen.getByRole('menuitemradio', { name: 'Codex', checked: false })).toBeTruthy()
    await userEvent.keyboard('{Escape}')
    await chooseCodex()
    expect(invoke).not.toHaveBeenCalled() // Choosing does not start login or generation.
    app.unmount()
    render(<AppShell initialState={ready} />)
    expect(agentSelector().textContent).toBe('Codex')
  })

  it('signs in to the chosen Codex account before generating and plays the saved tutorial', async () => {
    const login = deferred()
    const generation = deferred()
    const invoke = native({ signedIn: false, login: login.promise, generation: generation.promise })
    render(<AppShell initialState={ready} />)
    await chooseCodex()
    const panel = await startTutorial()
    await vi.waitFor(() => expect(invoke).toHaveBeenCalledWith('agent_sign_in', { provider: 'codex' }))
    expect(panel.textContent).toMatch(/sign in to Codex/i)
    expect(agentSelector().disabled).toBe(true)
    expect(invoke.mock.calls.some(([command]) => command === 'generate_tour')).toBe(false)
    await act(async () => login.resolve())
    await vi.waitFor(() => expect(invoke).toHaveBeenCalledWith('generate_tour', { dbPath, provider: 'codex' }))
    expect(agentSelector().disabled).toBe(true)
    await act(async () => generation.resolve())
    await within(panel).findByText('Tutorial ready')
    await userEvent.click(within(panel).getByRole('button', { name: 'Start tutorial' }))
    expect(within(panel).getByText(savedTour.plan.stops[0]!.caption)).toBeTruthy()
    expect(invoke.mock.calls.some(([command]) => command.startsWith('claude_'))).toBe(false)
  })

  it('generates a quiz for the current step using Codex, shows it and records the answer', async () => {
    const invoke = native({ tour: true })
    render(<AppShell initialState={ready} />)
    await chooseCodex()
    const tutorial = await startTutorial()
    await userEvent.click(await within(tutorial).findByRole('button', { name: 'Quiz on this step' }))
    const quiz = await screen.findByRole('region', { name: 'Quiz' })
    await within(quiz).findByText(savedQuiz.questions[0]!.question)
    expect(invoke).toHaveBeenCalledWith('generate_quiz', { provider: 'codex', target: {
      dbPath, tourId: savedTour.plan.id, tourRevision: 1, stopIndex: 0, nodeId,
    } })
    expect(invoke.mock.calls.some(([command]) => command === 'generate_tour' || command === 'agent_sign_in' || command.startsWith('claude_'))).toBe(false)
    await userEvent.click(within(quiz).getByRole('radio', { name: savedQuiz.questions[0]!.answer }))
    await userEvent.click(within(quiz).getByRole('button', { name: 'Check answer' }))
    await within(quiz).findByText('Correct.')
    expect(invoke).toHaveBeenCalledWith('record_quiz_response', expect.objectContaining({ quizSetId: savedQuiz.id, selectedIndex: 0 }))
  })

  it('explains that Codex is missing and does not start login or generation', async () => {
    const invoke = native({ installed: false, signedIn: false })
    render(<AppShell initialState={ready} />)
    await chooseCodex()
    const panel = await startTutorial()
    const heading = await within(panel).findByRole('heading', { name: /Codex/i })
    expect(heading.textContent).toMatch(/needed|install/i)
    expect(invoke).toHaveBeenCalledWith('agent_status', { provider: 'codex' })
    expect(invoke.mock.calls.some(([command]) => command === 'agent_sign_in' || command.startsWith('generate_'))).toBe(false)
  })

  it('shows a Codex generation failure without switching providers or claiming a saved tutorial', async () => {
    const invoke = native({ failure: 'Codex account quota reached.' })
    render(<AppShell initialState={ready} />)
    await chooseCodex()
    const panel = await startTutorial()
    await within(panel).findByText(/Codex account quota reached/)
    expect(within(panel).queryByText('Tutorial ready')).toBeNull()
    expect(invoke.mock.calls.filter(([command]) => command === 'generate_tour')).toHaveLength(1)
    expect(invoke.mock.calls.some(([command]) => command.startsWith('claude_'))).toBe(false)
  })

  it('uses saved tutorials and quizzes without checking or launching an agent', async () => {
    const invoke = native({ tour: true, quiz: true })
    render(<AppShell initialState={ready} />)
    await chooseCodex()
    const tutorial = await startTutorial()
    await userEvent.click(await within(tutorial).findByRole('button', { name: 'Quiz on this step' }))
    const quiz = await screen.findByRole('region', { name: 'Quiz' })
    await within(quiz).findByText(savedQuiz.questions[0]!.question)
    expect(invoke.mock.calls.every(([command]) => command === 'list_tours' || command === 'list_quizzes')).toBe(true)
  })
})
