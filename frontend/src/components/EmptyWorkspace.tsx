// What the canvas shows before anything is open: a repository URL field.
//
// Users never deal with Telescode's internal database. In development builds
// a quiet link switches to entering a database path directly, for opening a
// database made with TelescodeHeadless scan without going through GitHub.

import { useState } from 'react'

import { EmptyState } from '../ui/EmptyState'
import styles from './InlineForm.module.css'
import { OpenDatabaseForm } from './OpenDatabaseForm'
import { RepositoryUrlForm } from './RepositoryUrlForm'

interface EmptyWorkspaceProps {
  /** Offer the development-only database path entry. */
  allowLocalDatabase: boolean
  onOpenDatabase: (dbPath: string) => void
  onOpenRepository: (url: string) => void
  /** Last database path, to prefill; also starts in database mode. */
  lastDbPath?: string
  /** Last repository URL, to prefill. */
  lastUrl?: string
}

export function EmptyWorkspace({
  allowLocalDatabase,
  onOpenDatabase,
  onOpenRepository,
  lastDbPath,
  lastUrl,
}: EmptyWorkspaceProps) {
  const [mode, setMode] = useState<'repository' | 'database'>(
    allowLocalDatabase && lastDbPath ? 'database' : 'repository',
  )

  if (mode === 'database' && allowLocalDatabase) {
    return (
      <EmptyState title="Open a local database" description="Development only: a database created with TelescodeHeadless scan.">
        <div className={styles.stack}>
          <OpenDatabaseForm initialPath={lastDbPath} onOpen={onOpenDatabase} />
          <button type="button" className={styles.link} onClick={() => setMode('repository')}>
            Use a repository URL
          </button>
        </div>
      </EmptyState>
    )
  }

  return (
    <EmptyState title="Open a repository" description="Paste a public GitHub repository URL to map its code.">
      <div className={styles.stack}>
        <RepositoryUrlForm onSubmit={onOpenRepository} initialUrl={lastUrl} />
        {allowLocalDatabase && (
          <button type="button" className={styles.link} onClick={() => setMode('database')}>
            Open a local database (dev)
          </button>
        )}
      </div>
    </EmptyState>
  )
}
