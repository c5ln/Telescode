// A compact text button. `primary` is solid ink for the one main action in a
// view; `secondary` is a quiet outline for everything else.

import type { ButtonHTMLAttributes } from 'react'

import styles from './Button.module.css'

interface ButtonProps extends ButtonHTMLAttributes<HTMLButtonElement> {
  variant?: 'primary' | 'secondary'
}

export function Button({ variant = 'secondary', className, type = 'button', ...rest }: ButtonProps) {
  return (
    <button
      {...rest}
      type={type}
      data-variant={variant}
      className={[styles.button, className].filter(Boolean).join(' ')}
    />
  )
}
