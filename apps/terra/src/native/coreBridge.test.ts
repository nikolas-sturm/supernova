import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'

const neutralino = vi.hoisted(() => {
  const eventHandlers = new Map<string, (event: CustomEvent<unknown>) => void>()
  return {
    eventHandlers,
    dispatch: vi.fn(() => Promise.resolve()),
    eventsOff: vi.fn((name: string) => {
      eventHandlers.delete(name)
      return Promise.resolve()
    }),
    eventsOn: vi.fn((name: string, handler: (event: CustomEvent<unknown>) => void) => {
      eventHandlers.set(name, handler)
      return Promise.resolve()
    }),
    getStats: vi.fn(() => Promise.resolve({ connected: ['dev.terra.core'] })),
    init: vi.fn(),
  }
})

vi.mock('@neutralinojs/lib', () => ({
  app: { exit: vi.fn() },
  events: { off: neutralino.eventsOff, on: neutralino.eventsOn },
  extensions: { dispatch: neutralino.dispatch, getStats: neutralino.getStats },
  init: neutralino.init,
  window: {},
}))

vi.mock('./overlayWindow', () => ({
  dismissStreamOverlay: vi.fn(() => Promise.resolve()),
  openStreamOverlay: vi.fn(() => Promise.resolve()),
  prewarmStreamOverlay: vi.fn(() => Promise.resolve()),
  stopStreamOverlayPrewarm: vi.fn(() => Promise.resolve()),
  updateStreamOverlayStatistics: vi.fn(() => Promise.resolve()),
}))

import { defaultSettings } from '../settings'
import { launchCoreApp, pairCoreHost, startCoreBridge } from './coreBridge'

describe('coreBridge host protocol', () => {
  beforeEach(() => {
    Object.defineProperty(window, 'NL_OS', { configurable: true, value: 'Linux' })
  })

  afterEach(() => {
    neutralino.eventHandlers.clear()
    vi.clearAllMocks()
    Reflect.deleteProperty(window, 'NL_OS')
  })

  it('normalizes legacy negative host timestamps without rejecting the host list', () => {
    const onHosts = vi.fn()
    const onHostError = vi.fn()
    const stop = startCoreBridge({
      onStatus: vi.fn(),
      onHosts,
      onHostError,
      onPairing: vi.fn(),
      onApps: vi.fn(),
      onArtwork: vi.fn(),
      onSession: vi.fn(),
      onSolResource: vi.fn(),
    })

    neutralino.eventHandlers.get('terra.hosts.changed')?.(
      new CustomEvent('terra.hosts.changed', {
        detail: {
          schemaVersion: 1,
          hosts: [
            {
              id: 'host-1',
              name: 'RIG',
              address: 'rig',
              serverName: 'Rig',
              serverUniqueId: 'server-1',
              appVersion: '7.1.431.-1',
              serverState: 'SUNSHINE_SERVER_FREE',
              status: 'offline',
              error: 'Cancelled',
              httpsPort: 47984,
              currentGameId: 0,
              serverCodecModeSupport: 2_032_385,
              maxLumaPixelsHevc: 1_869_449_984,
              displayModes: [],
              lastSeenAt: -2_126_954_256,
              paired: false,
              wakeable: false,
            },
          ],
        },
      }),
    )

    expect(onHosts).toHaveBeenCalledWith([expect.objectContaining({ id: 'host-1', lastSeenAt: 0 })])
    expect(onHostError).not.toHaveBeenCalled()
    stop()
  })

  it('validates and forwards logical session events', () => {
    const onSolResource = vi.fn()
    const stop = startCoreBridge({
      onStatus: vi.fn(),
      onHosts: vi.fn(),
      onHostError: vi.fn(),
      onPairing: vi.fn(),
      onApps: vi.fn(),
      onArtwork: vi.fn(),
      onSession: vi.fn(),
      onSolResource,
    })
    const logicalSession = {
      id: '11111111-1111-4111-8111-111111111111',
      appUuid: '22222222-2222-4222-8222-222222222222',
      legacyAppId: 7,
      state: 'running',
      stateReason: 'streaming',
      startedAt: 1,
      updatedAt: 2,
      width: 2560,
      height: 1440,
      refreshRate: 120,
      hdr: true,
      displayId: '33333333-3333-4333-8333-333333333333',
      displayIds: ['33333333-3333-4333-8333-333333333333'],
      streams: [
        {
          id: '44444444-4444-4444-8444-444444444444',
          displayId: '33333333-3333-4333-8333-333333333333',
          primary: true,
          state: 'running',
          width: 2560,
          height: 1440,
          fps: 120,
          hdr: true,
        },
      ],
      revision: 3,
    }

    neutralino.eventHandlers.get('terra.sol.resource.changed')?.(
      new CustomEvent('terra.sol.resource.changed', {
        detail: {
          schemaVersion: 1,
          hostId: 'host-1',
          resource: 'event',
          payload: {
            schemaVersion: 1,
            id: 4,
            type: 'session.updated',
            timestamp: 5,
            data: logicalSession,
          },
        },
      }),
    )

    expect(onSolResource).toHaveBeenCalledWith({
      hostId: 'host-1',
      resource: 'event',
      payload: { type: 'session.updated', data: logicalSession },
    })
    stop()
  })

  it('validates Catalog V2 launch profiles', () => {
    const onApps = vi.fn()
    const stop = startCoreBridge({
      onStatus: vi.fn(),
      onHosts: vi.fn(),
      onHostError: vi.fn(),
      onPairing: vi.fn(),
      onApps,
      onArtwork: vi.fn(),
      onSession: vi.fn(),
      onSolResource: vi.fn(),
    })
    const launchProfile = {
      id: '11111111-1111-4111-8111-111111111111',
      name: 'Couch mode',
      default: true,
    }

    neutralino.eventHandlers.get('terra.apps.changed')?.(
      new CustomEvent('terra.apps.changed', {
        detail: {
          schemaVersion: 1,
          hostId: 'host-1',
          state: 'ready',
          apps: [
            {
              id: 7,
              name: 'Game',
              hdrSupported: true,
              appCollectorGame: true,
              launchProfiles: [launchProfile],
            },
          ],
          message: '',
        },
      }),
    )

    expect(onApps).toHaveBeenCalledWith(
      expect.objectContaining({
        hostId: 'host-1',
        apps: [expect.objectContaining({ id: 7, launchProfiles: [launchProfile] })],
      }),
    )
    stop()
  })

  it('validates workstation collections and refreshes them after resource events', async () => {
    const onSolResource = vi.fn()
    const onHostError = vi.fn()
    const stop = startCoreBridge({
      onStatus: vi.fn(),
      onHosts: vi.fn(),
      onHostError,
      onPairing: vi.fn(),
      onApps: vi.fn(),
      onArtwork: vi.fn(),
      onSession: vi.fn(),
      onSolResource,
    })
    const display = {
      id: '11111111-1111-4111-8111-111111111111',
      name: 'Studio Display',
      kind: 'physical',
      enabled: true,
      primary: true,
      position: { x: 0, y: 0 },
      currentMode: {
        id: '2560x1440@60',
        width: 2560,
        height: 1440,
        refreshNumerator: 60,
        refreshDenominator: 1,
        bitDepth: 10,
        hdr: true,
      },
      supportedModes: [],
      hdr: { supported: true, enabled: true },
      captureEligible: true,
      revision: 3,
    }

    neutralino.eventHandlers.get('terra.sol.resource.changed')?.(
      new CustomEvent('terra.sol.resource.changed', {
        detail: {
          schemaVersion: 1,
          hostId: 'host-1',
          resource: 'displays',
          payload: { schemaVersion: 1, revision: 3, displays: [display] },
        },
      }),
    )

    expect(onSolResource).toHaveBeenCalledWith({
      hostId: 'host-1',
      resource: 'displays',
      payload: { revision: 3, displays: [display] },
    })
    expect(onHostError).not.toHaveBeenCalled()

    neutralino.eventHandlers.get('terra.sol.resource.changed')?.(
      new CustomEvent('terra.sol.resource.changed', {
        detail: {
          schemaVersion: 1,
          hostId: 'host-1',
          resource: 'event',
          payload: { type: 'displays.changed', data: { revision: 4 } },
        },
      }),
    )
    await vi.waitFor(() =>
      expect(neutralino.dispatch).toHaveBeenCalledWith('dev.terra.core', 'host.resource', {
        hostId: 'host-1',
        resource: 'displays',
      }),
    )
    expect(neutralino.dispatch).toHaveBeenCalledWith('dev.terra.core', 'host.resource', {
      hostId: 'host-1',
      resource: 'display-topology',
    })
    stop()
  })

  it('requests workstation access while pairing', async () => {
    await pairCoreHost('host-1', '0427', 'workstation')

    expect(neutralino.dispatch).toHaveBeenCalledWith('dev.terra.core', 'host.pair', {
      id: 'host-1',
      pin: '0427',
      access: 'workstation',
    })
  })

  it('passes an explicit Sol launch profile to native core', async () => {
    await launchCoreApp('host-1', 7, defaultSettings, '11111111-1111-4111-8111-111111111111')

    expect(neutralino.dispatch).toHaveBeenCalledWith(
      'dev.terra.core',
      'app.launch',
      expect.objectContaining({
        hostId: 'host-1',
        appId: 7,
        launchProfileId: '11111111-1111-4111-8111-111111111111',
      }),
    )
  })

  it('passes a workspace to native core', async () => {
    await launchCoreApp('host-1', 7, defaultSettings, '', '55555555-5555-4555-8555-555555555555')

    expect(neutralino.dispatch).toHaveBeenCalledWith(
      'dev.terra.core',
      'app.launch',
      expect.objectContaining({ workspaceId: '55555555-5555-4555-8555-555555555555' }),
    )
  })

  it('passes client display topology to native core', async () => {
    const virtualDisplays = [
      {
        name: 'Primary',
        mode: {
          width: 2560,
          height: 1440,
          refreshNumerator: 200,
          refreshDenominator: 1,
          bitDepth: 8,
          hdr: false,
        },
        position: { x: 0, y: 0 },
        scale: 1,
        rotation: 0,
        primary: true,
        hdr: false,
        persistent: false,
      },
    ]

    await launchCoreApp('host-1', 7, defaultSettings, '', '', virtualDisplays)

    expect(neutralino.dispatch).toHaveBeenCalledWith(
      'dev.terra.core',
      'app.launch',
      expect.objectContaining({ virtualDisplays }),
    )
  })
})
