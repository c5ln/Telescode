import { execFile } from 'node:child_process'
import { existsSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { promisify } from 'node:util'
import type { AnalysisSnapshot } from '../../frontend/src/bridge/models.ts'

export interface RankingContext { model: string; query: string; queryId: string }
export interface RankingResult extends RankingContext {
  files: { fileId: string; score: number; rank: number }[]
}

const root = fileURLToPath(new URL('../..', import.meta.url))
const extension = process.platform === 'win32' ? '.exe' : ''
export const defaultHeadless = () => process.env.TELESCODE_HEADLESS ?? join(root, `build/TelescodeHeadless${extension}`)

/** Overlay query-specific ranks; baseline scores remain inputs to future queries. */
export function applyRanking(snapshot: AnalysisSnapshot, ranking: RankingResult): AnalysisSnapshot {
  const invalid = () => new Error('Invalid LambdaRank ranking: expected every candidate once, finite descending scores and consecutive ranks')
  if (!ranking || typeof ranking.model !== 'string' || !ranking.model.trim() ||
      typeof ranking.query !== 'string' || !ranking.query.trim() ||
      typeof ranking.queryId !== 'string' || !ranking.queryId.trim() || !Array.isArray(ranking.files)) throw invalid()
  const candidates = new Set(snapshot.files.filter(f => f.fileId.replaceAll('\\', '/').split('/').at(-1) !== '__init__.py').map(f => f.fileId))
  const files = [...ranking.files].sort((a, b) => a.rank - b.rank)
  const seen = new Set<string>()
  if (!files.length || files.length !== candidates.size) throw invalid()
  for (const [i, file] of files.entries()) {
    if (!candidates.has(file.fileId) || seen.has(file.fileId) || file.rank !== i + 1 ||
        !Number.isFinite(file.score) || (i > 0 && files[i - 1].score < file.score)) throw invalid()
    seen.add(file.fileId)
  }
  const originals = new Map(snapshot.readingSequence.filter(s => s.entityType === 'file').map(s => [s.fileId, s]))
  const locals = new Map<string, AnalysisSnapshot['readingSequence']>()
  for (const row of snapshot.readingSequence) {
    if (row.entityType === 'file') continue
    const group = locals.get(row.fileId) ?? []
    group.push(row)
    locals.set(row.fileId, group)
  }
  const readingSequence = files.flatMap(file => {
    const original = originals.get(file.fileId)
    if (!original) throw new Error(`Invalid ranking baseline: missing file reading scores for ${file.fileId}; run algo`)
    return [
      { ...original, fileRank: file.rank, scores: { ...original.scores, lambdarank: file.score } },
      ...(locals.get(file.fileId) ?? []).toSorted((a, b) => (a.localRank ?? Infinity) - (b.localRank ?? Infinity)),
    ]
  })
  return { ...snapshot, totals: { ...snapshot.totals, sequenceCount: readingSequence.length }, readingSequence }
}

/** Existing DB opening uses onboarding; a future query UI can pass context here. */
export async function rankProject(snapshot: AnalysisSnapshot, db: string, headless: string, repoPath?: string) {
  if (!repoPath) throw new Error('Source root unavailable for LambdaRank: reopen the repository folder/URL or pass sourceRoot to open_project')
  const python = process.env.TELESCODE_LTR_PYTHON ?? [
    join(root, 'bench/.venv', process.platform === 'win32' ? 'Scripts/python.exe' : 'bin/python'),
    join(root, '.venv', process.platform === 'win32' ? 'Scripts/python.exe' : 'bin/python'),
  ].find(existsSync) ?? (process.platform === 'win32' ? 'python' : 'python3')
  const ppr = process.env.TELESCODE_PPR ?? [
    join(dirname(headless), `TelescodePPR${extension}`),
    ...['', 'Release', 'RelWithDebInfo', 'MinSizeRel', 'Debug'].map(config => join(root, 'build', config, `TelescodePPR${extension}`)),
  ].find(existsSync) ?? join(root, `build/TelescodePPR${extension}`)
  try {
    const { stdout } = await promisify(execFile)(python, [
      '-m', 'ltr.tour_rank', '--repo', repoPath, '--db', db,
      '--headless-bin', headless, '--ppr-bin', ppr,
      '--model', join(root, 'ltr/models/lambdarank-v1'),
    ], { cwd: root, timeout: 240000, maxBuffer: 128 * 1024 * 1024 })
    const result: RankingResult = JSON.parse(stdout)
    const ranked = applyRanking(snapshot, result)
    const { model, query, queryId } = result
    return { snapshot: ranked, ranking: { model, query, queryId } }
  } catch (error) {
    const stderr = (error as { stderr?: string }).stderr
    throw new Error(`LambdaRank tour ranking failed: ${stderr?.trim() || String(error)}`)
  }
}
