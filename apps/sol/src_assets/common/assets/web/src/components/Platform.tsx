/**
 * @file Platform helpers: conditional rendering and platform-aware i18n.
 *
 * Ports the legacy `PlatformLayout.vue` and `platform-i18n.js`. The platform
 * value comes from `GET /api/config` and is shared via React context.
 */

import { createContext, type ReactNode, useContext } from 'react'
import { useTranslation } from 'react-i18next'

/** Values the backend reports in `config.platform`. */
export type Platform = 'windows' | 'linux' | 'freebsd' | 'macos' | ''

/** Context carrying the current platform string. */
export const PlatformContext = createContext<Platform>('')

/**
 * @brief Reads the current platform from context.
 * @returns The platform identifier.
 */
export function usePlatform(): Platform {
  return useContext(PlatformContext)
}

export interface PlatformSwitchProps {
  /** Content rendered only on Windows. */
  windows?: ReactNode
  /** Content rendered only on Linux. */
  linux?: ReactNode
  /** Content rendered only on FreeBSD. */
  freebsd?: ReactNode
  /** Content rendered only on macOS. */
  macos?: ReactNode
}

/**
 * @brief Renders the child matching the current platform.
 * @param props Per-platform children.
 * @returns The matching content, or null when the platform has no child.
 */
export function PlatformSwitch({ windows, linux, freebsd, macos }: PlatformSwitchProps) {
  const platform = usePlatform()
  switch (platform) {
    case 'windows':
      return <>{windows ?? null}</>
    case 'linux':
      return <>{linux ?? null}</>
    case 'freebsd':
      return <>{freebsd ?? null}</>
    case 'macos':
      return <>{macos ?? null}</>
    default:
      return null
  }
}

/**
 * @brief Resolves a platform-suffixed translation key.
 *
 * Tries `key_<platform>` first; on non-Windows platforms falls back to the
 * shared `key_unix` variant; finally falls back to the provided default.
 * @param key The base translation key.
 * @param defaultMsg Optional default when no translation exists.
 * @returns The resolved message.
 */
export function usePlatformTranslation() {
  const { t } = useTranslation()
  const platform = usePlatform()

  return (key: string, defaultMsg?: string): string => {
    const platformKey = `${key}_${platform}`
    const translated = t(platformKey)
    if (translated !== platformKey) {
      return translated
    }
    if (platform === 'windows') {
      return defaultMsg ?? translated
    }
    const unixKey = `${key}_unix`
    const unixTranslated = t(unixKey)
    if (unixTranslated === unixKey && defaultMsg !== undefined) {
      return defaultMsg
    }
    return unixTranslated
  }
}
