import { fireEvent, render, screen } from '@testing-library/react'
import { beforeEach, describe, expect, it } from 'vitest'
import { App } from './App'
import { defaultSettings } from './settings'
import { useClientStore } from './store/clientStore'

describe('App', () => {
  beforeEach(() => {
    localStorage.clear()
    useClientStore.setState({
      bridge: {
        state: 'connecting',
        label: 'Connecting',
        detail: 'Waiting for native core extension.',
      },
      hosts: [],
      hostError: undefined,
      pairing: undefined,
      library: { hostId: '', state: 'idle', apps: [], message: '' },
      session: undefined,
      settings: defaultSettings,
    })
  })

  it('shows preview state outside Neutralino', async () => {
    render(<App />)

    expect((await screen.findAllByText('Web preview')).length).toBeGreaterThan(0)
    expect(screen.getByText('No computers added')).toBeInTheDocument()
  })

  it('edits and persists the native stream profile', async () => {
    render(<App />)

    fireEvent.click(screen.getByRole('button', { name: 'Settings' }))
    expect(screen.getByRole('heading', { name: 'Stream' })).toBeVisible()

    fireEvent.change(screen.getByLabelText('Resolution'), { target: { value: '3840x2160' } })
    fireEvent.change(screen.getByLabelText('Display mode'), {
      target: { value: 'borderless' },
    })
    fireEvent.click(screen.getByText('Mute host PC speakers while streaming'))

    expect(useClientStore.getState().settings).toMatchObject({
      width: 3840,
      height: 2160,
      bitrateKbps: 80_000,
      displayMode: 'borderless',
      muteHostAudio: false,
    })
    expect(localStorage.getItem('eclipse-client-settings')).toContain('borderless')
    expect(screen.getByText('Frame pacing').closest('label')).toHaveClass(/settingDisabled/)
  })

  it('adds a manually configured host', async () => {
    render(<App />)

    fireEvent.click(screen.getByRole('button', { name: 'Add first computer' }))
    fireEvent.change(screen.getByLabelText('Display name'), { target: { value: 'Studio PC' } })
    fireEvent.change(screen.getByLabelText('Host or IP address'), {
      target: { value: '192.168.1.40' },
    })
    fireEvent.click(screen.getByRole('button', { name: 'Save computer' }))

    expect(await screen.findByText('Studio PC')).toBeInTheDocument()
    expect(screen.getByText('192.168.1.40')).toBeInTheDocument()
    expect(screen.getByText('offline')).toBeInTheDocument()
  })

  it('shows the active Sunshine pairing PIN', () => {
    useClientStore.setState({
      hosts: [
        {
          id: 'host-1',
          name: 'Studio PC',
          address: '192.168.1.40',
          serverName: 'Rig',
          serverUniqueId: 'server-1',
          appVersion: '7.1.431.-1',
          serverState: 'IDLE',
          status: 'online',
          error: '',
          httpsPort: 47984,
          currentGameId: 0,
          lastSeenAt: 1,
          paired: false,
        },
      ],
      pairing: {
        hostId: 'host-1',
        pin: '0427',
        state: 'pairing',
        message: "Enter this PIN in Sunshine's web interface.",
      },
    })

    render(<App />)

    expect(screen.getByRole('dialog', { name: 'Pair Studio PC' })).toBeInTheDocument()
    expect(screen.getByLabelText('Pairing PIN 0427')).toHaveTextContent('0427')
  })

  it('opens a paired application library with launch controls', async () => {
    useClientStore.setState({
      hosts: [
        {
          id: 'host-1',
          name: 'Studio PC',
          address: '192.168.1.40',
          serverName: 'Rig',
          serverUniqueId: 'server-1',
          appVersion: '7.1.431.-1',
          serverState: 'SUNSHINE_SERVER_IDLE',
          status: 'online',
          error: '',
          httpsPort: 47984,
          currentGameId: 0,
          lastSeenAt: 1,
          paired: true,
        },
      ],
      library: {
        hostId: 'host-1',
        state: 'ready',
        message: '',
        apps: [
          {
            id: 2048,
            name: 'Desktop',
            hdrSupported: true,
            appCollectorGame: false,
          },
        ],
      },
    })

    render(<App />)
    fireEvent.click(screen.getByRole('button', { name: 'Browse games' }))

    expect(await screen.findByRole('heading', { name: 'Applications' })).toBeInTheDocument()
    expect(screen.getByText('Desktop')).toBeInTheDocument()
    expect(screen.getByRole('button', { name: 'Launch Desktop' })).toBeEnabled()
  })

  it('reports native rendering and exposes session stop', async () => {
    useClientStore.setState({
      hosts: [
        {
          id: 'host-1',
          name: 'Studio PC',
          address: '192.168.1.40',
          serverName: 'Rig',
          serverUniqueId: 'server-1',
          appVersion: '7.1.431.-1',
          serverState: 'SUNSHINE_SERVER_BUSY',
          status: 'online',
          error: '',
          httpsPort: 47984,
          currentGameId: 2048,
          lastSeenAt: 1,
          paired: true,
        },
      ],
      library: {
        hostId: 'host-1',
        state: 'ready',
        message: '',
        apps: [
          {
            id: 2048,
            name: 'Desktop',
            hdrSupported: false,
            appCollectorGame: false,
          },
        ],
      },
      session: {
        hostId: 'host-1',
        appId: 2048,
        appName: 'Desktop',
        state: 'rendering',
        message: 'D3D11VA video and WASAPI audio playback active.',
        resumed: false,
      },
    })

    render(<App />)
    fireEvent.click(screen.getByRole('button', { name: 'Browse games' }))

    expect(await screen.findByText('D3D11VA video and WASAPI audio playback active.')).toBeVisible()
    expect(screen.getByRole('button', { name: 'Stop host app' })).toBeEnabled()
    expect(screen.getByRole('button', { name: 'Resume Desktop' })).toBeDisabled()
  })

  it('allows recovery of an orphaned host application', async () => {
    useClientStore.setState({
      hosts: [
        {
          id: 'host-1',
          name: 'Studio PC',
          address: '192.168.1.40',
          serverName: 'Rig',
          serverUniqueId: 'server-1',
          appVersion: '7.1.431.-1',
          serverState: 'SUNSHINE_SERVER_BUSY',
          status: 'online',
          error: '',
          httpsPort: 47984,
          currentGameId: 2048,
          lastSeenAt: 1,
          paired: true,
        },
      ],
      session: undefined,
    })

    render(<App />)
    fireEvent.click(screen.getByRole('button', { name: 'Browse games' }))

    expect(await screen.findByText('Running Sunshine application')).toBeVisible()
    expect(screen.getByRole('button', { name: 'Stop host app' })).toBeEnabled()
  })
})
