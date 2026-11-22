// Repository URL entry: the product's front door.
//
// URL → download and scan → internal database → analysis. The form reads the
// common ways people write a repository and says how to fix anything else;
// everything after that happens once it is submitted.

import { useState, type FormEvent } from 'react'

import { Button } from '../ui/Button'
import styles from './InlineForm.module.css'
import { readRepositoryInput } from './repositoryUrl'

interface RepositoryUrlFormProps {
  onSubmit: (url: string) => void
  /** URL to prefill, e.g. the repository opened last. */
  initialUrl?: string
}

export function RepositoryUrlForm({ onSubmit, initialUrl = '' }: RepositoryUrlFormProps) {
  const [url, setUrl] = useState(initialUrl)
  const [problem, setProblem] = useState<string | null>(null)
  const trimmed = url.trim()

  const submit = (e: FormEvent) => {
    e.preventDefault()
    const input = readRepositoryInput(trimmed)
    if ('url' in input) onSubmit(input.url)
    else setProblem(input.problem)
  }

  return (
    <div className={styles.stack}>
      <form className={styles.form} data-size="large" onSubmit={submit}>
        <input
          className={styles.input}
          value={url}
          onChange={(e) => {
            setUrl(e.target.value)
            setProblem(null)
          }}
          placeholder="Paste a public GitHub URL of a Python project"
          aria-label="Repository URL"
          aria-invalid={problem ? true : undefined}
          aria-describedby={problem ? 'repository-url-notice' : undefined}
          spellCheck={false}
          autoComplete="off"
          autoFocus
        />
        <Button type="submit" variant="primary" disabled={!trimmed}>
          Analyze
        </Button>
      </form>
      {problem && (
        <p id="repository-url-notice" className={styles.notice} data-kind="invalid" role="status">
          {problem}
        </p>
      )}
    </div>
  )
}
