/**
 * @file Log parsing utilities for the Sunshine log viewer.
 * Ported from the legacy troubleshooting page.
 */

/** Log severity levels recognized in Sunshine logs. */
export type LogLevel = 'Fatal' | 'Error' | 'Warning' | 'Debug' | 'Info'

/** A single parsed log entry starting with a timestamp token. */
export interface LogEntry {
  /** Zero-based position of the entry within the parsed output. */
  index: number
  /** Raw entry text, including the timestamp prefix. */
  raw: string
  /** Detected severity level. */
  level: LogLevel
}

const TIMESTAMP_REGEX = /\[\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3}\]:/g

/**
 * @brief Parses raw log text into timestamp-prefixed entries.
 *
 * Entries may span multiple lines. When no timestamps are present the whole
 * text is returned as a single Info entry.
 * @param text The raw log text.
 * @returns Parsed entries in order of appearance.
 */
export function parseLogEntries(text: string): LogEntry[] {
  const matches = [...text.matchAll(TIMESTAMP_REGEX)]

  if (matches.length === 0) {
    const raw = text.trimEnd()
    if (!raw) {
      return []
    }
    return [{ index: 0, raw, level: 'Info' }]
  }

  const entries: LogEntry[] = []
  for (let i = 0; i < matches.length; i++) {
    const match = matches[i]
    if (!match) {
      continue
    }
    const next = matches[i + 1]
    const start = match.index ?? 0
    const end = next?.index ?? text.length
    const raw = text.slice(start, end).trimEnd()
    if (!raw) {
      continue
    }
    entries.push({ index: entries.length, raw, level: detectLevel(raw) })
  }
  return entries
}

/**
 * @brief Detects the severity level of a single log entry.
 * @param raw The raw entry text.
 * @returns The detected level; defaults to Info.
 */
function detectLevel(raw: string): LogLevel {
  if (/\]:\s*Fatal:/i.test(raw)) {
    return 'Fatal'
  }
  if (/\]:\s*(Error|Critical):/i.test(raw)) {
    return 'Error'
  }
  if (/\]:\s*Warning:/i.test(raw)) {
    return 'Warning'
  }
  if (/\]:\s*Debug:/i.test(raw)) {
    return 'Debug'
  }
  return 'Info'
}

/**
 * @brief Filters log text line-by-line, keeping lines containing the query.
 * @param text The raw log text.
 * @param filter The case-insensitive substring to keep; empty keeps everything.
 * @returns The filtered text.
 */
export function filterLogs(text: string, filter: string): string {
  if (!filter) {
    return text
  }
  const lower = filter.toLowerCase()
  return text
    .split('\n')
    .filter((line) => line.toLowerCase().includes(lower))
    .join('\n')
}
