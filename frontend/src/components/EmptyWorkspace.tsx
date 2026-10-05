// What the canvas shows before anything is open: a repository URL field.
//
// Users never deal with Telescode's internal database. In development builds
// a quiet link switches to entering the path of a repository folder on this
// computer, which is scanned the same way as a downloaded repository.

import { useState } from 'react'

import type { RepositorySource } from '../bridge'
import { EmptyState } from '../ui/EmptyState'
import styles from './InlineForm.module.css'
import { OpenFolderForm } from './OpenFolderForm'
import { RepositoryUrlForm } from './RepositoryUrlForm'

interface EmptyWorkspaceProps {
  /** Offer the development-only folder entry. */
  allowLocalFolder: boolean
  onOpen: (source: RepositorySource) => void
  /** Last repository opened, to prefill; a folder also starts in folder mode. */
  lastSource?: RepositorySource
}

export function EmptyWorkspace({ allowLocalFolder, onOpen, lastSource }: EmptyWorkspaceProps) {
  const [mode, setMode] = useState<'repository' | 'folder'>(
    allowLocalFolder && lastSource?.kind === 'folder' ? 'folder' : 'repository',
  )

  if (mode === 'folder' && allowLocalFolder) {
    return (
      <EmptyState title="Open a local folder" description="Development only: a repository folder on this computer.">
        <div className={styles.stack}>
          <OpenFolderForm
            initialPath={lastSource?.kind === 'folder' ? lastSource.path : undefined}
            onOpen={(path) => onOpen({ kind: 'folder', path })}
          />
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
        <RepositoryUrlForm
          initialUrl={lastSource?.kind === 'github' ? lastSource.url : undefined}
          onSubmit={(url) => onOpen({ kind: 'github', url })}
        />
        {allowLocalFolder && (
          <button type="button" className={styles.link} onClick={() => setMode('folder')}>
            Open a local folder (dev)
          </button>
        )}
      </div>
    </EmptyState>
  )
}
