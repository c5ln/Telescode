// Tutorial mode's floating card over the map: the current step's explanation
// and playback controls, or, before a tutorial exists, its generation. A quiz
// on the step takes the card's place until the user goes back to the tutorial.

import type { KeyboardEvent } from 'react'

import { agents, installCommand, type AgentProvider } from '../agent'
import type { Quiz } from '../app/useQuiz'
import type { Learning } from '../app/useLearning'
import type { SetupPhase, Tutorial } from '../app/useTutorial'
import { formatEvidenceValue } from '../tour/model'
import { stepDurationMs } from '../tour/tutorial'
import { Button } from '../ui/Button'
import { ChevronLeftIcon, ChevronRightIcon, CloseIcon, PauseIcon, PlayIcon, QuizIcon } from '../ui/icons'
import { IconButton } from '../ui/IconButton'
import { Tooltip } from '../ui/Tooltip'
import { Spinner } from '../ui/Spinner'
import { AgentError } from './AgentError'
import { QuizCard } from './QuizCard'
import { TourStartPanel } from './TourStartPanel'
import styles from './TutorialPanel.module.css'

const SETUP_TITLES: Record<SetupPhase, (repository: string, account: string, name: string) => string> = {
  generating: (r) => `Writing a tour for ${r}`,
  'signing-in': (r, account) => `Sign in to ${account} to write a tour for ${r}`,
  'not-installed': (_r, _account, name) => `${name} is needed to write tours`,
}

interface TutorialPanelProps {
  provider?: AgentProvider
  /** Switch to another agent, offered when writing fails. */
  onSwitchAgent?: (provider: AgentProvider) => void
  tutorial: Tutorial
  quiz: Quiz
  repositoryName: string
  learning?: Learning
}

export function TutorialPanel({ tutorial, quiz, repositoryName, provider = 'claude', learning, onSwitchAgent }: TutorialPanelProps) {
  const { name, account } = agents[provider]
  const { state } = tutorial
  if (state.status === 'off') return null

  if (state.status === 'playing' && quiz.state.status !== 'off') {
    const stop = state.tour.plan.stops[quiz.state.at.stopIndex]
    return <QuizCard provider={provider} onSwitchAgent={onSwitchAgent} quiz={quiz} stepTitle={stop?.title || quiz.state.at.nodeId} />
  }

  const exit = () => {
    quiz.close()
    tutorial.exit()
  }

  if (state.status === 'choosing') return <TourStartPanel onClose={exit} onStart={requirements => {
    if (requirements && learning) void learning.start(requirements)
    else tutorial.start()
  }} />

  const onKeyDown = (e: KeyboardEvent) => {
    if (e.key === 'Escape') exit()
    else if (state.status !== 'playing') return
    else if (e.key === 'ArrowLeft') tutorial.goTo(state.index - 1)
    else if (e.key === 'ArrowRight') tutorial.goTo(state.index + 1)
    else return
    e.preventDefault()
  }

  const close = (
    <IconButton label="Exit tour" tooltip="top" tooltipAlign="end" onClick={exit}>
      <CloseIcon />
    </IconButton>
  )

  if (state.status === 'searching') {
    return (
      <section className={styles.panel} aria-label="Code tour" onKeyDown={onKeyDown}>
        <p className={styles.waiting} role="status">
          <Spinner />
          Looking for a tour…
        </p>
      </section>
    )
  }

  if (state.status === 'setup') {
    const { ready, error } = state
    return (
      <section className={styles.panel} aria-label="Code tour" onKeyDown={onKeyDown}>
        <header className={styles.header}>
          <p className={styles.eyebrow}>Code tour</p>
          {close}
        </header>
        {ready ? (
          <>
            <h2 className={styles.title}>Tour ready</h2>
            <p className={styles.text}>
              {ready.plan.title} · {ready.plan.stops.length} steps
            </p>
            <div className={styles.actions}>
              <Button variant="primary" onClick={() => tutorial.play(ready)}>
                Start tour
              </Button>
            </div>
          </>
        ) : (
          <>
            <h2 className={styles.title}>{SETUP_TITLES[state.phase](repositoryName, account, name)}</h2>
            {state.phase === 'not-installed' ? (
              <>
                <p className={styles.text}>
                  Tours are written by {name} with your {account} account. Install it, then press Start tour again:
                </p>
                <code className={styles.command}>{installCommand(provider)}</code>
              </>
            ) : error ? (
              <AgentError what="tour" signingIn={state.phase === 'signing-in'} message={error} provider={provider}
                onRetry={tutorial.start} onSwitchAgent={onSwitchAgent} />
            ) : (
              <p className={styles.waiting} role="status">
                <Spinner />
                {state.phase === 'signing-in'
                  ? `Finish signing in to ${account} in your browser. The tour is written right after.`
                  : 'Writing… It appears here as soon as it is ready.'}
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
  const startQuiz = () => {
    // The step stays put while the user answers.
    if (state.playing && !state.finished) tutorial.togglePlaying()
    quiz.open(state.tour, state.index)
  }
  return (
    <section className={styles.panel} aria-label="Code tour" onKeyDown={onKeyDown}>
      <header className={styles.header}>
        <p className={styles.eyebrow}>Code tour</p>
        <Tooltip
          label="Quiz on this step"
          description="One question about the code in this step. Pick easy, medium or hard, and see why the answer is right."
          align="end"
        >
          <button type="button" className={styles.quiz} onClick={startQuiz}>
            <QuizIcon />
            Quiz
          </button>
        </Tooltip>
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
      {learning && <div className={styles.actions}>
        <Tooltip
          label="Learn adaptively"
          description="Tell it what you want to learn. Each quiz answer updates how well you seem to know each part, and the next step goes where that estimate can rise the most."
          side="top"
          align="end"
        >
          <Button onClick={() => {
            if (state.playing && !state.finished) tutorial.togglePlaying()
            quiz.close()
            void learning.start()
          }}>Learn adaptively</Button>
        </Tooltip>
      </div>}
      <footer className={styles.footer}>
        <div className={styles.controls} role="toolbar" aria-label="Tour playback">
          <IconButton label="Previous step" tooltip="top" tooltipAlign="start" disabled={state.index === 0} onClick={() => tutorial.goTo(state.index - 1)}>
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
