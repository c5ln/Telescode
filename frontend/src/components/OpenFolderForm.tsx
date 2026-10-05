// Path entry for a repository folder on this computer.

import { useState, type FormEvent } from 'react'

import { Button } from '../ui/Button'
import styles from './InlineForm.module.css'

interface OpenFolderFormProps {
  initialPath?: string
  onOpen: (path: string) => void
}

export function OpenFolderForm({ initialPath = '', onOpen }: OpenFolderFormProps) {
  const [path, setPath] = useState(initialPath)
  const trimmed = path.trim()

  const submit = (e: FormEvent) => {
    e.preventDefault()
    if (trimmed) onOpen(trimmed)
  }

  return (
    <form className={styles.form} onSubmit={submit}>
      <input
        className={styles.input}
        value={path}
        onChange={(e) => setPath(e.target.value)}
        placeholder="/path/to/repository"
        aria-label="Folder path"
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
