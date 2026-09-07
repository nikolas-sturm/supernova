/**
 * @file Tests for the theme store.
 */

import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { getStoredThemePreference, resolveTheme, themeOptions, useThemeStore } from './themeStore'

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
      removeEventListener: vi.fn(),
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
  afterEach(() => vi.unstubAllGlobals())
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
    const dispose = useThemeStore.getState().initialize()
    expect(document.documentElement.dataset.theme).toBe('ocean')
    dispose()
  })

  it('defaults to Terra dark and rejects unknown stored theme names', () => {
    expect(getStoredThemePreference()).toBe('dark')
    window.localStorage.setItem('theme', 'not-a-theme')
    expect(getStoredThemePreference()).toBe('dark')
  })

  it.each([...themeOptions.dark, ...themeOptions.light])(
    'retains the %s selector and persistence',
    (theme) => {
      useThemeStore.getState().setTheme(theme)
      expect(document.documentElement.dataset.theme).toBe(theme)
      expect(getStoredThemePreference()).toBe(theme)
    },
  )

  it('tracks system changes only in auto mode and releases listeners', () => {
    const media = { matches: false, addEventListener: vi.fn(), removeEventListener: vi.fn() }
    vi.stubGlobal('matchMedia', () => media)
    const dispose = useThemeStore.getState().initialize()
    const onChange = media.addEventListener.mock.calls[0]?.[1] as () => void
    useThemeStore.getState().setTheme('auto')
    expect(document.documentElement.dataset.theme).toBe('light')
    media.matches = true
    onChange()
    expect(document.documentElement.dataset.theme).toBe('dark')
    useThemeStore.getState().setTheme('mocha')
    media.matches = false
    onChange()
    expect(document.documentElement.dataset.theme).toBe('mocha')
    dispose()
    expect(media.removeEventListener).toHaveBeenCalledWith('change', onChange)
  })

  it('syncs persisted changes from other windows without touching document classes', () => {
    document.documentElement.classList.add('stream-overlay-window')
    const dispose = useThemeStore.getState().initialize()
    window.localStorage.setItem('theme', 'latte')
    window.dispatchEvent(new StorageEvent('storage', { key: 'theme', newValue: 'latte' }))
    expect(useThemeStore.getState().preference).toBe('latte')
    expect(document.documentElement.dataset.theme).toBe('latte')
    expect(document.documentElement).toHaveClass('stream-overlay-window')
    dispose()
    document.documentElement.classList.remove('stream-overlay-window')
  })

  it('keeps selection working when storage is denied', () => {
    const get = vi.spyOn(Storage.prototype, 'getItem').mockImplementation(() => {
      throw new Error('denied')
    })
    const set = vi.spyOn(Storage.prototype, 'setItem').mockImplementation(() => {
      throw new Error('denied')
    })
    expect(getStoredThemePreference()).toBe('dark')
    useThemeStore.getState().setTheme('ember')
    expect(document.documentElement.dataset.theme).toBe('ember')
    expect(useThemeStore.getState().preference).toBe('ember')
    get.mockRestore()
    set.mockRestore()
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
