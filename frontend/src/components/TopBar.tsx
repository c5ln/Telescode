// The only persistent chrome: breadcrumbs | flexible space | search | overflow.

import { Breadcrumbs, type Crumb } from '../ui/Breadcrumbs'
import { Dropdown, type DropdownItem } from '../ui/Dropdown'
import { MoreIcon } from '../ui/icons'
import { IconButton } from '../ui/IconButton'
import { SearchField } from '../ui/SearchField'
import styles from './TopBar.module.css'

interface TopBarProps {
  crumbs: readonly Crumb[]
  onNavigate?: (crumb: Crumb, index: number) => void
  menuItems: DropdownItem[]
}

export function TopBar({ crumbs, onNavigate, menuItems }: TopBarProps) {
  return (
    <header className={styles.bar}>
      <div className={styles.crumbs}>
        <Breadcrumbs items={crumbs} onNavigate={onNavigate} />
      </div>
      <div className={styles.search}>
        <SearchField placeholder="Search files, classes, functions…" label="Search" />
      </div>
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
