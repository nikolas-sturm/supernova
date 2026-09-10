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
  LogicalSession,
  PairingUpdate,
  SessionUpdate,
  SolResourceUpdate,
  TelemetrySnapshot,
} from '../store/clientStore'
import {
  dismissStreamOverlay,
  openStreamOverlay,
  prewarmStreamOverlay,
  stopStreamOverlayPrewarm,
  updateStreamOverlayStatistics,
} from './overlayWindow'
import { hasNeutralinoRuntime, initializeNeutralinoRuntime } from './runtime'

const extensionId = 'dev.terra.core'
const statusEvent = 'terra.core.status'
const hostsEvent = 'terra.hosts.changed'
const hostErrorEvent = 'terra.host.error'
const pairingEvent = 'terra.pairing.changed'
const appsEvent = 'terra.apps.changed'
const artworkEvent = 'terra.app.art.changed'
const sessionEvent = 'terra.session.changed'
const streamOverlayEvent = 'terra.stream.overlay.requested'
const streamStatisticsEvent = 'terra.stream.statistics'
const solResourceEvent = 'terra.sol.resource.changed'

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
  lastSeenAt: z
    .number()
    .int()
    .transform((value) => Math.max(0, value)),
  paired: z.boolean(),
  wakeable: z.boolean().optional().default(false),
  apiVersion: z.number().int().nonnegative().optional().default(0),
  apiPort: z.number().int().min(0).max(65535).optional().default(0),
  capabilities: z.array(z.string()).optional().default([]),
  apiClientUuid: z.string().optional().default(''),
  apiClientName: z.string().optional().default(''),
  apiScopes: z.array(z.string()).optional().default([]),
  allowedApps: z.array(z.string()).optional().default([]),
  features: z.record(z.string(), z.unknown()).optional().default({}),
  limits: z.record(z.string(), z.unknown()).optional().default({}),
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
  uuid: z.string().optional().default(''),
  kind: z.string().optional().default('unknown'),
  description: z.string().optional().default(''),
  source: z.string().optional().default(''),
  publisher: z.string().optional().default(''),
  tags: z.array(z.string()).optional().default([]),
  inputRequirements: z.array(z.string()).optional().default([]),
  installed: z.boolean().optional().default(true),
  updateAvailable: z.boolean().optional().default(false),
  assetRevision: z.number().int().nonnegative().optional().default(0),
  displayProfileId: z.string().nullable().optional().default(null),
  streamProfileId: z.string().nullable().optional().default(null),
  sandboxProfileId: z.string().nullable().optional().default(null),
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

const logicalSessionSchema: z.ZodType<LogicalSession> = z.object({
  id: z.uuid(),
  appUuid: z.string(),
  legacyAppId: z.number().int().nonnegative(),
  state: z.enum(['preparing', 'starting', 'running', 'disconnected', 'stopped', 'failed']),
  stateReason: z.string(),
  startedAt: z.number().int(),
  updatedAt: z.number().int(),
  width: z.number().int().positive(),
  height: z.number().int().positive(),
  refreshRate: z.number().int().positive(),
  hdr: z.boolean(),
  revision: z.number().int().positive(),
})

const nullableMetric = z.number().nullable()
const telemetrySchema: z.ZodType<TelemetrySnapshot> = z.object({
  timestamp: z.number().int(),
  host: z.object({
    healthy: z.boolean(),
    captureHealthy: z.boolean().nullable(),
    captureFps: nullableMetric,
    encoderHealthy: z.boolean().nullable(),
    encoderCodec: z.string().nullable(),
    encoderLatencyMs: nullableMetric,
    audioCaptureHealthy: z.boolean().nullable(),
    activeLogicalSessions: z.number().int().nonnegative(),
    activeTransportSessions: z.number().int().nonnegative(),
  }),
  sessions: z.array(
    z.object({
      sessionId: z.uuid(),
      state: z.string(),
      codec: z.string().nullable(),
      captureFps: nullableMetric,
      encodeFps: nullableMetric,
      transmitFps: nullableMetric,
      encodeLatencyMs: nullableMetric,
      bitrateKbps: nullableMetric,
      droppedFrames: z.number().int().nonnegative().nullable(),
    }),
  ),
})

const solEventTypeSchema = z.enum([
  'host.changed',
  'capabilities.changed',
  'catalog.changed',
  'session.created',
  'session.updated',
  'session.removed',
  'displays.changed',
  'virtualDisplay.created',
  'virtualDisplay.updated',
  'virtualDisplay.removed',
  'telemetry.sample',
  'workspace.created',
  'workspace.updated',
  'workspace.removed',
  'profile.created',
  'profile.updated',
  'profile.removed',
  'peripheral.added',
  'peripheral.updated',
  'peripheral.removed',
  'sandbox.created',
  'sandbox.updated',
  'sandbox.removed',
  'operation.updated',
  'host.stopping',
  'resync.required',
])

const solResourceEnvelopeSchema = z.object({
  schemaVersion: z.literal(1),
  hostId: z.string().min(1),
  resource: z.string().min(1),
  payload: z.unknown(),
})

interface CoreBridgeHandlers {
  onStatus: (status: BridgeStatus) => void
  onHosts: (hosts: Host[]) => void
  onHostError: (message: string) => void
  onPairing: (update: PairingUpdate) => void
  onApps: (library: AppLibrary) => void
  onArtwork: (hostId: string, appId: number, dataUrl: string, error: string) => void
  onSession: (update: SessionUpdate) => void
  onSolResource: (update: SolResourceUpdate) => void
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
  onSolResource,
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
      detail: 'Native core exited. Restart Terra before sending host commands.',
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
    void openStreamOverlay(
      result.data,
      (action) =>
        action === 'resume'
          ? dispatchConnected('stream.overlay.close', {
              hostId: result.data.hostId,
              generation: result.data.generation,
              revision: result.data.revision,
            })
          : dispatchConnected('session.cancel', {
              hostId: result.data.hostId,
              quitHost: action === 'quit',
            }),
      (revision, visible) =>
        visible
          ? Promise.resolve()
          : dispatchConnected('stream.overlay.hidden', {
              hostId: result.data.hostId,
              generation: result.data.generation,
              revision,
            }),
    ).catch((error: unknown) => {
      onHostError(
        error instanceof Error ? error.message : 'Terra could not open the stream overlay.',
      )
    })
  }

  const handleStreamStatistics = (event: CustomEvent<unknown>) => {
    const result = streamStatisticsUpdateSchema.safeParse(event.detail)
    if (result.success) void updateStreamOverlayStatistics(result.data)
  }

  const handleSolResource = (event: CustomEvent<unknown>) => {
    const envelope = solResourceEnvelopeSchema.safeParse(event.detail)
    if (!envelope.success) {
      onHostError('Native core returned an invalid Sol API resource.')
      return
    }
    const { hostId, resource, payload } = envelope.data
    if (resource === 'sessions') {
      const result = z.object({ sessions: z.array(logicalSessionSchema) }).safeParse(payload)
      if (result.success) onSolResource({ hostId, resource, payload: result.data })
      else onHostError('Sol returned an invalid logical session collection.')
      return
    }
    if (resource === 'telemetry') {
      const result = telemetrySchema.safeParse(payload)
      if (result.success) onSolResource({ hostId, resource, payload: result.data })
      else onHostError('Sol returned invalid telemetry.')
      return
    }
    if (resource !== 'event') return
    const result = z.object({ type: solEventTypeSchema, data: z.unknown() }).safeParse(payload)
    if (!result.success) {
      onHostError('Sol returned an invalid event.')
      return
    }
    const eventPayload = result.data
    if (eventPayload.type === 'session.created' || eventPayload.type === 'session.updated') {
      const data = logicalSessionSchema.safeParse(eventPayload.data)
      if (data.success) {
        onSolResource({ hostId, resource, payload: { type: eventPayload.type, data: data.data } })
      }
      return
    }
    if (eventPayload.type === 'session.removed') {
      const data = z.object({ id: z.uuid() }).safeParse(eventPayload.data)
      if (data.success) {
        onSolResource({ hostId, resource, payload: { type: 'session.removed', data: data.data } })
      }
      return
    }
    if (eventPayload.type === 'telemetry.sample') {
      const data = telemetrySchema.safeParse(eventPayload.data)
      if (data.success) {
        onSolResource({ hostId, resource, payload: { type: 'telemetry.sample', data: data.data } })
      }
      return
    }
    onSolResource({ hostId, resource, payload: eventPayload } as SolResourceUpdate)
    if (eventPayload.type === 'catalog.changed') void dispatchConnected('host.apps', { id: hostId })
    if (eventPayload.type === 'resync.required') {
      const collections = z
        .object({ collections: z.array(z.string()) })
        .safeParse(eventPayload.data)
      if (collections.success) {
        for (const collection of collections.data.collections) {
          if (collection === 'sessions' || collection === 'telemetry') {
            void dispatchConnected('host.resource', { hostId, resource: collection })
          } else if (collection === 'apps') {
            void dispatchConnected('host.apps', { id: hostId })
          }
        }
      }
    }
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
  void events.on(solResourceEvent, handleSolResource)
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
    void events.off(solResourceEvent, handleSolResource)
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

export async function pairCoreHost(
  id: string,
  pin: string,
  access: 'gaming' | 'workstation' = 'gaming',
) {
  if (!hasNeutralinoRuntime()) return
  await dispatchConnected('host.pair', { id, pin, access })
}

export async function loadCoreApps(id: string) {
  if (!hasNeutralinoRuntime()) return
  await dispatchConnected('host.apps', { id })
}

export async function loadCoreResource(hostId: string, resource: 'sessions' | 'telemetry') {
  if (!hasNeutralinoRuntime()) return
  await dispatchConnected('host.resource', { hostId, resource })
}

export async function controlCoreLogicalSession(
  hostId: string,
  sessionId: string,
  action: 'disconnect' | 'stop',
) {
  if (!hasNeutralinoRuntime()) return
  await dispatchConnected('host.resource.mutate', {
    hostId,
    method: 'POST',
    path: `/eclipse/v1/sessions/${z.uuid().parse(sessionId)}/${action}`,
    body: {},
    idempotent: true,
  })
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
