// Tutorial mode's floating card over the map: the current step's explanation
// and playback controls, or, before a tutorial exists, its generation.

import type { KeyboardEvent } from 'react'

import type { Tutorial } from '../app/useTutorial'
import { formatEvidenceValue } from '../tour/model'
import { stepDurationMs } from '../tour/tutorial'
import { Button } from '../ui/Button'
import { ChevronLeftIcon, ChevronRightIcon, CloseIcon, PauseIcon, PlayIcon } from '../ui/icons'
import { IconButton } from '../ui/IconButton'
import { Spinner } from '../ui/Spinner'
import styles from './TutorialPanel.module.css'

interface TutorialPanelProps {
  tutorial: Tutorial
  repositoryName: string
}

export function TutorialPanel({ tutorial, repositoryName }: TutorialPanelProps) {
  const { state } = tutorial
  if (state.status === 'off') return null

  const onKeyDown = (e: KeyboardEvent) => {
    if (e.key === 'Escape') tutorial.exit()
    else if (state.status !== 'playing') return
    else if (e.key === 'ArrowLeft') tutorial.goTo(state.index - 1)
    else if (e.key === 'ArrowRight') tutorial.goTo(state.index + 1)
    else return
    e.preventDefault()
  }

  const close = (
    <IconButton label="Exit tutorial" tooltip="top" tooltipAlign="end" onClick={tutorial.exit}>
      <CloseIcon />
    </IconButton>
  )

  if (state.status === 'searching') {
    return (
      <section className={styles.panel} aria-label="Tutorial" onKeyDown={onKeyDown}>
        <p className={styles.waiting} role="status">
          <Spinner />
          Looking for a tutorial…
        </p>
      </section>
    )
  }

  if (state.status === 'setup') {
    const { ready, error } = state
    return (
      <section className={styles.panel} aria-label="Tutorial" onKeyDown={onKeyDown}>
        <header className={styles.header}>
          <p className={styles.eyebrow}>Tutorial</p>
          {close}
        </header>
        {ready ? (
          <>
            <h2 className={styles.title}>Tutorial ready</h2>
            <p className={styles.text}>
              {ready.plan.title} · {ready.plan.stops.length} steps
            </p>
            <div className={styles.actions}>
              <Button variant="primary" onClick={() => tutorial.play(ready)}>
                Start tutorial
              </Button>
            </div>
          </>
        ) : (
          <>
            <h2 className={styles.title}>Generating a tutorial for {repositoryName}</h2>
            {error ? (
              <p className={styles.error}>Cannot generate the tutorial: {error}</p>
            ) : (
              <p className={styles.waiting} role="status">
                <Spinner />
                Generating… It appears here as soon as it is ready.
              </p>
            )}
          </>
        )}
      </section>
    )
  }

  const { plan } = state.tour
  const stop = plan.stops[state.index]!
  const last = plan.stops.length - 1
  return (
    <section className={styles.panel} aria-label="Tutorial" onKeyDown={onKeyDown}>
      <header className={styles.header}>
        <p className={styles.eyebrow}>Tutorial</p>
        {close}
      </header>
      <div aria-live="polite">
        <h2 className={styles.title}>{stop.title || stop.nodeId}</h2>
        <p className={styles.caption}>{stop.caption}</p>
        {stop.evidence?.length > 0 && (
          <dl className={styles.evidence}>
            {stop.evidence.map((e) => (
              <div key={e.label}>
                <dt>{e.label}</dt>
                <dd>{formatEvidenceValue(e)}</dd>
              </div>
            ))}
          </dl>
        )}
      </div>
      <footer className={styles.footer}>
        <div className={styles.controls} role="toolbar" aria-label="Tutorial playback">
          <IconButton label="Previous step" tooltip="top" disabled={state.index === 0} onClick={() => tutorial.goTo(state.index - 1)}>
            <ChevronLeftIcon />
          </IconButton>
          <IconButton label={state.playing ? 'Pause' : 'Play'} tooltip="top" onClick={tutorial.togglePlaying}>
            {state.playing ? <PauseIcon /> : <PlayIcon />}
          </IconButton>
          <IconButton label="Next step" tooltip="top" disabled={state.index === last} onClick={() => tutorial.goTo(state.index + 1)}>
            <ChevronRightIcon />
          </IconButton>
        </div>
        <ol className={styles.track} aria-hidden="true">
          {plan.stops.map((s, i) => (
            <li key={i} data-state={i < state.index || (i === state.index && state.finished) ? 'done' : i === state.index ? 'current' : undefined}>
              {/* Fills over the step's time; mounted afresh for each step, and frozen while paused. */}
              {i === state.index && !state.finished && (
                <span
                  className={styles.fill}
                  data-testid="step-progress"
                  style={{
                    animationDuration: `${stepDurationMs(s)}ms`,
                    animationPlayState: state.playing ? 'running' : 'paused',
                  }}
                />
              )}
            </li>
          ))}
        </ol>
        <p className={styles.progress} aria-label={`Step ${state.index + 1} of ${plan.stops.length}`}>
          {state.index + 1} / {plan.stops.length}
        </p>
      </footer>
    </section>
  )
}
