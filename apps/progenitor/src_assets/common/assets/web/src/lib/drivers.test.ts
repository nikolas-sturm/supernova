/**
 * @file Tests for driver version helpers.
 */

import { describe, expect, it } from 'vitest'
import type { DriverRelease } from './drivers'
import { compareDriverVersions, driverReleaseState, parseDriverVersion } from './drivers'

const installedDriver = {
  installed: true,
  version: '1.2.0',
  version_compatible: true,
  minimum_version: '1.0.0',
  supported_versions: '1.x',
}

describe('parseDriverVersion', () => {
  it('parses dotted versions with optional v prefix', () => {
    expect(parseDriverVersion('v1.2.3')).toEqual([1, 2, 3])
    expect(parseDriverVersion('0.5')).toEqual([0, 5])
  })

  it('returns null for garbage', () => {
    expect(parseDriverVersion('not-a-version')).toBeNull()
  })
})

describe('compareDriverVersions', () => {
  it('detects outdated, current, and newer installs', () => {
    expect(compareDriverVersions('1.2.0', '1.3.0')).toBe(-1)
    expect(compareDriverVersions('1.3.0', '1.3.0')).toBe(0)
    expect(compareDriverVersions('2.0.0', '1.9.0')).toBe(1)
  })
})

describe('driverReleaseState', () => {
  const baseRelease: DriverRelease = { loading: false, version: '1.2.0', url: '', error: false }

  it('reports loading state', () => {
    expect(driverReleaseState(installedDriver, { ...baseRelease, loading: true })).toBe('loading')
  })

  it('reports unavailable on error or missing version', () => {
    expect(driverReleaseState(installedDriver, { ...baseRelease, error: true })).toBe('unavailable')
    expect(driverReleaseState(installedDriver, { ...baseRelease, version: '' })).toBe('unavailable')
  })

  it('reports not-installed separately', () => {
    expect(driverReleaseState({ ...installedDriver, installed: false }, baseRelease)).toBe(
      'not-installed',
    )
  })

  it('compares versions to current/outdated', () => {
    expect(driverReleaseState(installedDriver, baseRelease)).toBe('current')
    expect(driverReleaseState(installedDriver, { ...baseRelease, version: '2.0.0' })).toBe(
      'outdated',
    )
  })

  it('reports unknown when versions cannot be compared', () => {
    expect(driverReleaseState({ ...installedDriver, version: 'n/a' }, baseRelease)).toBe('unknown')
  })
})
