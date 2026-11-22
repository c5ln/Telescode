// Compact search input with a global focus shortcut (Ctrl K, or ⌘K on macOS)
// and an optional combobox results dropdown.
//
// When `results` is omitted, behaves as a plain text field: Escape clears a
// non-empty query first, then leaves the field. When `results` is provided,
// Escape closes the open dropdown first, then clears the query, then blurs.

import { useEffect, useId, useRef, useState, type KeyboardEvent } from 'react'

import type { ResultKind, SearchResult } from '../search/names'
import { ClassIcon, FieldIcon, FileIcon, MethodIcon, SearchIcon } from './icons'
import styles from './SearchField.module.css'

export interface SearchFieldProps {
  placeholder?: string
  /** Accessible name; defaults to the placeholder. */
  label?: string
  value?: string
  onChange?: (value: string) => void
  /** Register the Ctrl/⌘ K shortcut. */
  shortcut?: boolean
  disabled?: boolean
  /** When provided, renders a combobox listbox below the input. */
  results?: SearchResult[]
  onSelect?: (result: SearchResult) => void
  /** Shown when the query is non-empty and `results` is empty. */
  emptyMessage?: string
  /** Optional single-line note rendered at the bottom of the dropdown. */
  footer?: string
}

const IS_MAC = typeof navigator !== 'undefined' && /Mac|iPhone|iPad/.test(navigator.platform || navigator.userAgent)

const SEARCH_SHORTCUT_LABEL = IS_MAC ? '⌘K' : 'Ctrl K'

function KindIcon({ kind, className }: { kind: ResultKind; className?: string }) {
  switch (kind) {
    case 'file':
      return <FileIcon className={className} />
    case 'class':
      return <ClassIcon className={className} />
    case 'method':
      return <MethodIcon className={className} />
    case 'field':
      return <FieldIcon className={className} />
  }
}

export function SearchField({
  placeholder = 'Search',
  label,
  value,
  onChange,
  shortcut = true,
  disabled = false,
  results,
  onSelect,
  emptyMessage,
  footer,
}: SearchFieldProps) {
  const inputRef = useRef<HTMLInputElement>(null)
  const listRef = useRef<HTMLUListElement>(null)
  const [draft, setDraft] = useState('')
  const [open, setOpen] = useState(false)
  const [rawActiveIndex, setRawActiveIndex] = useState(0)
  const listboxId = useId()

  const query = value ?? draft
  const hasCombobox = results !== undefined
  const hasQuery = query.trim().length > 0
  const resultCount = results?.length ?? 0
  const hasPopoverContent = resultCount > 0 || Boolean(emptyMessage) || Boolean(footer)
  const isOpen = hasCombobox && !disabled && open && hasQuery && hasPopoverContent
  const activeIndex = isOpen && resultCount > 0 ? Math.min(rawActiveIndex, resultCount - 1) : -1
  const optionId = (index: number) => `${listboxId}-option-${index}`

  const setQuery = (next: string) => {
    if (value === undefined) setDraft(next)
    onChange?.(next)
    if (hasCombobox) {
      setRawActiveIndex(0)
      setOpen(next.trim().length > 0)
    }
  }

  const selectResult = (result: SearchResult) => {
    setOpen(false)
    onSelect?.(result)
  }

  useEffect(() => {
    if (!shortcut || disabled) return
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
  }, [shortcut, disabled])

  useEffect(() => {
    if (!isOpen || activeIndex < 0) return
    const item = listRef.current?.children[activeIndex] as HTMLElement | undefined
    if (item && typeof item.scrollIntoView === 'function') {
      item.scrollIntoView({ block: 'nearest' })
    }
  }, [isOpen, activeIndex])

  const onKeyDown = (e: KeyboardEvent<HTMLInputElement>) => {
    if (hasCombobox && !disabled) {
      if (e.key === 'ArrowDown') {
        e.preventDefault()
        if (!isOpen) {
          if (hasQuery && hasPopoverContent) setOpen(true)
        } else if (resultCount > 0) {
          setRawActiveIndex((prev) => Math.min(prev + 1, resultCount - 1))
        }
        return
      }
      if (e.key === 'ArrowUp') {
        e.preventDefault()
        if (isOpen && resultCount > 0) {
          setRawActiveIndex((prev) => Math.max(prev - 1, 0))
        }
        return
      }
      if (e.key === 'Enter') {
        if (isOpen && activeIndex >= 0 && results?.[activeIndex]) {
          e.preventDefault()
          selectResult(results[activeIndex])
        }
        return
      }
    }

    if (e.key !== 'Escape') return
    if (isOpen) {
      e.preventDefault()
      setOpen(false)
    } else if (query) {
      setQuery('')
    } else {
      inputRef.current?.blur()
    }
  }

  return (
    <div className={styles.root} role="search">
      <div className={styles.field} data-disabled={disabled ? 'true' : undefined}>
        <SearchIcon className={styles.icon} />
        <input
          ref={inputRef}
          className={styles.input}
          type="text"
          role={hasCombobox ? 'combobox' : undefined}
          aria-autocomplete={hasCombobox ? 'list' : undefined}
          aria-expanded={hasCombobox ? isOpen : undefined}
          aria-controls={hasCombobox && isOpen ? listboxId : undefined}
          aria-activedescendant={hasCombobox && activeIndex >= 0 ? optionId(activeIndex) : undefined}
          value={query}
          disabled={disabled}
          onChange={(e) => setQuery(e.target.value)}
          onFocus={() => {
            if (hasCombobox && !disabled && hasQuery) setOpen(true)
          }}
          onClick={() => {
            if (hasCombobox && !disabled && hasQuery) setOpen(true)
          }}
          onBlur={() => {
            if (hasCombobox) setOpen(false)
          }}
          onKeyDown={onKeyDown}
          placeholder={placeholder}
          aria-label={label ?? placeholder}
          aria-keyshortcuts={shortcut && !disabled ? (IS_MAC ? 'Meta+K' : 'Control+K') : undefined}
          spellCheck={false}
          autoComplete="off"
        />
        {shortcut && !query && (
          <kbd className={styles.hint} aria-hidden="true">
            {SEARCH_SHORTCUT_LABEL}
          </kbd>
        )}
      </div>

      {isOpen && (
        <div className={styles.popover}>
          <ul ref={listRef} id={listboxId} role="listbox" className={styles.list}>
            {resultCount > 0 ? (
              results!.map((result, index) => (
                <li
                  key={`${result.source}:${result.id}`}
                  id={optionId(index)}
                  role="option"
                  aria-selected={index === activeIndex}
                  className={styles.row}
                  onMouseDown={(e) => e.preventDefault()}
                  onMouseEnter={() => setRawActiveIndex(index)}
                  onClick={() => selectResult(result)}
                >
                  <KindIcon kind={result.kind} className={styles.rowIcon} />
                  <span className={styles.rowText}>
                    <span className={styles.rowTop}>
                      <span className={styles.rowLabel}>{result.label}</span>
                      {result.source === 'content' && <span className={styles.badge}>내용 일치</span>}
                    </span>
                    <span className={styles.rowSubtitle}>{result.subtitle}</span>
                  </span>
                </li>
              ))
            ) : (
              emptyMessage && <li className={styles.empty}>{emptyMessage}</li>
            )}
          </ul>
          {footer && <div className={styles.footer}>{footer}</div>}
        </div>
      )}
    </div>
  )
}
