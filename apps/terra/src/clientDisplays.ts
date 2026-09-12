import type { ClientDisplayOutput, ClientDisplayPreference } from './store/clientStore'

export interface VirtualDisplaySpecification {
  name: string
  mode: {
    width: number
    height: number
    refreshNumerator: number
    refreshDenominator: number
    bitDepth: number
    hdr: boolean
  }
  position: { x: number; y: number }
  scale: number
  rotation: number
  primary: boolean
  hdr: boolean
  persistent: boolean
}

export function effectiveClientDisplay(
  output: ClientDisplayOutput,
  preference: ClientDisplayPreference | undefined,
): ClientDisplayPreference {
  if (preference) return preference
  return {
    enabled: true,
    width: output.width,
    height: output.height,
    refreshRate: output.refreshRate,
    hdr: false,
  }
}

export function enabledClientDisplays(
  outputs: ClientDisplayOutput[],
  preferences: Record<string, ClientDisplayPreference>,
) {
  return outputs
    .map((output) => ({
      output,
      preference: effectiveClientDisplay(output, preferences[output.id]),
    }))
    .filter((entry) => entry.preference.enabled)
}

export function clientDisplayVirtualDisplays(
  outputs: ClientDisplayOutput[],
  preferences: Record<string, ClientDisplayPreference>,
): VirtualDisplaySpecification[] {
  const enabled = enabledClientDisplays(outputs, preferences)
  if (enabled.length === 0) return []
  const primaryIndex = enabled.findIndex((entry) => entry.output.primary)
  const ordered = [...enabled]
  if (primaryIndex > 0) {
    ordered.unshift(...ordered.splice(primaryIndex, 1))
  }
  let offset = 0
  return ordered.map(({ output, preference }) => {
    const hdr = preference.hdr
    const specification: VirtualDisplaySpecification = {
      name: output.name || 'Client display',
      mode: {
        width: preference.width,
        height: preference.height,
        refreshNumerator: preference.refreshRate,
        refreshDenominator: 1,
        bitDepth: hdr ? 10 : 8,
        hdr,
      },
      position: { x: offset, y: 0 },
      scale: 1,
      rotation: 0,
      primary: offset === 0,
      hdr,
      persistent: false,
    }
    offset += preference.width
    return specification
  })
}
