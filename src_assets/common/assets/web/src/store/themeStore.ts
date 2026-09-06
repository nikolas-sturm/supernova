/**
 * @file Theme state: preference persistence and DOM application.
 * Ported from the legacy `theme.js`.
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
 * @returns The stored preference, or `auto` when none is stored.
 */
export function getStoredThemePreference(): ThemePreference {
  const stored = window.localStorage.getItem(STORAGE_KEY)
  return (stored as ThemePreference) || 'auto'
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
  return window.matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light'
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
  initialize: () => void
}

export const useThemeStore = create<ThemeState>()((set, get) => ({
  preference: 'auto',
  setTheme: (preference) => {
    window.localStorage.setItem(STORAGE_KEY, preference)
    applyToDom(preference)
    set({ preference })
  },
  randomTheme: () => {
    const all = [...themeOptions.dark, ...themeOptions.light]
    const current = get().preference
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

    window.matchMedia('(prefers-color-scheme: dark)').addEventListener('change', () => {
      if (getStoredThemePreference() === 'auto') {
        applyToDom('auto')
      }
    })
  },
}))
