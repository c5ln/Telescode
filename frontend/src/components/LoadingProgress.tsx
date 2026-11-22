// Opening a repository, step by step: what is happening now, how long each
// step took, a small map that fills in as the code is read, and how long a
// repository like this one usually takes. Most open
// in a few seconds, so the screen stays light; it is there for the larger
// ones and for slow connections.

import { useEffect, useState } from 'react'

import type { LoadingProgress as Progress, LoadingStage } from '../app/useWorkspace'
import { Button } from '../ui/Button'
import { CheckIcon } from '../ui/icons'
import { Spinner } from '../ui/Spinner'
import { ReadMap } from './ReadMap'
import styles from './LoadingProgress.module.css'

const LABELS: Record<LoadingStage, string> = {
  download: 'Download',
  read: 'Read the code',
  rank: 'Rank files',
  draw: 'Draw the map',
}

interface LoadingProgressProps {
  /** What is being opened, e.g. "psf/requests". */
  name: string
  /** A GitHub repository is downloaded first; a folder is read in place. */
  downloads: boolean
  progress: Progress
  onCancel: () => void
}

/** Seconds under a minute to one decimal, then minutes and seconds. */
export function duration(ms: number): string {
  const s = Math.max(0, ms) / 1000
  if (s < 60) return `${s.toFixed(1)} s`
  return `${Math.floor(s / 60)}:${String(Math.floor(s % 60)).padStart(2, '0')}`
}

/** How long repositories like this one usually take, from measured opens (download included). */
export function estimate(pythonFiles: number | null | undefined): string {
  if (pythonFiles == null) return 'Most repositories open in a few seconds.'
  const files = `${pythonFiles.toLocaleString('en-US')} Python ${pythonFiles === 1 ? 'file' : 'files'}`
  if (pythonFiles < 500) return `Repositories with ${files} usually open in a few seconds.`
  if (pythonFiles < 2000) return `Repositories with ${files} usually open in 5 to 15 seconds.`
  return `Repositories with ${files} can take up to a minute.`
}

function megabytes(bytes: number) {
  return `${(bytes / (1024 * 1024)).toFixed(1)} MB`
}

function useNow(intervalMs = 200) {
  const [now, setNow] = useState(() => Date.now())
  useEffect(() => {
    const timer = setInterval(() => setNow(Date.now()), intervalMs)
    return () => clearInterval(timer)
  }, [intervalMs])
  return now
}

export function LoadingProgress({ name, downloads, progress, onCancel }: LoadingProgressProps) {
  const now = useNow()
  const reached = progress.stages
  const current = reached[reached.length - 1]?.stage
  // A re-analysis only draws the map again.
  const planned: LoadingStage[] =
    reached[0]?.stage === 'draw' ? ['draw'] : downloads ? ['download', 'read', 'rank', 'draw'] : ['read', 'rank', 'draw']

  const meta = [
    progress.pythonFiles != null && `${progress.pythonFiles.toLocaleString('en-US')} Python files`,
    progress.bytes ? `${megabytes(progress.bytes)} downloaded` : null,
  ].filter(Boolean)

  return (
    <div className={styles.loading} aria-busy="true">
      <p className={styles.eyebrow}>Opening</p>
      <h2 className={styles.name}>{name}</h2>
      {meta.length > 0 && <p className={styles.meta}>{meta.join(', ')}</p>}
      <ol className={styles.stages} aria-label="Steps">
        {planned.map((stage) => {
          const at = reached.findIndex((r) => r.stage === stage)
          const state = at < 0 ? 'pending' : stage === current ? 'now' : 'done'
          const started = reached[at]?.startedMs ?? 0
          const ended = reached[at + 1]?.startedMs ?? now
          return (
            <li key={stage} data-state={state}>
              <span className={styles.mark} aria-hidden="true">
                {state === 'done' ? <CheckIcon /> : state === 'now' ? <Spinner /> : <span className={styles.dot} />}
              </span>
              <span className={styles.label}>
                {state === 'now' ? <span role="status">{LABELS[stage]}…</span> : LABELS[stage]}
              </span>
              <span className={styles.time}>{state === 'pending' ? '' : duration(ended - started)}</span>
            </li>
          )
        })}
      </ol>
      {progress.found?.length ? <ReadMap found={progress.found} read={progress.read ?? []} /> : null}
      <p className={styles.foot}>
        <span>
          Elapsed <strong>{duration(now - progress.startedMs)}</strong>
        </span>
        <span>{estimate(progress.pythonFiles)}</span>
      </p>
      <Button onClick={onCancel}>Cancel</Button>
    </div>
  )
}
