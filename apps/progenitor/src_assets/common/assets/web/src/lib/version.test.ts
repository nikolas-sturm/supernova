/**
 * @file Tests for version parsing and comparison.
 */

import { describe, expect, it } from 'vitest'
import { compareVersions, SunshineVersion } from './version'

describe('compareVersions', () => {
  it('compares major, minor, and patch segments', () => {
    expect(compareVersions('1.2.3', '1.2.4')).toBe(-1)
    expect(compareVersions('1.2.4', '1.2.3')).toBe(1)
    expect(compareVersions('2.0.0', '1.9.9')).toBe(1)
    expect(compareVersions('1.2.3', '1.2.3')).toBe(0)
  })

  it('handles missing segments as zero', () => {
    expect(compareVersions('1.2', '1.2.0')).toBe(0)
    expect(compareVersions('1', '1.0.1')).toBe(-1)
  })

  it('ignores a leading v prefix', () => {
    expect(compareVersions('v1.2.3', '1.2.3')).toBe(0)
    expect(compareVersions('v2.0.0', '1.0.0')).toBe(1)
  })

  it('returns null for invalid input', () => {
    expect(compareVersions('', '1.0.0')).toBeNull()
    expect(compareVersions('1.0.0', '')).toBeNull()
  })
})

describe('SunshineVersion', () => {
  it('detects greater versions', () => {
    const installed = new SunshineVersion('1.2.3')
    expect(installed.isGreater('1.2.2')).toBe(true)
    expect(installed.isGreater('1.2.3')).toBe(false)
    expect(installed.isGreater('2.0.0')).toBe(false)
  })

  it('accepts other SunshineVersion instances', () => {
    const a = new SunshineVersion('3.0.0')
    expect(a.isGreater(new SunshineVersion('2.9.9'))).toBe(true)
  })

  it('detects dirty builds', () => {
    expect(new SunshineVersion('0.0.0.0.dirty').isDirty()).toBe(true)
    expect(new SunshineVersion('1.2.3').isDirty()).toBe(false)
  })
})
