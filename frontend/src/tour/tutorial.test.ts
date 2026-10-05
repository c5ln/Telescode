import { describe, expect, it } from 'vitest'

import type { TourStop } from './model'
import { pickTutorial, stepDurationMs, type SavedTour } from './tutorial'

const stop = (nodeId: string): TourStop => ({ nodeId, caption: nodeId, transitionMs: 1000, holdMs: 2000, title: nodeId, evidence: [] })

const tour = (id: string, nodes: string[], hash = 'other', updatedMs = 0): SavedTour => ({
  plan: { schemaVersion: 1, id, revision: 1, title: id, language: 'en', snapshotHash: hash, stops: nodes.map(stop) },
  snapshotHash: hash,
  state: 'draft',
  updatedMs,
})

const onMap = (id: string) => ['root', 'file:a.py', 'class:a.py::A'].includes(id)

describe('pickTutorial', () => {
  it('plays only tours whose every stop is on the map', () => {
    expect(pickTutorial([tour('t', ['root', 'file:b.py'])], onMap, 'h')).toBeNull()
    expect(pickTutorial([tour('t', ['root', 'file:a.py'])], onMap, 'h')?.plan.id).toBe('t')
  })

  it('prefers a tour of this exact snapshot, then the newest', () => {
    const tours = [tour('old', ['file:a.py'], 'other', 1), tour('new', ['file:a.py'], 'other', 2)]
    expect(pickTutorial(tours, onMap, 'h')?.plan.id).toBe('new')
    expect(pickTutorial([...tours, tour('exact', ['file:a.py'], 'h', 0)], onMap, 'h')?.plan.id).toBe('exact')
  })

  it('does not match a root-only tour of some other snapshot', () => {
    expect(pickTutorial([tour('t', ['root'])], onMap, 'h')).toBeNull()
    expect(pickTutorial([tour('t', ['root'], 'h')], onMap, 'h')?.plan.id).toBe('t')
  })

  it('skips malformed tours', () => {
    const broken = { ...tour('t', ['file:a.py']), plan: { stops: [{ nodeId: 'file:a.py' }] } } as unknown as SavedTour
    expect(pickTutorial([broken, tour('empty', [])], onMap, 'h')).toBeNull()
  })
})

describe('stepDurationMs', () => {
  it('plays a step a little quicker than the tour timing', () => {
    expect(stepDurationMs({ transitionMs: 1000, holdMs: 4000 })).toBe(4000)
  })
})
