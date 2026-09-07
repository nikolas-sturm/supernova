/**
 * @file Driver version comparison helpers for the troubleshooting page.
 * Ported from the legacy troubleshooting page.
 */

/**
 * @brief Parses a numeric dotted driver or release version.
 * @param version Version string, optionally prefixed with "v".
 * @returns Numeric version parts, or null for an invalid version.
 */
export function parseDriverVersion(version: string | null | undefined): number[] | null {
  const match = /^v?(\d+(?:\.\d+)*)/i.exec(String(version ?? '').trim())
  return match?.[1] ? match[1].split('.').map(Number) : null
}

/**
 * @brief Compares installed and latest driver versions.
 * @param installedVersion Installed driver version.
 * @param latestVersion Latest stable release version.
 * @returns Negative if outdated, zero if equal, positive if newer, null if invalid.
 */
export function compareDriverVersions(
  installedVersion: string,
  latestVersion: string,
): number | null {
  const installedParts = parseDriverVersion(installedVersion)
  const latestParts = parseDriverVersion(latestVersion)
  if (!installedParts || !latestParts) {
    return null
  }
  const partCount = Math.max(installedParts.length, latestParts.length)
  for (let index = 0; index < partCount; ++index) {
    const installedPart = installedParts[index] ?? 0
    const latestPart = latestParts[index] ?? 0
    if (installedPart !== latestPart) {
      return installedPart > latestPart ? 1 : -1
    }
  }
  return 0
}

/** Status of the latest GitHub release for one driver. */
export interface DriverRelease {
  /** True while the release lookup is in flight. */
  loading: boolean
  /** Latest stable version tag, empty when unknown. */
  version: string
  /** URL of the release page. */
  url: string
  /** True when the lookup failed. */
  error: boolean
}

/** Installed status of one virtual input driver. */
export interface DriverStatus {
  installed: boolean
  version: string
  version_compatible: boolean
  minimum_version: string
  supported_versions: string
}

/**
 * @brief Determines the availability state for one driver and its latest release.
 * @param driver Installed driver status.
 * @param release Latest GitHub release status.
 * @returns One of: loading, unavailable, not-installed, unknown, current, outdated.
 */
export function driverReleaseState(driver: DriverStatus, release: DriverRelease): string {
  if (release.loading) {
    return 'loading'
  }
  if (release.error || !release.version) {
    return 'unavailable'
  }
  if (!driver.installed) {
    return 'not-installed'
  }
  const comparison = compareDriverVersions(driver.version, release.version)
  if (comparison === null) {
    return 'unknown'
  }
  return comparison >= 0 ? 'current' : 'outdated'
}
