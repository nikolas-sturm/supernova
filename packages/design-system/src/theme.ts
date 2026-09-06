/**
 * @file Theme state: preference persistence and DOM application.
 * Shared by the host administration UI and streaming client.
 */

import { create } from 'zustand'

/** All selectable themes, grouped for the theme menu. */
export const themeOptions = {
  dark: [
    'dark',
    'dracula',
    'mocha',
    'ember',
    'rose-pine',
    'moonlight',
    'slate',
    'midnight',
    'nord',
  ],
  light: [
    'light',
    'alucard',
    'latte',
    'ember-light',
    'rose-pine-dawn',
    'sunshine',
    'indigo',
    'ocean',
    'forest',
    'rose',
    'lavender',
    'monochrome',
  ],
} as const

/** A concrete theme name or the `auto` preference. */
export type ThemePreference =
  | 'auto'
  | (typeof themeOptions)['dark'][number]
  | (typeof themeOptions)['light'][number]

const STORAGE_KEY = 'theme'

/**
 * @brief Reads the persisted theme preference.
 * @returns A validated preference, or Eclipse dark when none is stored.
 */
export function getStoredThemePreference(): ThemePreference {
  try {
    const stored = window.localStorage.getItem(STORAGE_KEY)
    return isThemePreference(stored) ? stored : 'dark'
  } catch {
    return 'dark'
  }
}

/** @brief Validates persisted preferences without accepting arbitrary selectors. */
export function isThemePreference(value: unknown): value is ThemePreference {
  return (
    value === 'auto' ||
    [...themeOptions.dark, ...themeOptions.light].some((theme) => theme === value)
  )
}

/**
 * @brief Resolves a theme preference to a concrete theme name.
 * @param preference The preference to resolve.
 * @returns The concrete theme; `auto` resolves through `prefers-color-scheme`.
 */
export function resolveTheme(preference: ThemePreference): string {
  if (preference !== 'auto') {
    return preference
  }
  return typeof window.matchMedia !== 'function' ||
    window.matchMedia('(prefers-color-scheme: dark)').matches
    ? 'dark'
    : 'light'
}

/**
 * @brief Applies the resolved theme to the document element.
 * @param preference The preference being applied.
 */
function applyToDom(preference: ThemePreference): void {
  document.documentElement.dataset.theme = resolveTheme(preference)
}

interface ThemeState {
  /** The active preference (`auto` or a concrete theme name). */
  preference: ThemePreference
  /**
   * @brief Persists and applies a theme preference.
   * @param preference The new preference.
   */
  setTheme: (preference: ThemePreference) => void
  /**
   * @brief Applies a random concrete theme (excluding the current one and `auto`).
   */
  randomTheme: () => void
  /**
   * @brief Reads the stored preference and applies it; subscribes to OS scheme changes.
   */
  initialize: () => () => void
}

export const useThemeStore = create<ThemeState>()((set, get) => ({
  preference: 'dark',
  setTheme: (preference) => {
    if (!isThemePreference(preference)) return
    try {
      window.localStorage.setItem(STORAGE_KEY, preference)
    } catch {
      // Private browsing and embedded webviews may deny storage. Selection still works.
    }
    applyToDom(preference)
    set({ preference })
  },
  randomTheme: () => {
    const all = [...themeOptions.dark, ...themeOptions.light]
    const current = resolveTheme(get().preference)
    const values = all.filter((theme) => theme !== current)
    const pick = values[Math.floor(Math.random() * values.length)]
    if (pick) {
      get().setTheme(pick)
    }
  },
  initialize: () => {
    const preference = getStoredThemePreference()
    applyToDom(preference)
    set({ preference })

    const media =
      typeof window.matchMedia === 'function'
        ? window.matchMedia('(prefers-color-scheme: dark)')
        : undefined
    const onSchemeChange = () => {
      if (get().preference === 'auto') {
        applyToDom('auto')
      }
    }
    const onStorage = (event: StorageEvent) => {
      if (event.key !== STORAGE_KEY && event.key !== null) return
      const preference = getStoredThemePreference()
      applyToDom(preference)
      set({ preference })
    }
    media?.addEventListener('change', onSchemeChange)
    window.addEventListener('storage', onStorage)
    return () => {
      media?.removeEventListener('change', onSchemeChange)
      window.removeEventListener('storage', onStorage)
    }
  },
}))
