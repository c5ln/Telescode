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

  it('matches a ranked tour to its source snapshot, including root-only tours', () => {
    const ranked = { ...tour('ranked', ['root'], 'ranked-hash', 3), sourceSnapshotHash: 'source-hash' }
    expect(pickTutorial([ranked], onMap, 'source-hash')?.plan.id).toBe('ranked')
    expect(pickTutorial([ranked], onMap, 'other-source')).toBeNull()
    expect(pickTutorial([tour('base', ['root'], 'source-hash', 1), ranked], onMap, 'source-hash')?.plan.id).toBe('ranked')
    const fileTour = { ...ranked, plan: { ...ranked.plan, stops: [stop('file:a.py')] } }
    expect(pickTutorial([fileTour], onMap, 'different-repository-with-same-files')).toBeNull()
  })

  it('can require model order so desktop onboarding regenerates legacy tours', () => {
    const legacy = tour('legacy', ['file:a.py'], 'source-hash', 10)
    const ranked = { ...tour('ranked', ['file:a.py'], 'ranked-hash', 1), sourceSnapshotHash: 'source-hash' }
    expect(pickTutorial([legacy], onMap, 'source-hash', true)).toBeNull()
    expect(pickTutorial([legacy, ranked], onMap, 'source-hash', true)?.plan.id).toBe('ranked')
  })
})

describe('stepDurationMs', () => {
  it('plays a step a little quicker than the tour timing', () => {
    expect(stepDurationMs({ transitionMs: 1000, holdMs: 4000 })).toBe(4000)
  })
})
