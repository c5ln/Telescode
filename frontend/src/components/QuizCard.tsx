// The tutorial panel while a quiz is open on a step: the question, its
// choices, and after answering, whether it was right and why, with the nodes
// the answer rests on. Takes the panel's place rather than stacking over it.

import type { KeyboardEvent } from 'react'

import { questionsOf, type Quiz } from '../app/useQuiz'
import type { SetupPhase } from '../app/useTutorial'
import { DIFFICULTIES, type Difficulty } from '../quiz/model'
import { Button } from '../ui/Button'
import { CloseIcon } from '../ui/icons'
import { IconButton } from '../ui/IconButton'
import { Spinner } from '../ui/Spinner'
import styles from './QuizCard.module.css'
import panel from './TutorialPanel.module.css'

const SETUP_TITLES: Record<SetupPhase, string> = {
  generating: 'Writing a quiz on this step',
  'signing-in': 'Sign in to Claude to write a quiz on this step',
  'not-installed': 'Claude Code is needed to write quizzes',
}

const LABELS: Record<Difficulty, string> = { easy: 'Easy', medium: 'Medium', hard: 'Hard' }

interface QuizCardProps {
  quiz: Quiz
  /** The step's title, for the eyebrow. */
  stepTitle: string
}

export function QuizCard({ quiz, stepTitle }: QuizCardProps) {
  const { state } = quiz
  if (state.status === 'off') return null

  const onKeyDown = (e: KeyboardEvent) => {
    if (e.key !== 'Escape') return
    e.preventDefault()
    quiz.close()
  }

  const header = (
    <header className={panel.header}>
      <p className={panel.eyebrow}>Quiz · {stepTitle}</p>
      <IconButton label="Back to tutorial" tooltip="top" tooltipAlign="end" onClick={quiz.close}>
        <CloseIcon />
      </IconButton>
    </header>
  )

  if (state.status === 'searching') {
    return (
      <section className={panel.panel} aria-label="Quiz" onKeyDown={onKeyDown}>
        {header}
        <p className={panel.waiting} role="status">
          <Spinner />
          Looking for a quiz…
        </p>
      </section>
    )
  }

  if (state.status === 'setup') {
    return (
      <section className={panel.panel} aria-label="Quiz" onKeyDown={onKeyDown}>
        {header}
        <h2 className={panel.title}>{SETUP_TITLES[state.phase]}</h2>
        {state.phase === 'not-installed' ? (
          <p className={panel.text}>Quizzes are written by Claude Code with your Claude account. Install it, then try again.</p>
        ) : state.error ? (
          <p className={panel.error}>Cannot write the quiz: {state.error}</p>
        ) : (
          <p className={panel.waiting} role="status">
            <Spinner />
            {state.phase === 'signing-in'
              ? 'Finish signing in to Claude in your browser. The quiz is written right after.'
              : 'Writing questions from the code map… They appear here when ready.'}
          </p>
        )}
      </section>
    )
  }

  const questions = questionsOf(state.set, state.difficulty)
  const question = questions[state.index]
  const { answer } = state
  const feedback = answer.status === 'answered' ? answer.feedback : null
  const locked = answer.status === 'answered' || answer.status === 'submitting'

  return (
    <section className={panel.panel} aria-label="Quiz" onKeyDown={onKeyDown}>
      {header}
      <div className={styles.difficulties} role="group" aria-label="Difficulty">
        {DIFFICULTIES.map((d) => {
          const count = questionsOf(state.set, d).length
          return (
            <button
              key={d}
              type="button"
              className={styles.difficulty}
              aria-pressed={d === state.difficulty}
              disabled={count === 0 || answer.status === 'submitting'}
              onClick={() => quiz.setDifficulty(d)}
            >
              {LABELS[d]}
            </button>
          )
        })}
      </div>
      {question && (
        <>
          <p className={styles.meta}>
            Question {state.index + 1} of {questions.length} · {question.concept}
          </p>
          <h2 className={panel.title}>{question.question}</h2>
          <fieldset className={styles.choices} disabled={locked}>
            <legend className={styles.hidden}>Choices</legend>
            {question.choices.map((choice, i) => {
              const verdict = feedback && (choice === feedback.answer ? 'correct' : i === state.selected ? 'incorrect' : undefined)
              return (
                <label key={i} className={styles.choice} data-state={verdict}>
                  <input
                    type="radio"
                    name={question.id}
                    checked={state.selected === i}
                    onChange={() => quiz.select(i)}
                  />
                  <span>{choice}</span>
                </label>
              )
            })}
          </fieldset>
          {answer.status === 'error' && <p className={panel.error}>Cannot record the answer: {answer.message}</p>}
          {feedback && (
            <div className={styles.feedback} role="status" data-correct={feedback.correct}>
              <p className={styles.verdict}>
                {feedback.correct ? 'Correct.' : `Not quite. The answer is “${feedback.answer}”.`}
              </p>
              <p className={panel.text}>{feedback.explanation}</p>
              {feedback.evidence.length > 0 && (
                <div className={styles.evidence}>
                  <span>Based on</span>
                  {feedback.evidence.map((e) => (
                    <button key={e.nodeId} type="button" className={styles.node} onClick={() => quiz.showNode(e.nodeId)}>
                      {e.title}
                    </button>
                  ))}
                </div>
              )}
            </div>
          )}
          <div className={panel.actions}>
            {feedback ? (
              <>
                {!feedback.correct && <Button onClick={quiz.retry}>Try again</Button>}
                {questions.length > 1 && (
                  <Button variant="primary" onClick={quiz.next}>
                    Next question
                  </Button>
                )}
              </>
            ) : (
              <Button variant="primary" disabled={state.selected === null || answer.status === 'submitting'} onClick={quiz.submit}>
                Check answer
              </Button>
            )}
          </div>
        </>
      )}
    </section>
  )
}
