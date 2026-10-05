// A menu button: a trigger plus a small floating list of actions.
//
// Follows the WAI-ARIA menu button pattern: Enter/Space/ArrowDown open it and
// focus the first item, arrows/Home/End move between enabled items, Escape
// closes and returns focus to the trigger, Tab or an outside click closes.
// A toggle item is a menuitemcheckbox drawn as a switch; it flips in place and
// leaves the menu open so the change is visible.

import { useCallback, useEffect, useId, useRef, useState, type KeyboardEvent, type ReactNode, type Ref } from 'react'

import styles from './Dropdown.module.css'

export type DropdownItem =
  | {
      type?: 'item'
      id: string
      label: ReactNode
      onSelect: () => void
      disabled?: boolean
      /** Muted text on the right, e.g. a shortcut. */
      hint?: string
      /** Full text shown on hover when the label is truncated. */
      title?: string
    }
  | {
      type: 'toggle'
      id: string
      label: ReactNode
      checked: boolean
      onChange: (checked: boolean) => void
      disabled?: boolean
    }
  | { type: 'separator'; id: string }

/** Props the trigger must spread onto its button. */
export interface DropdownTriggerProps {
  ref: Ref<HTMLButtonElement>
  id: string
  onClick: () => void
  onKeyDown: (e: KeyboardEvent<HTMLButtonElement>) => void
  'aria-haspopup': 'menu'
  'aria-expanded': boolean
  'aria-controls': string | undefined
}

interface DropdownProps {
  trigger: (props: DropdownTriggerProps) => ReactNode
  items: DropdownItem[]
  /** Which edge of the trigger the menu lines up with. */
  align?: 'start' | 'end'
  /** Accessible name for the menu; defaults to the trigger's name. */
  label?: string
}

export function Dropdown({ trigger, items, align = 'start', label }: DropdownProps) {
  const [open, setOpen] = useState(false)
  const rootRef = useRef<HTMLDivElement>(null)
  const triggerRef = useRef<HTMLButtonElement>(null)
  const menuRef = useRef<HTMLDivElement>(null)
  const triggerId = useId()
  const menuId = useId()

  const enabledItems = () =>
    Array.from(menuRef.current?.querySelectorAll<HTMLButtonElement>('[role^="menuitem"]:not(:disabled)') ?? [])

  const close = useCallback((returnFocus: boolean) => {
    setOpen(false)
    if (returnFocus) triggerRef.current?.focus()
  }, [])

  // Focus the first item on open.
  useEffect(() => {
    if (open) enabledItems()[0]?.focus()
  }, [open])

  // Close on a press outside.
  useEffect(() => {
    if (!open) return
    const onPointerDown = (e: PointerEvent) => {
      if (!rootRef.current?.contains(e.target as Node)) close(false)
    }
    document.addEventListener('pointerdown', onPointerDown)
    return () => document.removeEventListener('pointerdown', onPointerDown)
  }, [open, close])

  const onTriggerKeyDown = (e: KeyboardEvent<HTMLButtonElement>) => {
    if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
      e.preventDefault()
      setOpen(true)
    }
  }

  const onMenuKeyDown = (e: KeyboardEvent<HTMLDivElement>) => {
    const list = enabledItems()
    const index = list.indexOf(document.activeElement as HTMLButtonElement)
    const move = (to: number) => {
      e.preventDefault()
      list[(to + list.length) % list.length]?.focus()
    }
    switch (e.key) {
      case 'ArrowDown':
        return move(index + 1)
      case 'ArrowUp':
        return move(index - 1)
      case 'Home':
        return move(0)
      case 'End':
        return move(list.length - 1)
      case 'Escape':
        e.preventDefault()
        e.stopPropagation()
        return close(true)
      case 'Tab':
        return close(false)
    }
  }

  return (
    <div ref={rootRef} className={styles.root}>
      {trigger({
        ref: triggerRef,
        id: triggerId,
        onClick: () => setOpen((o) => !o),
        onKeyDown: onTriggerKeyDown,
        'aria-haspopup': 'menu',
        'aria-expanded': open,
        'aria-controls': open ? menuId : undefined,
      })}
      {open && (
        <div
          ref={menuRef}
          id={menuId}
          role="menu"
          aria-label={label}
          aria-labelledby={label ? undefined : triggerId}
          className={styles.menu}
          data-align={align}
          onKeyDown={onMenuKeyDown}
        >
          {items.map((item) =>
            item.type === 'separator' ? (
              <div key={item.id} role="separator" className={styles.separator} />
            ) : item.type === 'toggle' ? (
              <button
                key={item.id}
                type="button"
                role="menuitemcheckbox"
                aria-checked={item.checked}
                tabIndex={-1}
                disabled={item.disabled}
                className={styles.item}
                onClick={() => item.onChange(!item.checked)}
              >
                <span className={styles.label}>{item.label}</span>
                <span className={styles.switch} aria-hidden="true" />
              </button>
            ) : (
              <button
                key={item.id}
                type="button"
                role="menuitem"
                tabIndex={-1}
                disabled={item.disabled}
                title={item.title}
                className={styles.item}
                onClick={() => {
                  close(true)
                  item.onSelect()
                }}
              >
                <span className={styles.label}>{item.label}</span>
                {item.hint && <span className={styles.hint}>{item.hint}</span>}
              </button>
            ),
          )}
        </div>
      )}
    </div>
  )
}
