/**
 * @file Version parsing and comparison for Sunshine and GitHub releases.
 * Ported from the legacy `sunshine_version.js`.
 */

/**
 * @brief Parses a dotted version string into numeric parts.
 * @param version Version string, optionally prefixed with "v".
 * @returns Numeric version parts, or null when the input is empty.
 */
export function parseVersion(version: string | null | undefined): number[] | null {
  if (!version) {
    return null
  }
  const v = version.startsWith('v') ? version.slice(1) : version
  return v.split('.').map(Number)
}

/**
 * @brief Compares two version strings numerically.
 * @param a First version string.
 * @param b Second version string.
 * @returns Positive when a > b, negative when a < b, zero when equal, null when either is invalid.
 */
export function compareVersions(a: string, b: string): number | null {
  const aParts = parseVersion(a)
  const bParts = parseVersion(b)
  if (!aParts || !bParts) {
    return null
  }
  const length = Math.max(aParts.length, bParts.length)
  for (let i = 0; i < length; i++) {
    const aPart = aParts[i] ?? 0
    const bPart = bParts[i] ?? 0
    if (aPart !== bPart) {
      return aPart > bPart ? 1 : -1
    }
  }
  return 0
}

/**
 * @brief Semantic wrapper around two versions with comparison helpers.
 */
export class SunshineVersion {
  /** The version string this instance represents. */
  readonly version: string
  /** Numeric version parts. */
  readonly parts: number[] | null

  /**
   * @param version The version string.
   */
  constructor(version: string) {
    this.version = version
    this.parts = parseVersion(version)
  }

  /**
   * @brief Checks whether this version is strictly greater than another.
   * @param other The other version string or instance.
   * @returns True when this version is greater.
   */
  isGreater(other: string | SunshineVersion): boolean {
    const otherVersion = typeof other === 'string' ? other : other.version
    return compareVersions(this.version, otherVersion) === 1
  }

  /**
   * @brief Checks whether the version is a dirty development build (five segments containing "dirty").
   * @returns True when the build is dirty.
   */
  isDirty(): boolean {
    return this.version.split('.').length === 5 && this.version.includes('dirty')
  }
}
