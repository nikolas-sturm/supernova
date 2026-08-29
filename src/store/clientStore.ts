import { create } from 'zustand'
import { persist } from 'zustand/middleware'
import { defaultSettings, type StreamSettings, settingsSchema } from '../settings'

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
  lastSeenAt: number
  paired: boolean
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
  settings: StreamSettings
  setBridge: (bridge: BridgeStatus) => void
  setHosts: (hosts: Host[]) => void
  setHostError: (hostError?: string) => void
  startPairing: (hostId: string, pin: string) => void
  updatePairing: (update: PairingUpdate) => void
  clearPairing: () => void
  setLibrary: (library: AppLibrary) => void
  setArtwork: (hostId: string, appId: number, dataUrl: string, error: string) => void
  setSession: (session: SessionUpdate) => void
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
      settings: defaultSettings,
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
      updateSettings: (settings) =>
        set((state) => ({ settings: { ...state.settings, ...settings } })),
      resetSettings: () => set({ settings: defaultSettings }),
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
              lastSeenAt: 0,
              paired: false,
            },
          ],
        })),
    }),
    {
      name: 'eclipse-client-settings',
      partialize: (state) => ({ settings: state.settings }),
      merge: (persisted, current) => {
        const stored = persisted as Partial<ClientState>
        const settings = settingsSchema.safeParse({ ...defaultSettings, ...stored.settings })
        return {
          ...current,
          settings: settings.success ? settings.data : defaultSettings,
        }
      },
    },
  ),
)
