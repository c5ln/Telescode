// Structured failures from the bridge. Components branch on `code`; `message`
// is for display.

/** Codes produced by the Rust side (src-tauri/src/bridge.rs). */
export type NativeErrorCode =
  | 'invalid_argument'
  | 'db_not_found'
  | 'db_not_a_file'
  | 'db_invalid'
  | 'sidecar_missing'
  | 'sidecar_spawn_failed'
  | 'sidecar_failed'
  | 'output_not_utf8'
  | 'internal'

/** Codes produced on the TypeScript side. */
export type ClientErrorCode =
  /** Not running inside the Tauri shell (e.g. plain `npm run dev` in a browser). */
  | 'bridge_unavailable'
  /** The core's stdout was not valid JSON. */
  | 'malformed_json'
  /** Valid JSON, but not the shape this frontend was written against. */
  | 'unexpected_schema'
  /** A schemaVersion this frontend does not understand. */
  | 'unsupported_schema_version'
  /** Anything that did not arrive in a recognised form. */
  | 'unknown'

export type TelescodeErrorCode = NativeErrorCode | ClientErrorCode

export interface TelescodeErrorDetails {
  path?: string
  exitCode?: number | null
  stderr?: string
}

export class TelescodeError extends Error {
  readonly code: TelescodeErrorCode
  readonly details: TelescodeErrorDetails

  constructor(code: TelescodeErrorCode, message: string, details: TelescodeErrorDetails = {}) {
    super(message)
    this.name = 'TelescodeError'
    this.code = code
    this.details = details
  }
}

const NATIVE_CODES: ReadonlySet<string> = new Set<NativeErrorCode>([
  'invalid_argument',
  'db_not_found',
  'db_not_a_file',
  'db_invalid',
  'sidecar_missing',
  'sidecar_spawn_failed',
  'sidecar_failed',
  'output_not_utf8',
  'internal',
])

/** Turns whatever `invoke` rejected with into a TelescodeError. */
export function fromNativeError(raw: unknown): TelescodeError {
  if (raw instanceof TelescodeError) return raw
  if (typeof raw === 'object' && raw !== null) {
    const r = raw as Record<string, unknown>
    if (typeof r.code === 'string' && NATIVE_CODES.has(r.code)) {
      return new TelescodeError(
        r.code as NativeErrorCode,
        typeof r.message === 'string' ? r.message : r.code,
        {
          path: typeof r.path === 'string' ? r.path : undefined,
          exitCode: typeof r.exitCode === 'number' ? r.exitCode : r.exitCode === null ? null : undefined,
          stderr: typeof r.stderr === 'string' ? r.stderr : undefined,
        },
      )
    }
  }
  const message = raw instanceof Error ? raw.message : typeof raw === 'string' ? raw : 'Unknown bridge error'
  return new TelescodeError('unknown', message)
}
