import { describe, expect, it } from 'vitest'
import {
  clientDisplayVirtualDisplays,
  effectiveClientDisplay,
  resolvePrimaryClientDisplayId,
} from './clientDisplays'
import type { ClientDisplayOutput } from './store/clientStore'

const primary: ClientDisplayOutput = {
  id: 'display-1',
  name: 'Center',
  primary: true,
  x: 0,
  y: 0,
  width: 2560,
  height: 1440,
  refreshRate: 60,
  modes: [{ width: 2560, height: 1440, refreshRate: 60 }],
}

const secondary: ClientDisplayOutput = {
  id: 'display-2',
  name: 'Right',
  primary: false,
  x: 2560,
  y: 0,
  width: 1920,
  height: 1080,
  refreshRate: 60,
  modes: [{ width: 1920, height: 1080, refreshRate: 60 }],
}

describe('clientDisplays', () => {
  it('defaults every detected output to streamed at its native mode', () => {
    expect(effectiveClientDisplay(primary, undefined)).toEqual({
      enabled: true,
      width: 2560,
      height: 1440,
      refreshRate: 60,
      hdr: false,
    })
    expect(clientDisplayVirtualDisplays([primary, secondary], {})).toHaveLength(2)
  })

  it('streams only enabled outputs and lays them out without overlap', () => {
    const displays = clientDisplayVirtualDisplays([secondary, primary], {
      'display-2': { enabled: false, width: 1920, height: 1080, refreshRate: 60, hdr: false },
    })

    expect(displays).toHaveLength(1)
    expect(displays[0]).toMatchObject({
      name: 'Center',
      primary: true,
      position: { x: 0, y: 0 },
      persistent: false,
    })
  })

  it('puts the primary output at the origin and offsets the rest', () => {
    const displays = clientDisplayVirtualDisplays([secondary, primary], {})

    expect(displays[0]).toMatchObject({ name: 'Center', primary: true, position: { x: 0, y: 0 } })
    expect(displays[1]).toMatchObject({
      name: 'Right',
      primary: false,
      position: { x: 2560, y: 0 },
    })
  })

  it('honors a manually selected primary and falls back when it is unavailable', () => {
    expect(resolvePrimaryClientDisplayId([primary, secondary], {}, 'display-2')).toBe('display-2')
    expect(resolvePrimaryClientDisplayId([primary, secondary], {}, 'missing')).toBe('display-1')
    expect(
      resolvePrimaryClientDisplayId(
        [primary, secondary],
        { 'display-2': { enabled: false, width: 1920, height: 1080, refreshRate: 60, hdr: false } },
        'display-2',
      ),
    ).toBe('display-1')

    const displays = clientDisplayVirtualDisplays([primary, secondary], {}, 'display-2')
    expect(displays[0]).toMatchObject({ name: 'Right', primary: true, position: { x: 0, y: 0 } })
    expect(displays[1]).toMatchObject({ name: 'Center', position: { x: 1920, y: 0 } })
  })

  it('marks HDR modes as ten bit', () => {
    const displays = clientDisplayVirtualDisplays([primary], {
      'display-1': { enabled: true, width: 2560, height: 1440, refreshRate: 60, hdr: true },
    })

    expect(displays[0]?.mode.bitDepth).toBe(10)
    expect(displays[0]?.hdr).toBe(true)
  })
})
