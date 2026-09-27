// A quiet, centred message for a view with nothing to show yet: a short title,
// an optional line of explanation, and optional actions below.

import type { ReactNode } from 'react'

import styles from './States.module.css'

interface EmptyStateProps {
  title: string
  description?: ReactNode
  children?: ReactNode
}

export function EmptyState({ title, description, children }: EmptyStateProps) {
  return (
    <div className={styles.state}>
      <p className={styles.title}>{title}</p>
      {description && <p className={styles.description}>{description}</p>}
      {children && <div className={styles.actions}>{children}</div>}
    </div>
  )
}
