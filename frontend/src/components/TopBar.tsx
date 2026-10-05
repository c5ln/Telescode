// The only persistent chrome: breadcrumbs | flexible space | search | tutorial | overflow.

import { Breadcrumbs, type Crumb } from '../ui/Breadcrumbs'
import { Dropdown, type DropdownItem } from '../ui/Dropdown'
import { MoreIcon, PlayIcon } from '../ui/icons'
import { IconButton } from '../ui/IconButton'
import { SearchField } from '../ui/SearchField'
import styles from './TopBar.module.css'

interface TopBarProps {
  crumbs: readonly Crumb[]
  onNavigate?: (crumb: Crumb, index: number) => void
  menuItems: DropdownItem[]
  /** The entry point to tutorial mode: plays the tutorial, or starts making one. */
  tutorial: { active: boolean; disabled: boolean; onToggle: () => void }
}

export function TopBar({ crumbs, onNavigate, menuItems, tutorial }: TopBarProps) {
  return (
    <header className={styles.bar}>
      <div className={styles.crumbs}>
        <Breadcrumbs items={crumbs} onNavigate={onNavigate} />
      </div>
      <div className={styles.search}>
        <SearchField placeholder="Search files, classes, functions…" label="Search" />
      </div>
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
