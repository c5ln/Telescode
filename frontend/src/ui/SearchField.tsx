// Compact search input with a global focus shortcut (Ctrl K, or ⌘K on macOS).
//
// Only the field lives here; what a query does is up to the caller. Escape
// clears a non-empty query first, then leaves the field.

import { useEffect, useRef, useState, type KeyboardEvent } from 'react'

import { SearchIcon } from './icons'
import styles from './SearchField.module.css'

interface SearchFieldProps {
  placeholder?: string
  /** Accessible name; defaults to the placeholder. */
  label?: string
  value?: string
  onChange?: (value: string) => void
  /** Register the Ctrl/⌘ K shortcut. */
  shortcut?: boolean
}

const IS_MAC = typeof navigator !== 'undefined' && /Mac|iPhone|iPad/.test(navigator.platform || navigator.userAgent)

const SEARCH_SHORTCUT_LABEL = IS_MAC ? '⌘K' : 'Ctrl K'

export function SearchField({ placeholder = 'Search', label, value, onChange, shortcut = true }: SearchFieldProps) {
  const inputRef = useRef<HTMLInputElement>(null)
  const [draft, setDraft] = useState('')
  const query = value ?? draft

  const setQuery = (next: string) => {
    if (value === undefined) setDraft(next)
    onChange?.(next)
  }

  useEffect(() => {
    if (!shortcut) return
    const onKeyDown = (e: globalThis.KeyboardEvent) => {
      const mod = IS_MAC ? e.metaKey : e.ctrlKey
      if (mod && !e.altKey && !e.shiftKey && e.key.toLowerCase() === 'k') {
        e.preventDefault()
        inputRef.current?.focus()
        inputRef.current?.select()
      }
    }
    window.addEventListener('keydown', onKeyDown)
    return () => window.removeEventListener('keydown', onKeyDown)
  }, [shortcut])

  const onKeyDown = (e: KeyboardEvent<HTMLInputElement>) => {
    if (e.key !== 'Escape') return
    if (query) setQuery('')
    else inputRef.current?.blur()
  }

  return (
    <div className={styles.field} role="search">
      <SearchIcon className={styles.icon} />
      <input
        ref={inputRef}
        className={styles.input}
        type="text"
        value={query}
        onChange={(e) => setQuery(e.target.value)}
        onKeyDown={onKeyDown}
        placeholder={placeholder}
        aria-label={label ?? placeholder}
        aria-keyshortcuts={shortcut ? (IS_MAC ? 'Meta+K' : 'Control+K') : undefined}
        spellCheck={false}
        autoComplete="off"
      />
      {shortcut && !query && (
        <kbd className={styles.hint} aria-hidden="true">
          {SEARCH_SHORTCUT_LABEL}
        </kbd>
      )}
    </div>
  )
}
