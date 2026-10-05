import { readFile, mkdir, writeFile } from 'node:fs/promises'
import { resolve } from 'node:path'
import { fileURLToPath } from 'node:url'
import { TourStore } from './store.ts'
import { readingTourOrder, durationMs, toVtt, type StopInput } from '../../frontend/src/tour/model.ts'

// Materialize an agent-authored plan against actual core results; never inject ranks.
const root = fileURLToPath(new URL('../..', import.meta.url))
const specPath = resolve(process.argv[2] ?? resolve(root, 'mcp/tours/sherlock-onboarding.json'))
const dbPath = resolve(process.argv[3] ?? resolve(root, 'mcp/artifacts/onboarding-source/sherlock-reading.db'))
const spec = JSON.parse(await readFile(specPath, 'utf8')) as {
  title: string; language: 'ko' | 'en'; repositoryCommit: string; prompt: string;
  stops: (StopInput & { sourceReferences: string[] })[]
}
const store = new TourStore(resolve(root, 'mcp/artifacts'))
const project = await store.open(dbPath, resolve(root, 'build/TelescodeHeadless'))
const inputs = spec.stops.map(({ nodeId, caption, transitionMs, holdMs }) => ({ nodeId, caption, transitionMs, holdMs }))
const draft = await store.create(project.id, spec.title, spec.language, inputs)
const output = resolve(store.outputRoot, draft.plan.id, 'draft')
await mkdir(output, { recursive: true })
const order = readingTourOrder(project.snapshot)
let ms = 0
const rows = draft.plan.stops.map((s, i) => {
  const start = ms
  ms += s.transitionMs + s.holdMs
  const rank = order.find(n => n.nodeId === s.nodeId)
  return `${i + 1}. **${s.title}** (${start / 1000}–${ms / 1000}초)${rank ? ` · 파일 순위 #${rank.fileRank}${rank.localRank != null ? ` · 클래스 localRank #${rank.localRank}` : ''}` : ''}\n\n   ${s.caption.replace(/\n/g, '\n   ')}\n\n   소스 근거: ${spec.stops[i].sourceReferences.join(', ')}\n`
})
const document = `# ${draft.plan.title}\n\n목적: 신규 개발자 온보딩\n\n상태: 검토용 초안 · ${durationMs(draft.plan) / 1000}초 · ${draft.plan.stops.length}개 지점\n\n실제 입력: ${dbPath}\n\n저장소 커밋: ${spec.repositoryCommit}\n\n분석 결과: ${project.snapshot.readingSequence.length}개 reading sequence 항목\n\n프롬프트: ${spec.prompt}\n\n순서는 실제 fileRank와 class localRank를 검증했다. 역할 설명은 저장소 소스를 읽고 작성했다. 함수/멤버는 별도 방문하지 않는다.\n\n${rows.join('\n')}`
await Promise.all([
  writeFile(resolve(output, 'onboarding-plan.md'), document),
  writeFile(resolve(output, 'tour.json'), JSON.stringify(draft.plan, null, 2)),
  writeFile(resolve(output, 'tour.vtt'), toVtt(draft.plan)),
  writeFile(resolve(output, 'snapshot.json'), JSON.stringify(project.snapshot, null, 2)),
  writeFile(resolve(output, 'source-evidence.json'), JSON.stringify({ repositoryCommit: spec.repositoryCommit, prompt: spec.prompt, stops: spec.stops.map(s => ({ nodeId: s.nodeId, sourceReferences: s.sourceReferences })) }, null, 2)),
])
console.log(JSON.stringify({ tourId: draft.plan.id, projectId: project.id, durationMs: durationMs(draft.plan), state: draft.state, outputDirectory: output, readingSequenceEntries: project.snapshot.readingSequence.length }, null, 2))
