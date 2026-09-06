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

interface ClientState {
  bridge: BridgeStatus
  hosts: Host[]
  hostError: string | undefined
  pairing: PairingState | undefined
  library: AppLibrary
  session: SessionUpdate | undefined
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
            message: "Enter this PIN in Sunshine's web interface.",
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
            },
          ],
        })),
    }),
    {
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
