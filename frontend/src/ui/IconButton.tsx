// A square, borderless button holding a single icon. `label` is required: it
// is both the accessible name and the tooltip text.

import type { ButtonHTMLAttributes, ReactNode, Ref } from 'react'

import styles from './IconButton.module.css'
import { Tooltip, type TooltipSide } from './Tooltip'

interface IconButtonProps extends Omit<ButtonHTMLAttributes<HTMLButtonElement>, 'aria-label' | 'children'> {
  label: string
  children: ReactNode
  /** Tooltip placement, or `false` to show none. */
  tooltip?: TooltipSide | false
  tooltipAlign?: 'center' | 'start' | 'end'
  shortcut?: string
  /** Turns the tooltip into a rich tip: how to use the control. */
  description?: ReactNode
  ref?: Ref<HTMLButtonElement>
}

export function IconButton({
  label,
  children,
  tooltip = 'bottom',
  tooltipAlign,
  shortcut,
  description,
  className,
  type = 'button',
  ...rest
}: IconButtonProps) {
  const button = (
    <button {...rest} type={type} aria-label={label} className={[styles.button, className].filter(Boolean).join(' ')}>
      {children}
    </button>
  )
  if (tooltip === false) return button
  return (
    <Tooltip label={label} shortcut={shortcut} description={description} side={tooltip} align={tooltipAlign}>
      {button}
    </Tooltip>
  )
}
