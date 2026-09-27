// Repository URL entry: the product's front door.
//
// The intended flow is URL → clone and scan → internal database → analysis.
// Cloning and scanning do not exist yet, so without `onSubmit` the form only
// checks the URL and says the step is not available.

import { useState, type FormEvent } from 'react'

import { Button } from '../ui/Button'
import styles from './InlineForm.module.css'
import { isRepositoryUrl } from './repositoryUrl'

interface RepositoryUrlFormProps {
  onSubmit?: (url: string) => void
}

type Notice = { kind: 'invalid' | 'unavailable'; text: string } | null

export function RepositoryUrlForm({ onSubmit }: RepositoryUrlFormProps) {
  const [url, setUrl] = useState('')
  const [notice, setNotice] = useState<Notice>(null)
  const trimmed = url.trim()

  const submit = (e: FormEvent) => {
    e.preventDefault()
    if (!isRepositoryUrl(trimmed)) {
      setNotice({ kind: 'invalid', text: 'Enter a Git repository URL, such as https://github.com/owner/repo.' })
    } else if (onSubmit) {
      onSubmit(trimmed)
    } else {
      setNotice({ kind: 'unavailable', text: 'Repository scanning is not available yet.' })
    }
  }

  return (
    <div className={styles.stack}>
      <form className={styles.form} onSubmit={submit}>
        <input
          className={styles.input}
          value={url}
          onChange={(e) => {
            setUrl(e.target.value)
            setNotice(null)
          }}
          placeholder="https://github.com/owner/repo"
          aria-label="Repository URL"
          aria-invalid={notice?.kind === 'invalid' || undefined}
          aria-describedby={notice ? 'repository-url-notice' : undefined}
          spellCheck={false}
          autoComplete="off"
          autoFocus
        />
        <Button type="submit" variant="primary" disabled={!trimmed}>
          Analyze
        </Button>
      </form>
      {notice && (
        <p id="repository-url-notice" className={styles.notice} data-kind={notice.kind} role="status">
          {notice.text}
        </p>
      )}
    </div>
  )
}
