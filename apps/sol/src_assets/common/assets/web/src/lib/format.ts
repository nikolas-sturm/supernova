/**
 * @brief Formats large numbers for compact display (e.g. GitHub stars).
 * @param num The number to format.
 * @returns Compact representation such as "1.2k" or "3.4M".
 */
export function formatNumber(num: number | undefined | null): string {
  if (num === undefined || num === null) {
    return '0'
  }
  const absNum = Math.abs(num)
  if (absNum >= 1_000_000_000) {
    return `${(num / 1_000_000_000).toFixed(1)}B`
  }
  if (absNum >= 1_000_000) {
    return `${(num / 1_000_000).toFixed(1)}M`
  }
  if (absNum >= 1_000) {
    return `${(num / 1_000).toFixed(1)}k`
  }
  return num.toString()
}
