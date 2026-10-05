import { execFileSync } from 'node:child_process'
import { fileURLToPath } from 'node:url'
import './browser.ts'
execFileSync(process.execPath, [fileURLToPath(new URL('../node_modules/playwright/cli.js', import.meta.url)), 'install', 'chromium'], { stdio: 'inherit' })
