/**
 * @file Tests for GameDB cover search helpers.
 */

import { describe, expect, it } from 'vitest'
import { getSearchBucket } from './covers'

describe('getSearchBucket', () => {
  it('uses the first two alphanumeric characters lowercased', () => {
    expect(getSearchBucket('Half-Life')).toBe('ha')
    expect(getSearchBucket('The Witcher')).toBe('th')
  })

  it('falls back to @ when no usable characters exist', () => {
    expect(getSearchBucket('!!!')).toBe('@')
    expect(getSearchBucket('')).toBe('@')
  })
})
