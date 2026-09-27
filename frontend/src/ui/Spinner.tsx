// A small indeterminate ring. Decorative by default: pair it with visible text.
// Pass `label` when it stands alone.

import styles from './Spinner.module.css'

interface SpinnerProps {
  size?: number
  label?: string
}

export function Spinner({ size = 14, label }: SpinnerProps) {
  return (
    <span
      className={styles.spinner}
      style={{ width: size, height: size }}
      role={label ? 'status' : undefined}
      aria-label={label}
      aria-hidden={label ? undefined : true}
    />
  )
}
