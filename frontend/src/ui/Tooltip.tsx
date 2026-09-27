// A small label shown on hover or keyboard focus after a short delay.
//
// Pure CSS positioning relative to the trigger: enough for the shell's static
// controls. It is visual only -- the trigger must carry its own accessible
// name (IconButton requires one), so the tip is hidden from assistive tech.

import type { ReactNode } from 'react'

import styles from './Tooltip.module.css'

export type TooltipSide = 'top' | 'bottom' | 'left' | 'right'

interface TooltipProps {
  label: ReactNode
  /** Optional shortcut shown after the label, e.g. "Ctrl K". */
  shortcut?: string
  side?: TooltipSide
  /** For top/bottom tips: line up with the trigger's centre or end edge. */
  align?: 'center' | 'end'
  children: ReactNode
}

export function Tooltip({ label, shortcut, side = 'bottom', align = 'center', children }: TooltipProps) {
  return (
    <span className={styles.anchor}>
      {children}
      <span className={styles.tip} data-side={side} data-align={align} aria-hidden="true">
        {label}
        {shortcut && <kbd className={styles.shortcut}>{shortcut}</kbd>}
      </span>
    </span>
  )
}
