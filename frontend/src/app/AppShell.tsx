// Top bar over a full-bleed canvas. No sidebar: orientation comes from the
// breadcrumbs, and later from semantic zoom and contextual UI.

import { useState } from 'react'

import type { TelescodeApi } from '../bridge'
import { TopBar } from '../components/TopBar'
import { WorkspaceCanvas } from '../components/WorkspaceCanvas'
import type { Crumb } from '../ui/Breadcrumbs'
import type { DropdownItem } from '../ui/Dropdown'
import styles from './AppShell.module.css'
import { baseName, useWorkspace, type WorkspaceState } from './useWorkspace'

interface AppShellProps {
  api?: TelescodeApi
  initialState?: WorkspaceState
  /** Levels below the database root. Semantic zoom will drive these later. */
  initialPath?: Crumb[]
}

export function AppShell({ api, initialState, initialPath = [] }: AppShellProps) {
  const workspace = useWorkspace(api, initialState)
  const { state } = workspace
  const [path, setPath] = useState<Crumb[]>(initialPath)
  const [lastPath, setLastPath] = useState(state.status === 'empty' ? '' : state.dbPath)

  const open = (dbPath: string) => {
    setLastPath(dbPath)
    setPath([])
    workspace.open(dbPath)
  }

  const close = () => {
    setPath([])
    workspace.close()
  }

  const crumbs: Crumb[] =
    state.status === 'empty'
      ? [{ id: 'home', label: 'Telescode' }]
      : [{ id: 'root', label: baseName(state.dbPath), title: state.dbPath }, ...path]

  const menuItems: DropdownItem[] = [
    { id: 'open', label: 'Open database…', onSelect: close },
    {
      id: 'reload',
      label: 'Reload analysis',
      onSelect: workspace.reload,
      disabled: state.status === 'empty' || state.status === 'loading',
    },
  ]

  return (
    <div className={styles.shell}>
      <TopBar crumbs={crumbs} onNavigate={(_, index) => setPath(path.slice(0, index))} menuItems={menuItems} />
      <WorkspaceCanvas state={state} onOpen={open} onRetry={workspace.reload} onClose={close} lastPath={lastPath} />
    </div>
  )
}
