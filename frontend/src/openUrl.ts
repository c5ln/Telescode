import { isTauri } from '@tauri-apps/api/core'
import { openUrl } from '@tauri-apps/plugin-opener'

/** Opens a web page in the user's browser. */
export function openExternal(url: string): void {
  if (isTauri()) void openUrl(url)
  else window.open(url, '_blank', 'noopener')
}
