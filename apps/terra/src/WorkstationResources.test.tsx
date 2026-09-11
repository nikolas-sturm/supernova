import { fireEvent, render, screen, waitFor } from '@testing-library/react'
import { beforeEach, describe, expect, it, vi } from 'vitest'
import { defaultSettings } from './settings'
import type { Host } from './store/clientStore'
import { useClientStore } from './store/clientStore'
import { DisplayManager } from './WorkstationResources'

const coreBridge = vi.hoisted(() => ({
  loadCoreResource: vi.fn(),
  mutateCoreResource: vi.fn(),
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
  capabilities: ['displays-v1', 'virtual-displays-v1'],
  apiScopes: ['display.read', 'display.manage'],
}

describe('DisplayManager', () => {
  beforeEach(() => {
    coreBridge.loadCoreResource.mockReset()
    coreBridge.mutateCoreResource.mockReset()
    useClientStore.setState({ resourcesByHost: {}, operationsByHost: {} })
  })

  it('resets the submitted form after an asynchronous mutation', async () => {
    let completeMutation: (() => void) | undefined
    coreBridge.mutateCoreResource.mockImplementation(
      () =>
        new Promise<void>((resolve) => {
          completeMutation = resolve
        }),
    )
    render(<DisplayManager host={host} apps={[]} settings={defaultSettings} onError={vi.fn()} />)
    const name = screen.getByLabelText('Display name')
    const form = name.closest('form') as HTMLFormElement
    const reset = vi.spyOn(form, 'reset')

    fireEvent.change(name, { target: { value: 'Desk' } })
    fireEvent.submit(form)
    await waitFor(() => expect(coreBridge.mutateCoreResource).toHaveBeenCalledOnce())
    completeMutation?.()

    await waitFor(() => expect(reset).toHaveBeenCalledOnce())
  })
})
