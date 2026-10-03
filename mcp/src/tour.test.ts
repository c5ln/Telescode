import { test } from 'node:test'
import assert from 'node:assert/strict'
import { readFile, mkdtemp } from 'node:fs/promises'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { TourStore } from './store.ts'
import { startHttp } from './http.ts'
import { makeStops, toVtt, inspectNode } from '../../frontend/src/tour/model.ts'
const fixture = new URL('../../frontend/src/graph/fixtures/sherlock.graph.json', import.meta.url)
const snapshot = { ...JSON.parse(await readFile(fixture, 'utf8')), readingSequence: [] }
const stops = [{ nodeId: 'root', caption: '전체 구조 <확인>', transitionMs: 1000, holdMs: 2000 }]

test('limits and unsupported targets reject instead of silently truncating', () => {
  assert.throws(() => makeStops(snapshot, Array(9).fill(stops[0])), /1–8/)
  assert.throws(() => makeStops(snapshot, [{ ...stops[0], holdMs: 120000 }]), /120 seconds/)
  assert.throws(() => makeStops(snapshot, [{ ...stops[0], nodeId: 'file:missing.py' }]), /Unsupported/)
  assert.throws(() => makeStops(snapshot, [{ ...stops[0], transitionMs: NaN }]), /transitionMs/)
  assert.throws(() => makeStops(snapshot, [{ ...stops[0], holdMs: 1000 }]), /holdMs/)
})
test('evidence uses actual data and captions begin after movement', () => {
  const info = inspectNode(snapshot, 'root')
  assert.equal(info.evidence.find(e => e.label === 'Files')?.value, String(snapshot.files.length))
  const plan = { schemaVersion: 1 as const, id: 'test', revision: 1, title: 'test', language: 'ko' as const, snapshotHash: 'x', stops: makeStops(snapshot, stops) }
  assert.match(toVtt(plan), /00:00:01\.000 --> 00:00:03\.000/)
  assert.match(toVtt(plan), /&lt;확인&gt;/)
})
test('approval requires user review; revisions invalidate approval', async () => {
  const store = new TourStore(await mkdtemp(join(tmpdir(), 'telescode-tour-')))
  const p = await store.open(fileURLToPath(fixture))
  const d = await store.create(p.id, '테스트', 'ko', stops)
  assert.throws(() => store.assertApproved(d, 1), /approval/)
  const http = await startHttp(store, '/tmp/nonexistent')
  try {
    const url = http.reviewUrl(d)
    assert.equal((await fetch(url)).status, 200)
    assert.equal((await fetch(url, { method: 'POST' })).status, 403)
    assert.equal((await fetch(url, { method: 'POST', headers: { origin: http.baseUrl } })).status, 200)
    store.assertApproved(d, 1)
    await store.revise(d.plan.id, 1, '수정', stops)
    assert.throws(() => store.assertApproved(d, 2), /approval/)
    assert.equal((await fetch(url)).status, 403)
    const restored = new TourStore(store.outputRoot)
    await restored.restore()
    assert.equal(restored.draft(d.plan.id).plan.revision, 2)
    assert.throws(() => restored.assertApproved(restored.draft(d.plan.id), 2), /approval/)
  } finally { await http.close() }
})
