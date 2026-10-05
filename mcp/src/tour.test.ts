import { test } from 'node:test'
import assert from 'node:assert/strict'
import { readFile, mkdtemp } from 'node:fs/promises'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { TourStore } from './store.ts'
import { startHttp } from './http.ts'
import { makeStops, toVtt, inspectNode, readingTourOrder, tourTimeline, tourFrameState, FPS, durationMs } from '../../frontend/src/tour/model.ts'
import { tourFrames } from './frames.ts'
import { chromium } from './browser.ts'
import { resolve } from 'node:path'
const fixture = new URL('../../frontend/src/graph/fixtures/sherlock.graph.json', import.meta.url)
const snapshot = { ...JSON.parse(await readFile(fixture, 'utf8')), readingSequence: [] }
const stops = [{ nodeId: 'root', caption: '전체 구조 <확인>', transitionMs: 1000, holdMs: 2000 }]

test('timeline assigns boundaries to the next stop and holds only after movement', () => {
  const timeline = tourTimeline([stops[0], { ...stops[0], transitionMs: 0 }])
  assert.deepEqual(tourFrameState(timeline, 999), { index: 0, progress: 0.999, holding: false })
  assert.deepEqual(tourFrameState(timeline, 1000), { index: 0, progress: 1, holding: true })
  assert.equal(tourFrameState(timeline, 2999).index, 0)
  assert.deepEqual(tourFrameState(timeline, 3000), { index: 1, progress: 1, holding: true })
})

test('hold frames reuse one PNG per stop, including repeated nodes and unaligned boundaries', async () => {
  const plan = { schemaVersion: 1 as const, id: 'frames', revision: 1, title: 'frames', language: 'ko' as const, snapshotHash: 'x', stops: makeStops(snapshot, [
    { ...stops[0], transitionMs: 0 },
    { ...stops[0], transitionMs: 101, caption: '다음 패널' },
    { ...stops[0], transitionMs: 0, caption: '세 번째 패널' },
  ]) }
  const captures: number[] = []
  const stats = { capturedFrames: 0, reusedFrames: 0 }
  const frames: Buffer[] = []
  for await (const png of tourFrames(plan, async ms => {
    captures.push(ms)
    return Buffer.from(String(ms))
  }, stats)) frames.push(png)
  const total = Math.ceil(durationMs(plan) * FPS / 1000)
  assert.equal(frames.length, total)
  assert.equal(stats.capturedFrames, 7) // 4 moving frames, 3 settled images.
  assert.equal(stats.reusedFrames, total - 7)
  assert.deepEqual(captures, [0, 2000, 61000 / 30, 62000 / 30, 2100, 64000 / 30, 124000 / 30])
  assert.strictEqual(frames[0], frames[59])
  assert.notStrictEqual(frames[59], frames[60])
  assert.notStrictEqual(frames[63], frames[64]) // First settled frame refreshes selection/panel.
  assert.strictEqual(frames[64], frames[123])
  assert.notStrictEqual(frames[123], frames[124]) // Same node, new caption/active stop.
  assert.strictEqual(frames[124], frames.at(-1))
})

test('capture errors stop frame production', async () => {
  const plan = { schemaVersion: 1 as const, id: 'frames', revision: 1, title: 'frames', language: 'ko' as const, snapshotHash: 'x', stops: makeStops(snapshot, stops) }
  await assert.rejects(async () => {
    for await (const png of tourFrames(plan, async () => { throw new Error('capture failed') }, { capturedFrames: 0, reusedFrames: 0 })) void png
  }, /capture failed/)
})

test('render screen paints captions at 100% size only during each hold', async () => {
  const store = new TourStore(await mkdtemp(join(tmpdir(), 'telescode-caption-')))
  const project = await store.open(fileURLToPath(fixture))
  const draft = await store.create(project.id, 'caption test', 'ko', [
    { ...stops[0], transitionMs: 0, caption: '첫 번째 설명 <문자>' },
    { ...stops[0], caption: '두 번째 설명' },
  ])
  const http = await startHttp(store, resolve(fileURLToPath(new URL('../../frontend/dist', import.meta.url))))
  const session = http.registerRender(draft)
  let browser: Awaited<ReturnType<typeof chromium.launch>> | undefined
  try {
    browser = await chromium.launch({ headless: true })
    const page = await browser.newPage({ viewport: { width: 1920, height: 1080 } })
    await page.goto(session.url)
    await page.waitForFunction(() => window.telescodeTour || window.tourError)
    assert.equal(await page.evaluate(() => window.tourError), undefined)
    const caption = page.locator('.tour-caption')
    assert.equal(await caption.textContent(), '첫 번째 설명 <문자>')
    assert.equal(await caption.evaluate(el => getComputedStyle(el).fontSize), '40px')
    const box = await caption.boundingBox()
    assert.ok(box && box.y >= 980 && box.y + box.height <= 1080)
    await page.evaluate(() => window.telescodeTour!.seek(2000))
    assert.equal(await caption.count(), 0)
    await page.evaluate(() => window.telescodeTour!.seek(2999))
    assert.equal(await caption.count(), 0)
    await page.evaluate(() => window.telescodeTour!.seek(3000))
    assert.equal(await caption.textContent(), '두 번째 설명')
  } finally { await browser?.close(); session.dispose(); await http.close() }
})

test('limits and unsupported targets reject instead of silently truncating', () => {
  assert.throws(() => makeStops(snapshot, Array(9).fill(stops[0])), /1–8/)
  assert.throws(() => makeStops(snapshot, [{ ...stops[0], holdMs: 60000 }]), /60 seconds/)
  assert.equal(makeStops(snapshot, [{ ...stops[0], transitionMs: 0, holdMs: 60000 }]).length, 1)
  assert.throws(() => makeStops(snapshot, [{ ...stops[0], nodeId: 'file:missing.py' }]), /Unsupported/)
  assert.throws(() => makeStops(snapshot, [{ ...stops[0], transitionMs: NaN }]), /transitionMs/)
  assert.throws(() => makeStops(snapshot, [{ ...stops[0], holdMs: 1000 }]), /holdMs/)
})
test('onboarding uses file ranks and class local ranks, not snapshot row order', () => {
  const first = snapshot.files[0].fileId
  const second = snapshot.files[1].fileId
  const ranked = { ...snapshot, readingSequence: [
    { entityId: first, entityType: 'file' as const, fileId: first, fileRank: 2, localRank: null, scores: { pagerank: null, betweenness: null, combined: null } },
    { entityId: second, entityType: 'file' as const, fileId: second, fileRank: 1, localRank: null, scores: { pagerank: null, betweenness: null, combined: null } },
  ] }
  assert.equal(readingTourOrder(ranked)[0].nodeId, `file:${second}`)
  assert.throws(() => makeStops(ranked, [{ ...stops[0], nodeId: `file:${first}` }, { ...stops[0], nodeId: `file:${second}` }]), /reading sequence order/)
  assert.throws(() => makeStops(snapshot, [{ ...stops[0], nodeId: `file:${first}` }]), /Reading sequence required/)
  const classRanked = { ...ranked,
    classes: [
      { ...snapshot.classes[0], classId: 'ranked-a', fileId: second },
      { ...snapshot.classes[0], classId: 'ranked-b', fileId: second },
    ],
    readingSequence: [...ranked.readingSequence,
      { entityId: 'ranked-a', entityType: 'class' as const, fileId: second, fileRank: null, localRank: 2, scores: { pagerank: null, betweenness: null, combined: null } },
      { entityId: 'ranked-b', entityType: 'class' as const, fileId: second, fileRank: null, localRank: 1, scores: { pagerank: null, betweenness: null, combined: null } },
    ],
  }
  assert.deepEqual(readingTourOrder(classRanked).slice(0, 3).map(n => n.nodeId), [`file:${second}`, 'class:ranked-b', 'class:ranked-a'])
  assert.throws(() => makeStops(classRanked, [{ ...stops[0], nodeId: 'class:ranked-a' }, { ...stops[0], nodeId: 'class:ranked-b' }]), /reading sequence order/)
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

test('list reads every saved draft from disk without its snapshot, skipping broken files', async () => {
  const output = await mkdtemp(join(tmpdir(), 'telescode-list-'))
  const store = new TourStore(output)
  const project = await store.open(fileURLToPath(fixture))
  const draft = await store.create(project.id, 'Listed', 'en', [{ nodeId: 'root', caption: 'Overview', transitionMs: 0, holdMs: 2000 }])
  const { writeFile } = await import('node:fs/promises')
  await writeFile(join(output, 'broken.draft.json'), '{ not json')
  // A second store stands in for another server process that saved after this one started.
  const other = new TourStore(output)
  const tours = await other.list()
  assert.equal(tours.length, 1)
  assert.equal(tours[0].plan.id, draft.plan.id)
  assert.equal(tours[0].snapshotHash, project.hash)
  assert.equal(tours[0].state, 'draft')
  assert.ok(tours[0].updatedMs > 0)
  assert.equal('project' in tours[0], false)
})
