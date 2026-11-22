// What the AI agent is doing while it writes a tour, a quiz or a learning
// path: the step it is on, read from the Telescode tools it calls (nothing is
// asked of the agent for this), the file it is reading, and how long it has
// been at it. Writing takes minutes, so this is where the waiting is.

import { useEffect, useState } from 'react'

import type { AgentActivity } from '../agent'
import { CheckIcon } from '../ui/icons'
import { Spinner } from '../ui/Spinner'
import { duration } from './LoadingProgress'
import styles from './AgentProgress.module.css'

export type AgentWork = 'tour' | 'quiz' | 'learning path'

interface Step {
  label: string
  tools: string[]
}

/** The steps each kind of writing goes through, by the tools that mark them. */
const STEPS: Record<AgentWork, Step[]> = {
  tour: [
    { label: 'Read the analysis', tools: ['open_project', 'get_project_overview'] },
    { label: 'Choose the reading order', tools: ['get_reading_tour_order'] },
    { label: 'Read the files', tools: ['inspect_node', 'search_nodes'] },
    { label: 'Write the steps', tools: ['create_tour_draft', 'revise_tour_draft'] },
    { label: 'Check the tour', tools: ['get_tour_status'] },
  ],
  quiz: [
    { label: 'Read the analysis', tools: ['open_project'] },
    { label: 'Gather the code for each difficulty', tools: ['get_quiz_context', 'inspect_node'] },
    { label: 'Write the questions', tools: ['create_quiz'] },
  ],
  'learning path': [
    { label: 'Read your goal and the code', tools: ['get_learning_preparation_context'] },
    { label: 'Write the learning path', tools: ['save_learning_curriculum'] },
  ],
}

const NOTES: Record<AgentWork, string> = {
  tour: 'Writing a tour can take a few minutes. You can keep exploring the map meanwhile.',
  quiz: 'Writing a quiz can take a minute or two.',
  'learning path': 'Preparing a learning path can take a few minutes.',
}

/** A node as the user knows it: its path, without the kind prefix. */
function nodeName(id: string) {
  return id.replace(/^(file|dir|class):/, '').replace('::', ' › ')
}

function useNow(intervalMs = 500) {
  const [now, setNow] = useState(() => Date.now())
  useEffect(() => {
    const timer = setInterval(() => setNow(Date.now()), intervalMs)
    return () => clearInterval(timer)
  }, [intervalMs])
  return now
}

interface AgentProgressProps {
  work: AgentWork
  /** The agent's name, e.g. "Claude Code". */
  agent: string
  activity: AgentActivity
}

export function AgentProgress({ work, agent, activity }: AgentProgressProps) {
  const now = useNow()
  const steps = STEPS[work]
  // The furthest step reached, and when each step was first reached.
  const reachedAt = steps.map((s) => activity.calls.find((c) => s.tools.includes(c.call.tool))?.atMs)
  const furthest = reachedAt.reduce<number>((last, at, i) => (at !== undefined ? i : last), -1)
  const startAt = (i: number) => (i < 0 ? activity.startedMs : (reachedAt[i] ?? activity.startedMs))

  const read = new Set(activity.calls.map((c) => c.call.nodeId).filter((id): id is string => Boolean(id)))
  const latest = [...activity.calls].reverse().find((c) => c.call.nodeId)?.call.nodeId

  const rows = [{ label: `Start ${agent}` }, ...steps]
  return (
    <div className={styles.progress}>
      <ol className={styles.steps} aria-label="Steps">
        {rows.map((row, i) => {
          const step = i - 1 // -1 is starting the agent
          const state = step < furthest ? 'done' : step === furthest ? 'now' : 'pending'
          const from = startAt(step)
          const to = step < furthest ? startAt(step + 1) : now
          const detail =
            state !== 'pending' && read.size > 0 && 'tools' in row && row.tools.includes('inspect_node')
              ? `${read.size} ${read.size === 1 ? 'part' : 'parts'} of the code read`
              : null
          return (
            <li key={row.label} data-state={state}>
              <span className={styles.mark} aria-hidden="true">
                {state === 'done' ? <CheckIcon /> : state === 'now' ? <Spinner /> : <span className={styles.dot} />}
              </span>
              <span className={styles.label}>
                {state === 'now' ? <span role="status">{row.label}…</span> : row.label}
                {detail && <span className={styles.detail}>{detail}</span>}
                {state === 'now' && latest && 'tools' in row && row.tools.includes('inspect_node') && (
                  <span className={styles.file}>{nodeName(latest)}</span>
                )}
              </span>
              <span className={styles.time}>{state === 'pending' ? '' : duration(Math.max(0, to - from))}</span>
            </li>
          )
        })}
      </ol>
      <p className={styles.foot}>
        <span>
          Elapsed <strong>{duration(now - activity.startedMs)}</strong>
        </span>
        <span>{NOTES[work]}</span>
      </p>
    </div>
  )
}
