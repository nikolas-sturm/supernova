import { useThemeStore } from '@supernova/design-system/theme'
import { fireEvent, render, screen } from '@testing-library/react'
import { beforeEach, describe, expect, it } from 'vitest'
import { App } from './App'
import { defaultSettings, defaultSettingsByMode } from './settings'
import { useClientStore } from './store/clientStore'

describe('App', () => {
  beforeEach(() => {
    localStorage.clear()
    useThemeStore.getState().setTheme('dark')
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
      logicalSessionsByHost: {},
      telemetryByHost: {},
      resourcesByHost: {},
      operationsByHost: {},
      appMode: 'gaming',
      settingsByMode: {
        gaming: { ...defaultSettings },
        workstation: { ...defaultSettingsByMode.workstation },
      },
    })
  })

  it('shows preview state outside Neutralino', async () => {
    render(<App />)

    expect((await screen.findAllByText('Web preview')).length).toBeGreaterThan(0)
    expect(screen.getByText('Connect your first gaming rig')).toBeInTheDocument()
    expect(screen.getByText('TERRA')).toBeVisible()
    expect(screen.queryByText('ECLIPSE')).not.toBeInTheDocument()
    expect(screen.getByRole('option', { name: 'Terra / Dark' })).toHaveValue('dark')
  })

  it('switches shared themes without resetting client modes or stream settings', () => {
    render(<App />)
    const theme = screen.getByRole('combobox', { name: 'Theme' })
    expect(theme).toHaveValue('dark')
    fireEvent.change(theme, { target: { value: 'latte' } })
    expect(document.documentElement.dataset.theme).toBe('latte')
    expect(localStorage.getItem('theme')).toBe('latte')
    fireEvent.click(screen.getByRole('button', { name: 'Workstation' }))
    expect(theme).toHaveValue('latte')
    expect(useClientStore.getState().settingsByMode.gaming).toEqual(defaultSettings)
    fireEvent.change(theme, { target: { value: 'dracula' } })
    expect(document.documentElement.dataset.theme).toBe('dracula')
    expect(useClientStore.getState().appMode).toBe('workstation')
  })

  it('edits and persists the native stream profile', async () => {
    render(<App />)

    fireEvent.click(screen.getByRole('button', { name: 'Stream Settings' }))
    expect(screen.getByRole('heading', { name: 'Stream' })).toBeVisible()

    fireEvent.change(screen.getByLabelText('Resolution'), { target: { value: 'custom' } })
    fireEvent.change(screen.getByLabelText('Custom width'), { target: { value: '3440' } })
    fireEvent.blur(screen.getByLabelText('Custom width'))
    fireEvent.change(screen.getByLabelText('Custom height'), { target: { value: '1440' } })
    fireEvent.blur(screen.getByLabelText('Custom height'))
    fireEvent.change(screen.getByLabelText('Frame rate'), { target: { value: 'custom' } })
    fireEvent.change(screen.getByLabelText('Custom frame rate'), { target: { value: '75' } })
    fireEvent.blur(screen.getByLabelText('Custom frame rate'))
    fireEvent.change(screen.getByLabelText('Display mode'), {
      target: { value: 'borderless' },
    })
    fireEvent.change(screen.getByLabelText('Target display'), { target: { value: '1' } })
    fireEvent.change(screen.getByLabelText('Audio configuration'), { target: { value: '5.1' } })
    fireEvent.change(screen.getByLabelText('Capture system keyboard shortcuts'), {
      target: { value: 'fullscreen' },
    })
    fireEvent.click(screen.getByText('Mute host PC speakers while streaming'))
    fireEvent.click(screen.getByText('Quit app on host after ending stream'))
    fireEvent.click(screen.getByText('Automatically find PCs on local network'))
    fireEvent.click(screen.getByText('Automatically detect blocked connections'))
    fireEvent.click(screen.getByText('Show performance stats while streaming'))

    expect(useClientStore.getState().settingsByMode.gaming).toMatchObject({
      width: 3440,
      height: 1440,
      fps: 75,
      displayMode: 'borderless',
      displayIndex: 1,
      audioConfig: '5.1',
      captureSystemKeys: 'fullscreen',
      muteHostAudio: false,
      quitAppAfter: true,
      autoDiscoverHosts: true,
      detectBlockedConnections: true,
      showPerformanceStats: true,
    })
    expect(localStorage.getItem('eclipse-client-settings')).toContain('borderless')
    expect(screen.getByText('Frame pacing').closest('label')).toHaveClass(/settingDisabled/)
  })

  it('swaps complete app modes from synchronized controls', () => {
    render(<App />)

    expect(screen.getByRole('heading', { name: 'Game library' })).toBeVisible()
    expect(screen.getByRole('button', { name: /Game Library/ })).toHaveAttribute(
      'aria-current',
      'page',
    )
    expect(screen.queryByRole('button', { name: 'Workspaces & Desktops' })).not.toBeInTheDocument()

    fireEvent.click(screen.getAllByRole('button', { name: 'Workstation' })[0] as HTMLElement)

    expect(screen.getByRole('heading', { name: 'Workspaces & desktops' })).toBeVisible()
    expect(screen.getByRole('button', { name: 'Workspaces & Desktops' })).toHaveAttribute(
      'aria-current',
      'page',
    )
    expect(screen.queryByRole('button', { name: /Game Library/ })).not.toBeInTheDocument()
    expect(screen.getAllByRole('button', { name: 'Workstation' })).toHaveLength(1)
    for (const button of screen.getAllByRole('button', { name: 'Workstation' })) {
      expect(button).toHaveAttribute('aria-pressed', 'true')
    }
    expect(screen.getByRole('main').closest('[data-mode]')).toHaveAttribute(
      'data-mode',
      'workstation',
    )
  })

  it('retains navigation and settings independently between modes', () => {
    render(<App />)

    fireEvent.click(screen.getByRole('button', { name: 'Paired Hosts' }))
    fireEvent.click(screen.getAllByRole('button', { name: 'Workstation' })[0] as HTMLElement)
    fireEvent.click(screen.getByRole('button', { name: 'Workstation Settings' }))
    fireEvent.click(screen.getByText('Mute host PC speakers while streaming'))

    expect(useClientStore.getState().settingsByMode.workstation.muteHostAudio).toBe(false)
    expect(useClientStore.getState().settingsByMode.gaming.muteHostAudio).toBe(true)

    fireEvent.click(screen.getAllByRole('button', { name: 'Gaming' })[0] as HTMLElement)

    expect(screen.getByRole('heading', { name: 'Paired hosts' })).toBeVisible()
    expect(useClientStore.getState().appMode).toBe('gaming')
  })

  it('adds a manually configured host', async () => {
    render(<App />)

    fireEvent.click(screen.getByRole('button', { name: 'Add computer' }))
    fireEvent.change(screen.getByLabelText('Display name'), { target: { value: 'Studio PC' } })
    fireEvent.change(screen.getByLabelText('Host or IP address'), {
      target: { value: '192.168.1.40' },
    })
    fireEvent.click(screen.getByRole('button', { name: 'Save computer' }))

    expect(await screen.findByText('Studio PC')).toBeInTheDocument()
    expect(screen.getByText('192.168.1.40')).toBeInTheDocument()
    expect(screen.getByText('offline')).toBeInTheDocument()
  })

  it('shows the active Sol pairing PIN', () => {
    useClientStore.setState({
      appMode: 'workstation',
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
          serverCodecModeSupport: 1,
          maxLumaPixelsHevc: 0,
          displayModes: [],
          lastSeenAt: 1,
          paired: false,
        },
      ],
      pairing: {
        hostId: 'host-1',
        pin: '0427',
        state: 'pairing',
        message: "Enter this PIN in Sol's web interface.",
      },
    })

    render(<App />)

    expect(screen.getByRole('dialog', { name: 'Pair Studio PC' })).toBeInTheDocument()
    expect(screen.getByLabelText('Pairing PIN 0427')).toHaveTextContent('0427')
    expect(screen.getByText('WORKSTATION CONTROL')).toBeVisible()
    expect(screen.getByText("Enter this PIN in Sol's web interface.")).toBeVisible()
  })

  it('offers Wake-on-LAN for authenticated offline hosts', async () => {
    useClientStore.setState({
      hosts: [
        {
          id: 'host-1',
          name: 'Studio PC',
          address: '192.168.1.40',
          serverName: 'Rig',
          serverUniqueId: 'server-1',
          appVersion: '7.1.431.-1',
          serverState: '',
          status: 'offline',
          error: '',
          httpsPort: 47984,
          currentGameId: 0,
          serverCodecModeSupport: 1,
          maxLumaPixelsHevc: 0,
          displayModes: [],
          lastSeenAt: 1,
          paired: true,
          wakeable: true,
        },
      ],
    })

    render(<App />)

    expect(await screen.findByRole('button', { name: 'Wake-on-LAN' })).toBeEnabled()
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
          serverCodecModeSupport: 1,
          maxLumaPixelsHevc: 0,
          displayModes: [],
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
            description: 'Full remote desktop session.',
            publisher: 'Supernova',
            source: 'host',
            tags: ['desktop', 'productivity'],
            inputRequirements: ['keyboard', 'mouse'],
            launchProfiles: [
              {
                id: '11111111-1111-4111-8111-111111111111',
                name: 'Couch mode',
                default: false,
              },
            ],
            installed: true,
            updateAvailable: true,
          },
        ],
      },
    })

    render(<App />)

    expect(await screen.findByRole('heading', { name: 'Applications' })).toBeInTheDocument()
    expect(screen.getAllByText('Desktop')).toHaveLength(2)
    expect(screen.getByRole('button', { name: 'Launch Desktop' })).toBeEnabled()
    expect(screen.getByRole('textbox', { name: 'Filter library' })).toBeVisible()

    fireEvent.click(screen.getByRole('button', { name: 'games' }))
    expect(screen.getByText('No applications match this filter.')).toBeVisible()
    fireEvent.click(screen.getByRole('button', { name: 'apps' }))
    expect(screen.getByRole('button', { name: 'Launch Desktop' })).toBeEnabled()
    fireEvent.click(screen.getByRole('button', { name: 'Updates (1)' }))
    expect(screen.getByRole('button', { name: 'Launch Desktop' })).toBeEnabled()

    fireEvent.click(
      screen.getAllByRole('button', { name: 'View Desktop details' })[0] as HTMLElement,
    )
    expect(screen.getByRole('dialog', { name: 'Desktop' })).toBeVisible()
    expect(screen.getByText('Full remote desktop session.')).toBeVisible()
    expect(screen.getByText('Supernova')).toBeVisible()
    expect(screen.getByText('keyboard, mouse')).toBeVisible()
    const launchConfiguration = screen.getByLabelText('Launch configuration')
    fireEvent.change(launchConfiguration, {
      target: { value: '11111111-1111-4111-8111-111111111111' },
    })
    expect(launchConfiguration).toHaveValue('11111111-1111-4111-8111-111111111111')
    fireEvent.click(screen.getByRole('button', { name: 'Close application details' }))

    fireEvent.click(screen.getByRole('button', { name: 'Workstation' }))

    expect(await screen.findByText('SPATIAL ARRANGEMENT')).toBeVisible()
    expect(screen.getByText('Stream Canvas: 1920 × 1080 px')).toBeVisible()
    expect(screen.getByRole('button', { name: /Desktop OPEN REMOTELY/ })).toBeEnabled()
  })

  it('renders client display topology instead of host display management', async () => {
    useClientStore.setState({
      appMode: 'workstation',
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
          serverCodecModeSupport: 1,
          maxLumaPixelsHevc: 0,
          displayModes: [],
          lastSeenAt: 1,
          paired: true,
          apiVersion: 1,
          apiPort: 47984,
          capabilities: ['multi-display-streaming-v1'],
          apiScopes: ['display.read', 'display.manage'],
        },
      ],
      clientDisplays: [
        {
          id: 'display-1',
          name: 'Studio Display',
          primary: true,
          x: 0,
          y: 0,
          width: 2560,
          height: 1440,
          refreshRate: 60,
          modes: [{ width: 2560, height: 1440, refreshRate: 60 }],
        },
      ],
    })

    render(<App />)
    fireEvent.click(screen.getByRole('button', { name: 'Display & Topology' }))

    expect(await screen.findByRole('heading', { name: 'Streamed display topology' })).toBeVisible()
    expect(screen.getByRole('heading', { name: 'Studio Display' })).toBeVisible()
    expect(screen.getByText('2560 × 1440 / 60 Hz')).toBeVisible()
    expect(screen.getByLabelText(/Stream this display/)).toBeChecked()
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
          serverCodecModeSupport: 1,
          maxLumaPixelsHevc: 0,
          displayModes: [],
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

    expect(await screen.findByText('D3D11VA video and WASAPI audio playback active.')).toBeVisible()
    expect(screen.getByRole('button', { name: 'Disconnect' })).toBeEnabled()
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
          serverCodecModeSupport: 1,
          maxLumaPixelsHevc: 0,
          displayModes: [],
          lastSeenAt: 1,
          paired: true,
        },
      ],
      session: undefined,
    })

    render(<App />)

    expect(await screen.findByText('Running host application')).toBeVisible()
    expect(screen.getByRole('button', { name: 'Stop host app' })).toBeEnabled()
  })

  it('keeps host recovery available when a stop did not end the application', async () => {
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
          serverCodecModeSupport: 1,
          maxLumaPixelsHevc: 0,
          displayModes: [],
          lastSeenAt: 1,
          paired: true,
        },
      ],
      session: {
        hostId: 'host-1',
        appId: 2048,
        appName: 'Desktop',
        state: 'stopped',
        message: 'Host application stopped.',
        resumed: false,
      },
    })

    render(<App />)

    expect(await screen.findByText('Running host application')).toBeVisible()
    expect(screen.getByRole('button', { name: 'Stop host app' })).toBeEnabled()
  })
})
