/**
 * @file Tests for the theme store.
 */

import { beforeEach, describe, expect, it, vi } from 'vitest'
import { getStoredThemePreference, resolveTheme, useThemeStore } from './themeStore'

/**
 * @brief Installs a matchMedia stub (jsdom does not implement it).
 * @param dark Whether prefers-color-scheme: dark matches.
 */
function stubMatchMedia(dark: boolean): void {
  vi.stubGlobal(
    'matchMedia',
    vi.fn().mockImplementation((query: string) => ({
      matches: dark && query.includes('dark'),
      addEventListener: vi.fn(),
    })),
  )
}

describe('resolveTheme', () => {
  it('passes concrete themes through', () => {
    expect(resolveTheme('dracula')).toBe('dracula')
    expect(resolveTheme('light')).toBe('light')
  })

  it('resolves auto against prefers-color-scheme', () => {
    stubMatchMedia(true)
    expect(resolveTheme('auto')).toBe('dark')
    stubMatchMedia(false)
    expect(resolveTheme('auto')).toBe('light')
    vi.unstubAllGlobals()
  })
})

describe('themeStore', () => {
  beforeEach(() => {
    stubMatchMedia(false)
    window.localStorage.clear()
    document.documentElement.dataset.theme = ''
  })

  it('persists and applies a selected theme', () => {
    useThemeStore.getState().setTheme('nord')
    expect(window.localStorage.getItem('theme')).toBe('nord')
    expect(document.documentElement.dataset.theme).toBe('nord')
    expect(useThemeStore.getState().preference).toBe('nord')
    expect(getStoredThemePreference()).toBe('nord')
  })

  it('initializes from storage', () => {
    window.localStorage.setItem('theme', 'ocean')
    useThemeStore.getState().initialize()
    expect(document.documentElement.dataset.theme).toBe('ocean')
  })

  it('random theme picks a concrete theme', () => {
    useThemeStore.getState().setTheme('dark')
    useThemeStore.getState().randomTheme()
    const preference = useThemeStore.getState().preference
    expect(preference).not.toBe('auto')
    expect(preference).not.toBe('dark')
    expect(document.documentElement.dataset.theme).toBe(preference)
  })
})
