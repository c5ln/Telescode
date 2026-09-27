// Path entry for a database produced by `TelescodeHeadless scan`.

import { useState, type FormEvent } from 'react'

import { Button } from '../ui/Button'
import styles from './OpenDatabaseForm.module.css'

interface OpenDatabaseFormProps {
  initialPath?: string
  onOpen: (dbPath: string) => void
}

export function OpenDatabaseForm({ initialPath = '', onOpen }: OpenDatabaseFormProps) {
  const [dbPath, setDbPath] = useState(initialPath)
  const trimmed = dbPath.trim()

  const submit = (e: FormEvent) => {
    e.preventDefault()
    if (trimmed) onOpen(trimmed)
  }

  return (
    <form className={styles.form} onSubmit={submit}>
      <input
        className={styles.input}
        value={dbPath}
        onChange={(e) => setDbPath(e.target.value)}
        placeholder="C:\path\to\telescode.db"
        aria-label="Database path"
        spellCheck={false}
        autoComplete="off"
        autoFocus
      />
      <Button type="submit" variant="primary" disabled={!trimmed}>
        Open
      </Button>
    </form>
  )
}
