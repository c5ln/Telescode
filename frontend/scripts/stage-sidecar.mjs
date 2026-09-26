// Copies the CMake-built TelescodeHeadless into src-tauri/binaries/ under the
// name Tauri's `externalBin` expects: TelescodeHeadless-<target-triple>[.exe].
//
// The C++ build directory defaults to <repo>/build and can be overridden with
// TELESCODE_BUILD_DIR. The target triple comes from TAURI_ENV_TARGET_TRIPLE when
// Tauri sets it, otherwise from the host rustc.

import { execFileSync } from 'node:child_process'
import { copyFileSync, existsSync, mkdirSync } from 'node:fs'
import { dirname, join, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'

const here = dirname(fileURLToPath(import.meta.url))
const repoRoot = resolve(here, '..', '..')
const buildDir = resolve(process.env.TELESCODE_BUILD_DIR ?? join(repoRoot, 'build'))
const ext = process.platform === 'win32' ? '.exe' : ''
const exe = `TelescodeHeadless${ext}`

// Multi-config generators (Visual Studio, Xcode) put the binary in a
// per-configuration folder; single-config ones put it at the top.
const candidates = ['Release', 'RelWithDebInfo', 'MinSizeRel', 'Debug', '.'].map((c) =>
  join(buildDir, c, exe),
)
const source = candidates.find((p) => existsSync(p))
if (!source) {
  console.error(
    `[sidecar] ${exe} not found under ${buildDir}.\n` +
      '          Build the C++ core first:\n' +
      '            cmake -S . -B build\n' +
      '            cmake --build build --config Release --target TelescodeHeadless\n' +
      '          or point TELESCODE_BUILD_DIR at an existing build directory.',
  )
  process.exit(1)
}

function targetTriple() {
  if (process.env.TAURI_ENV_TARGET_TRIPLE) return process.env.TAURI_ENV_TARGET_TRIPLE
  const out = execFileSync('rustc', ['-vV'], { encoding: 'utf8' })
  const host = /^host:\s*(\S+)/m.exec(out)
  if (!host) throw new Error('could not read the host target triple from `rustc -vV`')
  return host[1]
}

const destDir = join(here, '..', 'src-tauri', 'binaries')
const dest = join(destDir, `TelescodeHeadless-${targetTriple()}${ext}`)
mkdirSync(destDir, { recursive: true })
copyFileSync(source, dest)
console.log(`[sidecar] ${source} -> ${dest}`)
