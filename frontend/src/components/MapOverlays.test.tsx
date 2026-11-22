// @vitest-environment jsdom

import { cleanup, render, screen, within } from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import { afterEach, beforeEach, describe, expect, it } from 'vitest'

import { AppShell } from '../app/AppShell'
import type { AnalysisSnapshot, GraphResponse, TelescodeApi } from '../bridge'
import sherlock from '../graph/fixtures/sherlock.graph.json'
import type { GraphNode } from '../graph/model'
import { CanvasControls } from './CanvasControls'
import { FileCard, fileFactsText } from './MapOverlays'

const snapshot: AnalysisSnapshot = { ...(sherlock as GraphResponse), dbPath: 'C:/x/project.db', readingSequence: [] }
const api = { analyze: () => new Promise(() => {}) } as unknown as TelescodeApi
const open = () => render(<AppShell api={api} initialState={{ status: 'ready', dbPath: snapshot.dbPath, snapshot }} tours={{ list: async () => [] }} />)

beforeEach(() => localStorage.clear())
afterEach(cleanup)

describe('the first-time hint', () => {
  it('shows how to move around until dismissed, and stays dismissed', async () => {
    open()
    const hint = screen.getByRole('note', { name: 'How to move around the map' })
    expect(within(hint).getByText('Scroll')).toBeTruthy()
    await userEvent.click(within(hint).getByRole('button', { name: 'Got it' }))
    expect(screen.queryByRole('note', { name: 'How to move around the map' })).toBeNull()

    cleanup()
    open()
    expect(screen.queryByRole('note', { name: 'How to move around the map' })).toBeNull()
  })

  it('makes way for the code tour panel', async () => {
    open()
    await userEvent.click(within(screen.getByRole('banner')).getByRole('button', { name: 'Start tour' }))
    expect(screen.queryByRole('note', { name: 'How to move around the map' })).toBeNull()
  })
})

describe('the legend', () => {
  it('opens from the corner, explains size, shade and lines, and is remembered open', async () => {
    open()
    expect(screen.getByText('16 files, 11 classes')).toBeTruthy()
    await userEvent.click(screen.getByRole('button', { name: /Legend/ }))
    const legend = screen.getByRole('region', { name: 'Legend' })
    for (const title of ['Size', 'Shade', 'Line']) expect(within(legend).getByText(title)).toBeTruthy()

    cleanup()
    open()
    expect(screen.getByRole('region', { name: 'Legend' })).toBeTruthy()
    await userEvent.click(screen.getByRole('button', { name: 'Close legend' }))
    expect(screen.queryByRole('region', { name: 'Legend' })).toBeNull()
  })
})

describe('FileCard', () => {
  it("reads a file's own numbers", () => {
    const node = { id: 'file:pkg/notify.py', kind: 'file', label: 'notify.py', loc: 1286 } as GraphNode
    render(<FileCard facts={{ node, level: 2, imports: 1, usedBy: 3 }} x={0} y={0} offset={16} />)
    const card = screen.getByRole('tooltip')
    expect(within(card).getByText('pkg/notify.py')).toBeTruthy()
    expect(within(card).getByText('1,286')).toBeTruthy()
    expect(within(card).getByText('High for this repository')).toBeTruthy()
    expect(within(card).getByText('1 file')).toBeTruthy()
    expect(within(card).getByText('3 files')).toBeTruthy()
  })
  it('says the same numbers in one sentence for a screen reader', () => {
    const node = { id: 'file:pkg/notify.py', kind: 'file', label: 'notify.py', loc: 1286 } as GraphNode
    expect(fileFactsText({ node, level: 2, imports: 1, usedBy: 3 })).toBe('1,286 lines, High complexity for this repository, imports 1 file, used by 3 files')
  })
})

describe('CanvasControls help', () => {
  it('lists the mouse and keyboard controls, and closes on Escape', async () => {
    render(<CanvasControls />)
    await userEvent.click(screen.getByRole('button', { name: 'How to move around' }))
    const help = screen.getByRole('region', { name: 'Moving around the map' })
    expect(within(help).getByText('Double-click')).toBeTruthy()
    expect(within(help).getByText('Start or leave a code tour')).toBeTruthy()
    await userEvent.keyboard('{Escape}')
    expect(screen.queryByRole('region', { name: 'Moving around the map' })).toBeNull()
  })
})
