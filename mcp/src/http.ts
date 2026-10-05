import { createServer, type ServerResponse } from 'node:http'
import { readFile, stat } from 'node:fs/promises'
import { resolve, extname, sep } from 'node:path'
import { randomUUID } from 'node:crypto'
import type { TourStore, Draft } from './store.ts'
import { durationMs, formatEvidenceValue } from '../../frontend/src/tour/model.ts'

export const escapeHtml = (s: string) => s.replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]!))
const html = (body: string) => `<!doctype html><html lang="ko"><meta charset="utf-8"><meta name="viewport" content="width=device-width"><title>Telescode tour</title><style>body{font:18px system-ui;max-width:960px;margin:40px auto;padding:20px;background:#121212;color:#eee}li{margin:24px 0}button{font:inherit;padding:12px 24px;cursor:pointer}small{color:#aaa}video{width:100%}a{color:#9cc8ff}</style>${body}</html>`
function send(res: ServerResponse, status: number, type: string, body: string | Buffer) {
  res.writeHead(status, { 'Content-Type': type, 'Cache-Control': 'no-store', 'X-Content-Type-Options': 'nosniff', 'Referrer-Policy': 'same-origin' })
  res.end(body)
}
const types: Record<string, string> = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript', '.css': 'text/css', '.woff2': 'font/woff2', '.mp4': 'video/mp4', '.webm': 'video/webm', '.vtt': 'text/vtt; charset=utf-8', '.json': 'application/json' }

export async function startHttp(store: TourStore, frontendDist: string) {
  const renderPayloads = new Map<string, { plan: Draft['plan']; snapshot: Draft['project']['snapshot'] }>()
  const server = createServer(async (req, res) => {
    try {
      const url = new URL(req.url ?? '/', 'http://localhost')
      if (url.pathname === '/tour-api/payload') {
        const payload = renderPayloads.get(url.searchParams.get('token') ?? '')
        if (!payload) return send(res, 403, 'text/plain', 'Invalid render session')
        return send(res, 200, 'application/json', JSON.stringify(payload))
      }
      const review = /^\/review\/([\w-]+)$/.exec(url.pathname)
      if (review) {
        const draft = store.draft(review[1])
        const token = url.searchParams.get('token') ?? ''
        if (token !== draft.reviewToken) return send(res, 403, 'text/plain', 'Review expired')
        if (req.method === 'POST') {
          if (req.headers.origin !== baseUrl) return send(res, 403, 'text/plain', 'Invalid approval origin')
          await store.approve(draft.plan.id, Number(url.searchParams.get('revision')), token)
          return send(res, 200, 'text/html; charset=utf-8', html('<h1>투어 승인 완료</h1><p>에이전트에게 돌아가서 영상 생성을 요청하세요.</p>'))
        }
        if (req.method !== 'GET') return send(res, 405, 'text/plain', 'Method not allowed')
        const p = draft.plan
        const stops = p.stops.map(s => `<li><strong>${escapeHtml(s.title)}</strong> · ${(s.transitionMs + s.holdMs) / 1000}초<p>${escapeHtml(s.caption)}</p><small>${s.evidence.map(e => `${escapeHtml(e.label)}: ${escapeHtml(formatEvidenceValue(e))}`).join(' · ')}</small></li>`).join('')
        const action = `/review/${p.id}?token=${token}&revision=${p.revision}`
        return send(res, 200, 'text/html; charset=utf-8', html(`<h1>${escapeHtml(p.title)}</h1><p>Revision ${p.revision} · ${durationMs(p) / 1000}초 · ${p.stops.length}개 정차 지점</p><p>무음 MP4 · 자막 영상 합성 + 참고용 VTT · 분석 근거 패널 포함</p><ol>${stops}</ol>${draft.state === 'draft' ? `<form method="post" action="${action}"><button>이 계획으로 영상 생성 승인</button></form>` : `<p>상태: ${draft.state}</p>`}`))
      }
      if (req.method !== 'GET') return send(res, 405, 'text/plain', 'Method not allowed')
      let root = frontendDist
      let filePath = url.pathname === '/tour-render' ? '/index.html' : decodeURIComponent(url.pathname)
      const artifact = /^\/artifacts\/([\w-]+)\/(.+)$/.exec(filePath)
      if (artifact) {
        const draft = store.draft(artifact[1])
        if (url.searchParams.get('token') !== draft.reviewToken || draft.state !== 'completed' || !draft.output) return send(res, 403, 'text/plain', 'Artifact unavailable')
        root = draft.output
        filePath = '/' + artifact[2]
      }
      const path = resolve(root, '.' + filePath)
      if (!path.startsWith(resolve(root) + sep)) return send(res, 403, 'text/plain', 'Invalid path')
      if (!(await stat(path)).isFile()) return send(res, 404, 'text/plain', 'Not found')
      send(res, 200, types[extname(path)] ?? 'application/octet-stream', await readFile(path))
    } catch (e) { send(res, 400, 'text/plain', String(e)) }
  })
  await new Promise<void>(resolve => server.listen(0, '127.0.0.1', resolve))
  const address = server.address()
  if (!address || typeof address === 'string') throw new Error('No HTTP address')
  const baseUrl = `http://127.0.0.1:${address.port}`
  return {
    baseUrl,
    reviewUrl: (d: Draft) => `${baseUrl}/review/${d.plan.id}?token=${d.reviewToken}&revision=${d.plan.revision}`,
    playerUrl: (d: Draft) => `${baseUrl}/artifacts/${d.plan.id}/player.html?token=${d.reviewToken}`,
    registerRender(d: Draft) {
      const token = randomUUID()
      renderPayloads.set(token, structuredClone({ plan: d.plan, snapshot: d.project.snapshot }))
      return { url: `${baseUrl}/tour-render?token=${token}`, dispose: () => renderPayloads.delete(token) }
    },
    close: () => new Promise<void>((resolve, reject) => server.close(e => e ? reject(e) : resolve())),
  }
}
