// A concise, centred failure message: what went wrong in one line, the
// machine-readable code for reference, and optional recovery actions.

import type { ReactNode } from 'react'

import styles from './States.module.css'

interface ErrorStateProps {
  title: string
  message: string
  /** Stable identifier, e.g. a TelescodeError code. */
  code?: string
  children?: ReactNode
}

export function ErrorState({ title, message, code, children }: ErrorStateProps) {
  return (
    <div className={styles.state} role="alert">
      <p className={styles.title}>
        <span className={styles.errorMark} aria-hidden="true" />
        {title}
      </p>
      <p className={styles.description}>{message}</p>
      {code && <code className={styles.code}>{code}</code>}
      {children && <div className={styles.actions}>{children}</div>}
    </div>
  )
}
