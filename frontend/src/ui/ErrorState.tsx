// Every failure in one layout: what happened, why, what to do next, the
// actions that fit, and the technical details folded away for a bug report.
//
// `center` fills an empty view; `inline` sits inside a panel, left aligned.

import { useState, type ReactNode } from 'react'

import { Button } from './Button'
import { ChevronRightIcon } from './icons'
import styles from './States.module.css'

interface ErrorStateProps {
  title: string
  /** Why it happened, in one line. */
  cause?: ReactNode
  /** What the user can do about it. */
  next?: ReactNode
  /** Red for failures, amber for things outside what Telescode supports. */
  tone?: 'error' | 'warning'
  layout?: 'center' | 'inline'
  /** Code, message and stderr, shown under Details with a copy button. */
  details?: string
  /** Actions, the main one first. */
  children?: ReactNode
}

export function ErrorState({ title, cause, next, tone = 'error', layout = 'center', details, children }: ErrorStateProps) {
  return (
    <div className={styles.state} data-layout={layout} role="alert">
      <p className={styles.title}>
        <span className={styles.errorMark} data-tone={tone} aria-hidden="true" />
        {title}
      </p>
      {cause && <p className={styles.description}>{cause}</p>}
      {next && <p className={styles.next}>{next}</p>}
      {children && <div className={styles.actions}>{children}</div>}
      {details && <ErrorDetails text={details} />}
    </div>
  )
}

function ErrorDetails({ text }: { text: string }) {
  const [copied, setCopied] = useState(false)
  return (
    <details className={styles.details}>
      <summary>
        <ChevronRightIcon className={styles.disclosure} />
        Details
      </summary>
      <pre className={styles.detailsText}>{text}</pre>
      <Button
        className={styles.copy}
        onClick={() => {
          navigator.clipboard?.writeText(text).then(
            () => setCopied(true),
            () => setCopied(false),
          )
        }}
      >
        {copied ? 'Copied' : 'Copy details'}
      </Button>
    </details>
  )
}
