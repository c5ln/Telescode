import { fileURLToPath } from 'node:url'

// Keep the browser cache with this package on Linux and Windows.
process.env.PLAYWRIGHT_BROWSERS_PATH ??= fileURLToPath(new URL('../node_modules/.cache/playwright', import.meta.url))
export const { chromium } = await import('playwright')
