/**
 * @file Tests for log parsing and filtering.
 */

import { describe, expect, it } from 'vitest'
import { filterLogs, parseLogEntries } from './logs'

const sample = [
  '[2026-09-01 10:00:00.000]: Info: Sol started',
  '[2026-09-01 10:00:01.000]: Warning: something odd',
  '[2026-09-01 10:00:02.000]: Error: bad thing',
  '[2026-09-01 10:00:03.000]: Fatal: cannot continue',
  '[2026-09-01 10:00:04.000]: Debug: details',
].join('\n')

describe('parseLogEntries', () => {
  it('splits entries on timestamp tokens', () => {
    const entries = parseLogEntries(sample)
    expect(entries).toHaveLength(5)
    expect(entries[0]?.level).toBe('Info')
    expect(entries[1]?.level).toBe('Warning')
    expect(entries[2]?.level).toBe('Error')
    expect(entries[3]?.level).toBe('Fatal')
    expect(entries[4]?.level).toBe('Debug')
  })

  it('keeps multi-line messages inside one entry', () => {
    const text =
      '[2026-09-01 10:00:00.000]: Info: line one\ncontinued line\n[2026-09-01 10:00:01.000]: Info: next'
    const entries = parseLogEntries(text)
    expect(entries).toHaveLength(2)
    expect(entries[0]?.raw).toContain('continued line')
  })

  it('returns a single info entry without timestamps', () => {
    const entries = parseLogEntries('plain text')
    expect(entries).toHaveLength(1)
    expect(entries[0]?.level).toBe('Info')
  })

  it('returns no entries for empty text', () => {
    expect(parseLogEntries('')).toHaveLength(0)
  })

  it('treats Critical as Error', () => {
    const entries = parseLogEntries('[2026-09-01 10:00:00.000]: Critical: boom')
    expect(entries[0]?.level).toBe('Error')
  })
})

describe('filterLogs', () => {
  it('keeps matching lines case-insensitively', () => {
    const filtered = filterLogs(sample, 'warning')
    expect(filtered).toContain('Warning')
    expect(filtered).not.toContain('Fatal')
  })

  it('returns the original text for empty filter', () => {
    expect(filterLogs(sample, '')).toBe(sample)
  })
})
