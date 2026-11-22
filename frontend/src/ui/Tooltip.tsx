// A tip shown on hover or keyboard focus after a short delay.
//
// A label tip names its control. Given a `description`, it becomes a rich tip:
// the label as a title, then a line or two on how to use the control or how
// the feature works. Either way it is only seen on hover or focus, so it holds
// no links or buttons.
//
// Pure CSS positioning relative to the trigger: enough for the shell's static
// controls. The trigger must carry its own accessible name (IconButton
// requires one), so the label is hidden from assistive tech; a rich tip's
// description becomes the trigger's accessible description.

import { cloneElement, isValidElement, useId, type ReactElement, type ReactNode } from 'react'

import styles from './Tooltip.module.css'

export type TooltipSide = 'top' | 'bottom' | 'left' | 'right'

interface TooltipProps {
  label: ReactNode
  /** Optional shortcut shown after the label, e.g. "Ctrl K". */
  shortcut?: string
  /** How to use the control or how it works, below the label. */
  description?: ReactNode
  side?: TooltipSide
  /** For top/bottom tips: line up with the trigger's centre, start or end edge. */
  align?: 'center' | 'start' | 'end'
  children: ReactNode
}

export function Tooltip({ label, shortcut, description, side = 'bottom', align = 'center', children }: TooltipProps) {
  const kbd = shortcut && <kbd className={styles.shortcut}>{shortcut}</kbd>
  const descriptionId = useId()
  const trigger = description && isValidElement<{ 'aria-describedby'?: string }>(children)
    ? cloneElement(children as ReactElement<{ 'aria-describedby'?: string }>, {
        'aria-describedby': [children.props['aria-describedby'], descriptionId].filter(Boolean).join(' '),
      })
    : children
  return (
    <span className={styles.anchor}>
      {trigger}
      {description ? (
        <span className={`${styles.tip} ${styles.rich}`} data-side={side} data-align={align}>
          <span className={styles.title} aria-hidden="true">
            {label}
            {kbd}
          </span>
          <span className={styles.description} id={descriptionId}>{description}</span>
        </span>
      ) : (
        <span className={styles.tip} data-side={side} data-align={align} aria-hidden="true">
          {label}
          {kbd}
        </span>
      )}
    </span>
  )
}
