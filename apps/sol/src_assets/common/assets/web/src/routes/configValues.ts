/**
 * @file Hook exposing the config values shared by page bodies.
 *
 * Must be used inside a `Page` shell (which fetches the config).
 */

import { useQuery } from '@tanstack/react-query'
import { type Platform, usePlatform } from '../components/Platform'
import { configQuery } from '../queries'

/**
 * @brief Provides the raw config object, the platform string, and the version.
 * @returns Config values for the current page.
 */
export function useConfigValues() {
  const { data: config } = useQuery(configQuery())
  const platform = usePlatform()
  return {
    config: (config ?? {}) as Record<string, unknown> & { notify_pre_releases?: string },
    platform: platform as Platform,
    version: typeof config?.version === 'string' ? config.version : undefined,
  }
}
