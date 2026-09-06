import { app, events, extensions, window as neutralinoWindow } from '@neutralinojs/lib'
import { z } from 'zod'
import {
  streamOverlayRequestSchema,
  streamStatisticsUpdateSchema,
} from '../overlay/overlayProtocol'
import { type StreamSettings, settingsSchema } from '../settings'
import type {
  AppLibrary,
  BridgeStatus,
  Host,
  PairingUpdate,
  SessionUpdate,
} from '../store/clientStore'
import {
  dismissStreamOverlay,
  openStreamOverlay,
  prewarmStreamOverlay,
  stopStreamOverlayPrewarm,
  updateStreamOverlayStatistics,
} from './overlayWindow'
import { hasNeutralinoRuntime, initializeNeutralinoRuntime } from './runtime'

const extensionId = 'dev.eclipse.core'
const statusEvent = 'eclipse.core.status'
const hostsEvent = 'eclipse.hosts.changed'
const hostErrorEvent = 'eclipse.host.error'
const pairingEvent = 'eclipse.pairing.changed'
const appsEvent = 'eclipse.apps.changed'
const artworkEvent = 'eclipse.app.art.changed'
const sessionEvent = 'eclipse.session.changed'
const streamOverlayEvent = 'eclipse.stream.overlay.requested'
const streamStatisticsEvent = 'eclipse.stream.statistics'

const statusSchema = z.object({
  schemaVersion: z.literal(1),
  state: z.enum(['ready', 'error']),
  detail: z.string(),
  moonlightQtRevision: z.string(),
  moonlightCommonRevision: z.string(),
  streamingAvailable: z.boolean(),
})

const hostSchema = z.object({
  id: z.string().min(1),
  name: z.string().min(1),
  address: z.string().min(1),
  serverName: z.string(),
  serverUniqueId: z.string(),
  appVersion: z.string(),
  serverState: z.string(),
  status: z.enum(['probing', 'pairing', 'online', 'offline']),
  error: z.string(),
  httpsPort: z.number().int().min(0).max(65535),
  currentGameId: z.number().int().nonnegative(),
  serverCodecModeSupport: z.number().int().nonnegative().optional().default(1),
  maxLumaPixelsHevc: z.number().int().nonnegative().optional().default(0),
  displayModes: z
    .array(
      z.object({
        width: z.number().int().positive(),
        height: z.number().int().positive(),
        refreshRate: z.number().int().positive(),
      }),
    )
    .optional()
    .default([]),
  lastSeenAt: z.number().int().nonnegative(),
  paired: z.boolean(),
  wakeable: z.boolean().optional().default(false),
})

const hostsSchema = z.object({
  schemaVersion: z.literal(1),
  hosts: z.array(hostSchema),
})

const hostErrorSchema = z.object({
  schemaVersion: z.literal(1),
  message: z.string().min(1),
})

const pairingSchema = z.object({
  schemaVersion: z.literal(1),
  hostId: z.string().min(1),
  state: z.enum(['pairing', 'paired', 'error']),
  message: z.string(),
})

const appSchema = z.object({
  id: z.number().int().positive(),
  name: z.string(),
  hdrSupported: z.boolean(),
  appCollectorGame: z.boolean(),
})

const appsSchema = z.object({
  schemaVersion: z.literal(1),
  hostId: z.string().min(1),
  state: z.enum(['loading', 'ready', 'error']),
  apps: z.array(appSchema),
  message: z.string(),
})

const artworkSchema = z.object({
  schemaVersion: z.literal(1),
  hostId: z.string().min(1),
  appId: z.number().int().positive(),
  dataUrl: z.string(),
  error: z.string(),
})

const sessionSchema = z.object({
  schemaVersion: z.literal(1),
  hostId: z.string().min(1),
  appId: z.number().int().nonnegative(),
  appName: z.string(),
  state: z.enum([
    'launching',
    'connecting',
    'connected',
    'receiving',
    'rendering',
    'stopping',
    'stopped',
    'terminated',
    'error',
  ]),
  message: z.string(),
  resumed: z.boolean(),
})

interface CoreBridgeHandlers {
  onStatus: (status: BridgeStatus) => void
  onHosts: (hosts: Host[]) => void
  onHostError: (message: string) => void
  onPairing: (update: PairingUpdate) => void
  onApps: (library: AppLibrary) => void
  onArtwork: (hostId: string, appId: number, dataUrl: string, error: string) => void
  onSession: (update: SessionUpdate) => void
}

async function dispatchConnected(event: string, data?: unknown) {
  const stats = await extensions.getStats()
  if (!stats.connected.includes(extensionId)) {
    throw new Error('Native core extension is disconnected.')
  }

  let timeoutId: ReturnType<typeof setTimeout> | undefined
  try {
    await Promise.race([
      extensions.dispatch(extensionId, event, data),
      new Promise<never>((_, reject) => {
        timeoutId = setTimeout(() => reject(new Error('Native core request timed out.')), 3000)
      }),
    ])
  } finally {
    if (timeoutId) clearTimeout(timeoutId)
  }
}

let initialized = false

export function startCoreBridge({
  onStatus,
  onHosts,
  onHostError,
  onPairing,
  onApps,
  onArtwork,
  onSession,
}: CoreBridgeHandlers) {
  if (!hasNeutralinoRuntime()) {
    onStatus({
      state: 'preview',
      label: 'Web preview',
      detail: 'Run through Neutralino to connect native core.',
    })
    return () => undefined
  }

  if (!initialized) {
    initializeNeutralinoRuntime()
    void events.on('windowClose', () => {
      void dismissStreamOverlay().finally(() =>
        stopStreamOverlayPrewarm().finally(() => app.exit()),
      )
    })
    initialized = true
  }

  const handleStatus = (event: CustomEvent<unknown>) => {
    const result = statusSchema.safeParse(event.detail)

    if (!result.success) {
      onStatus({
        state: 'error',
        label: 'Protocol error',
        detail: 'Native core returned an invalid status payload.',
      })
      return
    }

    onStatus({
      state: result.data.state,
      label: result.data.state === 'ready' ? 'Core ready' : 'Core error',
      detail: result.data.detail,
      moonlightCommonRevision: result.data.moonlightCommonRevision,
      moonlightQtRevision: result.data.moonlightQtRevision,
      streamingAvailable: result.data.streamingAvailable,
    })
  }

  const handleHosts = (event: CustomEvent<unknown>) => {
    const result = hostsSchema.safeParse(event.detail)
    if (result.success) {
      onHosts(result.data.hosts)
    } else {
      onHostError('Native core returned an invalid host list.')
    }
  }

  const handleHostError = (event: CustomEvent<unknown>) => {
    const result = hostErrorSchema.safeParse(event.detail)
    onHostError(result.success ? result.data.message : 'Native host request failed.')
  }

  const handleExtensionDisconnect = (event: CustomEvent<unknown>) => {
    if (event.detail !== extensionId) return
    onStatus({
      state: 'error',
      label: 'Core disconnected',
      detail: 'Native core exited. Restart Eclipse before sending host commands.',
    })
  }

  const handlePairing = (event: CustomEvent<unknown>) => {
    const result = pairingSchema.safeParse(event.detail)
    if (result.success) {
      onPairing(result.data)
    } else {
      onHostError('Native core returned an invalid pairing update.')
    }
  }

  const handleApps = (event: CustomEvent<unknown>) => {
    const result = appsSchema.safeParse(event.detail)
    if (result.success) {
      onApps({ ...result.data, state: result.data.state })
    } else {
      onHostError('Native core returned an invalid application list.')
    }
  }

  const handleArtwork = (event: CustomEvent<unknown>) => {
    const result = artworkSchema.safeParse(event.detail)
    if (result.success) {
      onArtwork(result.data.hostId, result.data.appId, result.data.dataUrl, result.data.error)
    }
  }

  const handleSession = (event: CustomEvent<unknown>) => {
    const result = sessionSchema.safeParse(event.detail)
    if (result.success) {
      onSession(result.data)
      if (['stopped', 'terminated', 'error'].includes(result.data.state)) {
        void dismissStreamOverlay()
        void stopStreamOverlayPrewarm()
      } else if (window.NL_OS === 'Linux') {
        void prewarmStreamOverlay()
      }
    } else {
      onHostError('Native core returned an invalid session update.')
    }
  }

  const handleStreamOverlay = (event: CustomEvent<unknown>) => {
    const result = streamOverlayRequestSchema.safeParse(event.detail)
    if (!result.success) {
      onHostError('Native core returned invalid stream overlay bounds.')
      return
    }
    void openStreamOverlay(result.data, (action) =>
      action === 'resume'
        ? dispatchConnected('stream.overlay.closed', {
            hostId: result.data.hostId,
            generation: result.data.generation,
          })
        : dispatchConnected('session.cancel', {
            hostId: result.data.hostId,
            quitHost: action === 'quit',
          }),
    ).catch((error: unknown) => {
      onHostError(
        error instanceof Error ? error.message : 'Eclipse could not open the stream overlay.',
      )
    })
  }

  const handleStreamStatistics = (event: CustomEvent<unknown>) => {
    const result = streamStatisticsUpdateSchema.safeParse(event.detail)
    if (result.success) void updateStreamOverlayStatistics(result.data)
  }

  void events.on(statusEvent, handleStatus)
  void events.on(hostsEvent, handleHosts)
  void events.on(hostErrorEvent, handleHostError)
  void events.on(pairingEvent, handlePairing)
  void events.on(appsEvent, handleApps)
  void events.on(artworkEvent, handleArtwork)
  void events.on(sessionEvent, handleSession)
  void events.on(streamOverlayEvent, handleStreamOverlay)
  void events.on(streamStatisticsEvent, handleStreamStatistics)
  void events.on('extClientDisconnect', handleExtensionDisconnect)
  void extensions.dispatch(extensionId, 'core.status').catch(() => {
    onStatus({
      state: 'error',
      label: 'Core unavailable',
      detail: 'Native core extension did not accept the status request.',
    })
  })
  void extensions.dispatch(extensionId, 'hosts.list')

  return () => {
    void events.off(statusEvent, handleStatus)
    void events.off(hostsEvent, handleHosts)
    void events.off(hostErrorEvent, handleHostError)
    void events.off(pairingEvent, handlePairing)
    void events.off(appsEvent, handleApps)
    void events.off(artworkEvent, handleArtwork)
    void events.off(sessionEvent, handleSession)
    void events.off(streamOverlayEvent, handleStreamOverlay)
    void events.off(streamStatisticsEvent, handleStreamStatistics)
    void events.off('extClientDisconnect', handleExtensionDisconnect)
  }
}

export async function saveCoreHost(name: string, address: string) {
  if (!hasNeutralinoRuntime()) return false
  await dispatchConnected('host.add', { name, address })
  return true
}

export async function configureCoreDiscovery(enabled: boolean) {
  if (!hasNeutralinoRuntime()) return
  await dispatchConnected('discovery.configure', { enabled })
}

export async function refreshCoreHost(id: string) {
  if (!hasNeutralinoRuntime()) return
  await dispatchConnected('host.refresh', { id })
}

export async function wakeCoreHost(id: string) {
  if (!hasNeutralinoRuntime()) return
  await dispatchConnected('host.wake', { id })
}

export async function pairCoreHost(id: string, pin: string) {
  if (!hasNeutralinoRuntime()) return
  await dispatchConnected('host.pair', { id, pin })
}

export async function loadCoreApps(id: string) {
  if (!hasNeutralinoRuntime()) return
  await dispatchConnected('host.apps', { id })
}

export async function launchCoreApp(hostId: string, appId: number, settings: StreamSettings) {
  if (!hasNeutralinoRuntime()) return
  await dispatchConnected('app.launch', { hostId, appId, settings: settingsSchema.parse(settings) })
}

export async function applyUiDisplayMode(mode: StreamSettings['uiDisplayMode']) {
  if (!hasNeutralinoRuntime()) return
  if (mode === 'fullscreen') {
    await neutralinoWindow.setFullScreen()
    return
  }
  if (await neutralinoWindow.isFullScreen()) await neutralinoWindow.exitFullScreen()
  if (mode === 'maximized') {
    await neutralinoWindow.maximize()
  } else if (await neutralinoWindow.isMaximized()) {
    await neutralinoWindow.unmaximize()
  }
}

export async function cancelCoreSession(hostId: string, quitHost: boolean) {
  if (!hasNeutralinoRuntime()) return
  await dispatchConnected('session.cancel', { hostId, quitHost })
}

export async function removeCoreHost(id: string) {
  if (!hasNeutralinoRuntime()) return
  await dispatchConnected('host.remove', { id })
}
