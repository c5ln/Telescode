// Persistent chrome: breadcrumbs | search | AI agent | tutorial | overflow.

import { agents, type AgentProvider } from '../agent'
import { Breadcrumbs, type Crumb } from '../ui/Breadcrumbs'
import { Dropdown, type DropdownItem } from '../ui/Dropdown'
import { ChevronDownIcon, MoreIcon, PlayIcon } from '../ui/icons'
import { IconButton } from '../ui/IconButton'
import { SearchField } from '../ui/SearchField'
import styles from './TopBar.module.css'

interface TopBarProps {
  agent?: { provider: AgentProvider; disabled: boolean; onChange: (provider: AgentProvider) => void }
  crumbs: readonly Crumb[]
  onNavigate?: (crumb: Crumb, index: number) => void
  menuItems: DropdownItem[]
  /** The entry point to tutorial mode: plays the tutorial, or starts making one. */
  tutorial: { active: boolean; disabled: boolean; onToggle: () => void }
}

export function TopBar({ crumbs, onNavigate, menuItems, tutorial, agent }: TopBarProps) {
  return (
    <header className={styles.bar}>
      <div className={styles.crumbs}>
        <Breadcrumbs items={crumbs} onNavigate={onNavigate} />
      </div>
      <div className={styles.search}>
        <SearchField placeholder="Search files, classes, functions…" label="Search" />
      </div>
      {agent && (
        <Dropdown
          align="end"
          label="AI agent"
          items={(['claude', 'codex'] as const).map(provider => ({
            type: 'radio', id: provider, label: agents[provider].name,
            checked: agent.provider === provider, disabled: agent.disabled,
            onSelect: () => agent.onChange(provider),
          }))}
          trigger={props => (
            <button {...props} type="button" className={styles.agent} aria-label="AI agent"
              title={`AI agent: ${agents[agent.provider].name}`} disabled={agent.disabled}>
              <span>{agents[agent.provider].name}</span>
              <ChevronDownIcon className={styles.chevron} />
            </button>
          )}
        />
      )}
      <IconButton
        label={tutorial.active ? 'Exit tutorial' : 'Tutorial'}
        aria-pressed={tutorial.active}
        disabled={tutorial.disabled}
        onClick={tutorial.onToggle}
      >
        <PlayIcon />
      </IconButton>
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
