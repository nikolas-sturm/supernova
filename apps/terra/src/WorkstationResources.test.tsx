import { fireEvent, render, screen } from '@testing-library/react'
import { beforeEach, describe, expect, it, vi } from 'vitest'
import { defaultSettings } from './settings'
import type { ClientDisplayOutput, Host } from './store/clientStore'
import { useClientStore } from './store/clientStore'
import { DisplayManager } from './WorkstationResources'

const coreBridge = vi.hoisted(() => ({
  loadCoreResource: vi.fn(),
  mutateCoreResource: vi.fn(),
  rescanClientDisplays: vi.fn(),
}))

vi.mock('./native/coreBridge', () => coreBridge)

const host: Host = {
  id: 'host-1',
  name: 'Studio PC',
  address: '192.168.1.40',
  serverName: 'Sol',
  serverUniqueId: 'server-1',
  appVersion: '1.0.0',
  serverState: 'IDLE',
  status: 'online',
  error: '',
  httpsPort: 47984,
  currentGameId: 0,
  serverCodecModeSupport: 1,
  maxLumaPixelsHevc: 0,
  displayModes: [],
  lastSeenAt: 1,
  paired: true,
  apiVersion: 1,
  capabilities: ['multi-display-streaming-v1'],
  apiScopes: ['display.read', 'display.manage'],
}

const output: ClientDisplayOutput = {
  id: 'display-1',
  name: 'Desk',
  primary: true,
  x: 0,
  y: 0,
  width: 2560,
  height: 1440,
  refreshRate: 60,
  modes: [
    { width: 2560, height: 1440, refreshRate: 60 },
    { width: 2560, height: 1440, refreshRate: 120 },
    { width: 1920, height: 1080, refreshRate: 60 },
  ],
}

describe('DisplayManager', () => {
  beforeEach(() => {
    coreBridge.loadCoreResource.mockReset()
    coreBridge.mutateCoreResource.mockReset()
    coreBridge.rescanClientDisplays.mockReset()
    useClientStore.setState({
      resourcesByHost: {},
      operationsByHost: {},
      clientDisplays: [output],
      clientDisplayPreferences: {},
      clientPrimaryDisplayId: undefined,
    })
  })

  it('persists a per-output HDR preference', () => {
    render(<DisplayManager host={host} apps={[]} settings={defaultSettings} onError={vi.fn()} />)

    fireEvent.click(screen.getByLabelText(/HDR/))

    expect(useClientStore.getState().clientDisplayPreferences[output.id]?.hdr).toBe(true)
  })

  it('keeps at least one output streamed', () => {
    render(<DisplayManager host={host} apps={[]} settings={defaultSettings} onError={vi.fn()} />)

    expect(screen.getByLabelText(/Stream this display/)).toBeChecked()
    expect(screen.getByLabelText(/Stream this display/)).toBeDisabled()
  })

  it('lets the user choose the streamed primary output', () => {
    const second: ClientDisplayOutput = { ...output, id: 'display-2', name: 'Side', primary: false }
    useClientStore.setState({ clientDisplays: [output, second] })
    render(<DisplayManager host={host} apps={[]} settings={defaultSettings} onError={vi.fn()} />)

    const primaries = screen.getAllByLabelText('Primary')
    expect(primaries).toHaveLength(2)
    fireEvent.click(primaries[1] as HTMLElement)

    expect(useClientStore.getState().clientPrimaryDisplayId).toBe('display-2')
  })

  it('repairs refresh rate when the resolution changes', () => {
    render(<DisplayManager host={host} apps={[]} settings={defaultSettings} onError={vi.fn()} />)

    fireEvent.change(screen.getByLabelText(/Resolution for Desk/), {
      target: { value: '1920x1080' },
    })
    fireEvent.change(screen.getByLabelText(/Refresh rate for Desk/), { target: { value: '60' } })

    const preference = useClientStore.getState().clientDisplayPreferences[output.id]
    expect(preference?.width).toBe(1920)
    expect(preference?.height).toBe(1080)
    expect(preference?.refreshRate).toBe(60)
  })
})
