import { create } from 'zustand'
import { createJSONStorage, persist } from 'zustand/middleware'
import {
  type AppMode,
  appModeSchema,
  defaultSettings,
  defaultSettingsByMode,
  type StreamSettings,
  settingsSchema,
} from '../settings'
import { settingsStorage } from './settingsStorage'

export type BridgeState = 'connecting' | 'ready' | 'preview' | 'error'

export interface BridgeStatus {
  state: BridgeState
  label: string
  detail: string
  moonlightCommonRevision?: string
  moonlightQtRevision?: string
  streamingAvailable?: boolean
}

export interface Host {
  id: string
  name: string
  address: string
  serverName: string
  serverUniqueId: string
  appVersion: string
  serverState: string
  status: 'probing' | 'pairing' | 'online' | 'offline'
  error: string
  httpsPort: number
  currentGameId: number
  serverCodecModeSupport: number
  maxLumaPixelsHevc: number
  displayModes: Array<{ width: number; height: number; refreshRate: number }>
  lastSeenAt: number
  paired: boolean
  wakeable?: boolean
  apiVersion?: number
  apiPort?: number
  capabilities?: string[]
  apiClientUuid?: string
  apiClientName?: string
  apiScopes?: string[]
  allowedApps?: string[]
  features?: Record<string, unknown>
  limits?: Record<string, unknown>
}

export interface PairingUpdate {
  hostId: string
  state: 'pairing' | 'paired' | 'error'
  message: string
}

export interface PairingState extends PairingUpdate {
  pin: string
}

export interface GameApp {
  id: number
  name: string
  hdrSupported: boolean
  appCollectorGame: boolean
  uuid?: string
  kind?: string
  description?: string
  source?: string
  publisher?: string
  tags?: string[]
  inputRequirements?: string[]
  installed?: boolean
  updateAvailable?: boolean
  assetRevision?: number
  displayProfileId?: string | null
  streamProfileId?: string | null
  sandboxProfileId?: string | null
  artDataUrl?: string
  artError?: string
}

export interface AppLibrary {
  hostId: string
  state: 'idle' | 'loading' | 'ready' | 'error'
  apps: GameApp[]
  message: string
}

export interface SessionUpdate {
  hostId: string
  appId: number
  appName: string
  state:
    | 'launching'
    | 'connecting'
    | 'connected'
    | 'receiving'
    | 'rendering'
    | 'stopping'
    | 'stopped'
    | 'terminated'
    | 'error'
  message: string
  resumed: boolean
}

export interface LogicalSession {
  id: string
  appUuid: string
  legacyAppId: number
  state: 'preparing' | 'starting' | 'running' | 'disconnected' | 'stopped' | 'failed'
  stateReason: string
  startedAt: number
  updatedAt: number
  width: number
  height: number
  refreshRate: number
  hdr: boolean
  revision: number
}

export interface TelemetrySnapshot {
  timestamp: number
  host: {
    healthy: boolean
    captureHealthy: boolean | null
    captureFps: number | null
    encoderHealthy: boolean | null
    encoderCodec: string | null
    encoderLatencyMs: number | null
    audioCaptureHealthy: boolean | null
    activeLogicalSessions: number
    activeTransportSessions: number
  }
  sessions: Array<{
    sessionId: string
    state: string
    codec: string | null
    captureFps: number | null
    encodeFps: number | null
    transmitFps: number | null
    encodeLatencyMs: number | null
    bitrateKbps: number | null
    droppedFrames: number | null
  }>
}

export type SolResourceUpdate =
  | { hostId: string; resource: 'sessions'; payload: { sessions: LogicalSession[] } }
  | { hostId: string; resource: 'telemetry'; payload: TelemetrySnapshot }
  | {
      hostId: string
      resource: 'event'
      payload:
        | { type: 'session.created' | 'session.updated'; data: LogicalSession }
        | { type: 'session.removed'; data: { id: string } }
        | { type: 'telemetry.sample'; data: TelemetrySnapshot }
        | {
            type:
              | 'host.changed'
              | 'capabilities.changed'
              | 'catalog.changed'
              | 'displays.changed'
              | 'virtualDisplay.created'
              | 'virtualDisplay.updated'
              | 'virtualDisplay.removed'
              | 'workspace.created'
              | 'workspace.updated'
              | 'workspace.removed'
              | 'profile.created'
              | 'profile.updated'
              | 'profile.removed'
              | 'peripheral.added'
              | 'peripheral.updated'
              | 'peripheral.removed'
              | 'sandbox.created'
              | 'sandbox.updated'
              | 'sandbox.removed'
              | 'operation.updated'
              | 'host.stopping'
              | 'resync.required'
            data: unknown
          }
    }

interface ClientState {
  bridge: BridgeStatus
  hosts: Host[]
  hostError: string | undefined
  pairing: PairingState | undefined
  library: AppLibrary
  session: SessionUpdate | undefined
  logicalSessionsByHost: Record<string, LogicalSession[]>
  telemetryByHost: Record<string, TelemetrySnapshot | undefined>
  appMode: AppMode
  settingsByMode: Record<AppMode, StreamSettings>
  setBridge: (bridge: BridgeStatus) => void
  setHosts: (hosts: Host[]) => void
  setHostError: (hostError?: string) => void
  startPairing: (hostId: string, pin: string) => void
  updatePairing: (update: PairingUpdate) => void
  clearPairing: () => void
  setLibrary: (library: AppLibrary) => void
  setArtwork: (hostId: string, appId: number, dataUrl: string, error: string) => void
  setSession: (session: SessionUpdate) => void
  setSolResource: (update: SolResourceUpdate) => void
  setAppMode: (appMode: AppMode) => void
  updateSettings: (settings: Partial<StreamSettings>) => void
  resetSettings: () => void
  addPreviewHost: (host: Pick<Host, 'name' | 'address'>) => void
}

export const useClientStore = create<ClientState>()(
  persist(
    (set) => ({
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
      appMode: 'gaming',
      settingsByMode: {
        gaming: { ...defaultSettingsByMode.gaming },
        workstation: { ...defaultSettingsByMode.workstation },
      },
      setBridge: (bridge) => set({ bridge }),
      setHosts: (hosts) => set({ hosts, hostError: undefined }),
      setHostError: (hostError) => set({ hostError }),
      startPairing: (hostId, pin) =>
        set({
          pairing: {
            hostId,
            pin,
            state: 'pairing',
            message: "Enter this PIN in Sol's web interface.",
          },
        }),
      updatePairing: (update) =>
        set((state) => ({
          pairing:
            state.pairing?.hostId === update.hostId
              ? { ...state.pairing, ...update }
              : { ...update, pin: '' },
        })),
      clearPairing: () => set({ pairing: undefined }),
      setLibrary: (library) => set({ library }),
      setArtwork: (hostId, appId, dataUrl, error) =>
        set((state) => {
          if (state.library.hostId !== hostId) return state
          return {
            library: {
              ...state.library,
              apps: state.library.apps.map((app) => {
                if (app.id !== appId) return app
                const { artDataUrl: _oldArtwork, ...withoutArtwork } = app
                return dataUrl
                  ? { ...withoutArtwork, artDataUrl: dataUrl, artError: error }
                  : { ...withoutArtwork, artError: error }
              }),
            },
          }
        }),
      setSession: (session) =>
        set((state) => ({
          session:
            session.appId === 0 && state.session?.hostId === session.hostId
              ? { ...session, appId: state.session.appId, appName: state.session.appName }
              : session,
        })),
      setSolResource: (update) =>
        set((state) => {
          if (update.resource === 'sessions') {
            return {
              logicalSessionsByHost: {
                ...state.logicalSessionsByHost,
                [update.hostId]: update.payload.sessions,
              },
            }
          }
          if (update.resource === 'telemetry') {
            return {
              telemetryByHost: {
                ...state.telemetryByHost,
                [update.hostId]: update.payload,
              },
            }
          }
          const event = update.payload
          if (event.type === 'telemetry.sample') {
            return {
              telemetryByHost: {
                ...state.telemetryByHost,
                [update.hostId]: event.data,
              },
            }
          }
          if (
            event.type !== 'session.created' &&
            event.type !== 'session.updated' &&
            event.type !== 'session.removed'
          ) {
            return state
          }
          const sessions = state.logicalSessionsByHost[update.hostId] ?? []
          const nextSessions =
            event.type === 'session.removed'
              ? sessions.filter((session) => session.id !== event.data.id)
              : [...sessions.filter((session) => session.id !== event.data.id), event.data].sort(
                  (left, right) => right.updatedAt - left.updatedAt,
                )
          return {
            logicalSessionsByHost: {
              ...state.logicalSessionsByHost,
              [update.hostId]: nextSessions,
            },
          }
        }),
      setAppMode: (appMode) => set({ appMode }),
      updateSettings: (settings) =>
        set((state) => ({
          settingsByMode: {
            ...state.settingsByMode,
            [state.appMode]: { ...state.settingsByMode[state.appMode], ...settings },
          },
        })),
      resetSettings: () =>
        set((state) => ({
          settingsByMode: {
            ...state.settingsByMode,
            [state.appMode]: { ...defaultSettingsByMode[state.appMode] },
          },
        })),
      addPreviewHost: (host) =>
        set((state) => ({
          hosts: [
            ...state.hosts,
            {
              ...host,
              id: crypto.randomUUID(),
              serverName: '',
              serverUniqueId: '',
              appVersion: '',
              serverState: '',
              status: 'offline',
              error: 'Native probing requires Neutralino.',
              httpsPort: 0,
              currentGameId: 0,
              serverCodecModeSupport: 1,
              maxLumaPixelsHevc: 0,
              displayModes: [],
              lastSeenAt: 0,
              paired: false,
              apiVersion: 0,
              apiPort: 0,
              capabilities: [],
              apiClientUuid: '',
              apiClientName: '',
              apiScopes: [],
              allowedApps: [],
              features: {},
              limits: {},
            },
          ],
        })),
    }),
    {
      // Persisted contract: keep existing profiles across the Terra branding rename.
      name: 'eclipse-client-settings',
      storage: createJSONStorage(() => settingsStorage),
      version: 1,
      partialize: (state) => ({
        appMode: state.appMode,
        settingsByMode: state.settingsByMode,
      }),
      migrate: (persisted, version) => {
        if (version !== 0 || !persisted || typeof persisted !== 'object') return persisted
        const legacy = persisted as { settings?: Partial<StreamSettings> }
        const gaming = settingsSchema.safeParse({ ...defaultSettings, ...legacy.settings })
        return {
          appMode: 'gaming',
          settingsByMode: {
            gaming: gaming.success ? gaming.data : { ...defaultSettingsByMode.gaming },
            workstation: { ...defaultSettingsByMode.workstation },
          },
        }
      },
      merge: (persisted, current) => {
        const stored = persisted as Partial<ClientState>
        const appMode = appModeSchema.safeParse(stored.appMode)
        const gaming = settingsSchema.safeParse({
          ...defaultSettingsByMode.gaming,
          ...stored.settingsByMode?.gaming,
        })
        const workstation = settingsSchema.safeParse({
          ...defaultSettingsByMode.workstation,
          ...stored.settingsByMode?.workstation,
        })
        return {
          ...current,
          appMode: appMode.success ? appMode.data : 'gaming',
          settingsByMode: {
            gaming: gaming.success ? gaming.data : { ...defaultSettingsByMode.gaming },
            workstation: workstation.success
              ? workstation.data
              : { ...defaultSettingsByMode.workstation },
          },
        }
      },
      skipHydration: true,
    },
  ),
)
