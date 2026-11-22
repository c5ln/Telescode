// Persistent chrome: breadcrumbs | search | Connect MCP | code tour | overflow.

import { useState } from 'react'

import { agents, agentStatus, type AgentProvider, type AgentStatus } from '../agent'
import type { SearchResult } from '../search/search'
import { Breadcrumbs, type Crumb } from '../ui/Breadcrumbs'
import { Dropdown, type DropdownItem } from '../ui/Dropdown'
import { ChevronDownIcon, MoreIcon, PlayIcon } from '../ui/icons'
import { IconButton } from '../ui/IconButton'
import { SearchField } from '../ui/SearchField'
import { Tooltip } from '../ui/Tooltip'
import styles from './TopBar.module.css'

interface TopBarProps {
  agent?: {
    provider: AgentProvider
    disabled: boolean
    onChange: (provider: AgentProvider) => void
    /** Whether each agent is installed and signed in; looked up when the menu opens. */
    status?: (provider: AgentProvider) => Promise<AgentStatus>
  }
  crumbs: readonly Crumb[]
  onNavigate?: (crumb: Crumb, index: number) => void
  menuItems: DropdownItem[]
  search?: {
    value: string
    onChange: (value: string) => void
    results: SearchResult[]
    onSelect: (result: SearchResult) => void
    disabled: boolean
    emptyMessage?: string
    footer?: string
  }
  /** The entry point to tour mode: plays the tour, or starts making one. */
  tutorial: { active: boolean; disabled: boolean; onToggle: () => void }
}

const PROVIDERS = ['claude', 'codex'] as const

/** What the menu says about an agent: nothing until its status is known. */
function statusHint(status: AgentStatus | undefined) {
  if (!status) return undefined
  if (!status.installed) return 'Not installed'
  if (!status.signedIn) return 'Sign-in needed'
  return (
    <>
      <span className={styles.signedIn} aria-hidden="true" />
      Signed in
    </>
  )
}

export function TopBar({ crumbs, onNavigate, menuItems, search, tutorial, agent }: TopBarProps) {
  const [statuses, setStatuses] = useState<Partial<Record<AgentProvider, AgentStatus>>>({})
  const lookUp = agent?.status ?? agentStatus

  const refreshStatuses = () => {
    for (const provider of PROVIDERS) {
      lookUp(provider).then(
        (status) => setStatuses((s) => ({ ...s, [provider]: status })),
        // Outside the desktop app there is nothing to look up; show no status.
        () => {},
      )
    }
  }

  return (
    <header className={styles.bar}>
      <div className={styles.crumbs}>
        <Breadcrumbs items={crumbs} onNavigate={onNavigate} />
      </div>
      <div className={styles.search}>
        <SearchField
          placeholder="Search files, classes, functions…"
          label="Search"
          value={search?.value}
          onChange={search?.onChange}
          results={search?.results}
          onSelect={search?.onSelect}
          disabled={search?.disabled}
          emptyMessage={search?.emptyMessage}
          footer={search?.footer}
        />
      </div>
      {agent && (
        <Dropdown
          align="end"
          label="Connect MCP"
          header="Choose the AI that writes your code tours and quizzes. Telescode connects it to its MCP server."
          footer="Uses the sign-in you already have. No API key needed."
          onOpenChange={(open) => open && refreshStatuses()}
          items={PROVIDERS.map((provider) => ({
            type: 'radio',
            id: provider,
            label: agents[provider].name,
            hint: statusHint(statuses[provider]),
            checked: agent.provider === provider,
            disabled: agent.disabled,
            onSelect: () => agent.onChange(provider),
          }))}
          trigger={(props) => (
            <Tooltip
              label="Connect MCP"
              description="Picks the AI that writes your tours and quizzes: Claude Code or Codex. It reads the analysis through Telescode's MCP server."
              align="end"
            >
              <button {...props} type="button" className={styles.agent} disabled={agent.disabled}>
                <span>Connect MCP</span>
                <ChevronDownIcon className={styles.chevron} />
              </button>
            </Tooltip>
          )}
        />
      )}
      <Tooltip
        label={tutorial.active ? 'Exit the code tour' : 'Start a code tour'}
        shortcut="T"
        description={
          tutorial.active
            ? 'Closes the tour and its quiz. You can pick it up again from Start tour.'
            : 'Walks you through this repository one file at a time, in the order worth reading. A ranking model picks the order, and your AI agent explains each step.'
        }
        align="end"
      >
        <button
          type="button"
          className={styles.tour}
          aria-pressed={tutorial.active}
          disabled={tutorial.disabled}
          onClick={tutorial.onToggle}
        >
          <PlayIcon className={styles.tourIcon} />
          {tutorial.active ? 'Exit tour' : 'Start tour'}
        </button>
      </Tooltip>
      <Dropdown
        align="end"
        items={menuItems}
        trigger={(props) => (
          <IconButton {...props} label="More actions" tooltipAlign="end">
            <MoreIcon />
          </IconButton>
        )}
      />
    </header>
  )
}
