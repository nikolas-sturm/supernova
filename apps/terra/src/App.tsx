import { Button, ThemePicker } from '@supernova/design-system'
import {
  Activity,
  ArrowRight,
  Boxes,
  Cable,
  CircleHelp,
  EllipsisVertical,
  Gamepad2,
  Gauge,
  ImageOff,
  Info,
  Keyboard,
  KeyRound,
  LayoutGrid,
  Library,
  Maximize2,
  Monitor,
  MonitorUp,
  Play,
  Plus,
  Power,
  Radio,
  RefreshCw,
  Search,
  Server,
  ShieldCheck,
  SlidersHorizontal,
  Square,
  Trash2,
  X,
} from 'lucide-react'
import { type FormEvent, useDeferredValue, useEffect, useState } from 'react'
import styles from './App.module.css'
import { clientDisplayVirtualDisplays } from './clientDisplays'
import {
  applyUiDisplayMode,
  cancelCoreSession,
  configureCoreDiscovery,
  controlCoreLogicalSession,
  launchCoreApp,
  loadCoreApps,
  loadCoreResource,
  pairCoreHost,
  refreshCoreHost,
  removeCoreHost,
  saveCoreHost,
  startCoreBridge,
  wakeCoreHost,
} from './native/coreBridge'
import { SettingsView } from './SettingsView'
import { settingsSchema } from './settings'
import { useClientStore } from './store/clientStore'
import {
  DisplayManager,
  HardwareManager,
  ProfileManager,
  SandboxManager,
  WorkspaceManager,
} from './WorkstationResources'

type GamingView =
  | 'game-library'
  | 'paired-hosts'
  | 'active-sessions'
  | 'stream-settings'
  | 'gamepad'
type WorkstationView =
  | 'workspaces'
  | 'display-topology'
  | 'app-sandboxes'
  | 'workstation-settings'
  | 'hardware'
type AppView = GamingView | WorkstationView

const navigation = {
  gaming: [
    { id: 'game-library', label: 'Game Library', icon: Library },
    { id: 'paired-hosts', label: 'Paired Hosts', icon: Radio },
    { id: 'active-sessions', label: 'Active Sessions', icon: Activity },
    { id: 'stream-settings', label: 'Stream Settings', icon: SlidersHorizontal },
    { id: 'gamepad', label: 'Gamepad & Haptics', icon: Gamepad2 },
  ],
  workstation: [
    { id: 'workspaces', label: 'Workspaces & Desktops', icon: Monitor },
    { id: 'display-topology', label: 'Display & Topology', icon: LayoutGrid },
    { id: 'app-sandboxes', label: 'App Sandboxes', icon: Boxes },
    { id: 'workstation-settings', label: 'Workstation Settings', icon: SlidersHorizontal },
    { id: 'hardware', label: 'Hardware & Peripherals', icon: Cable },
  ],
} as const

const viewTitles: Record<AppView, string> = {
  'game-library': 'Game library',
  'paired-hosts': 'Paired hosts',
  'active-sessions': 'Active sessions',
  'stream-settings': 'Stream settings',
  gamepad: 'Gamepad & haptics',
  workspaces: 'Workspaces & desktops',
  'display-topology': 'Display & topology',
  'app-sandboxes': 'App sandboxes',
  'workstation-settings': 'Workstation settings',
  hardware: 'Hardware & peripherals',
}

function ModeSwitch({
  value,
  onChange,
  label,
}: {
  value: 'gaming' | 'workstation'
  onChange: (mode: 'gaming' | 'workstation') => void
  label: string
}) {
  return (
    <fieldset className={styles.modeSwitch} aria-label={label}>
      <button type="button" aria-pressed={value === 'gaming'} onClick={() => onChange('gaming')}>
        <Gamepad2 size={14} /> Gaming
      </button>
      <button
        type="button"
        aria-pressed={value === 'workstation'}
        onClick={() => onChange('workstation')}
      >
        <Monitor size={14} /> Workstation
      </button>
    </fieldset>
  )
}

export function App() {
  const bridge = useClientStore((state) => state.bridge)
  const hosts = useClientStore((state) => state.hosts)
  const hostError = useClientStore((state) => state.hostError)
  const pairing = useClientStore((state) => state.pairing)
  const library = useClientStore((state) => state.library)
  const session = useClientStore((state) => state.session)
  const logicalSessionsByHost = useClientStore((state) => state.logicalSessionsByHost)
  const telemetryByHost = useClientStore((state) => state.telemetryByHost)
  const resourcesByHost = useClientStore((state) => state.resourcesByHost)
  const appMode = useClientStore((state) => state.appMode)
  const settings = useClientStore((state) => state.settingsByMode[state.appMode])
  const setAppMode = useClientStore((state) => state.setAppMode)
  const setBridge = useClientStore((state) => state.setBridge)
  const setHosts = useClientStore((state) => state.setHosts)
  const setHostError = useClientStore((state) => state.setHostError)
  const addPreviewHost = useClientStore((state) => state.addPreviewHost)
  const startPairing = useClientStore((state) => state.startPairing)
  const updatePairing = useClientStore((state) => state.updatePairing)
  const clearPairing = useClientStore((state) => state.clearPairing)
  const setLibrary = useClientStore((state) => state.setLibrary)
  const setArtwork = useClientStore((state) => state.setArtwork)
  const setSession = useClientStore((state) => state.setSession)
  const setSolResource = useClientStore((state) => state.setSolResource)
  const setClientDisplays = useClientStore((state) => state.setClientDisplays)
  const [showAddHost, setShowAddHost] = useState(false)
  const [selectedHostId, setSelectedHostId] = useState<string>()
  const [libraryQuery, setLibraryQuery] = useState('')
  const [libraryFilter, setLibraryFilter] = useState<
    'all' | 'games' | 'apps' | 'installed' | 'updates'
  >('all')
  const [librarySort, setLibrarySort] = useState<'host' | 'name' | 'updates'>('host')
  const [selectedAppId, setSelectedAppId] = useState<number>()
  const [selectedLaunchProfileId, setSelectedLaunchProfileId] = useState('')
  const deferredLibraryQuery = useDeferredValue(libraryQuery)
  const [activeViews, setActiveViews] = useState<Record<'gaming' | 'workstation', AppView>>({
    gaming: 'game-library',
    workstation: 'workspaces',
  })
  const activeView = activeViews[appMode]

  function setActiveView(activeView: AppView) {
    setActiveViews((current) => ({ ...current, [appMode]: activeView }))
  }

  function openAppDetails(appId: number) {
    setSelectedLaunchProfileId('')
    setSelectedAppId(appId)
  }

  function closeAppDetails() {
    setSelectedAppId(undefined)
    setSelectedLaunchProfileId('')
  }

  useEffect(
    () =>
      startCoreBridge({
        onStatus: setBridge,
        onHosts: setHosts,
        onHostError: setHostError,
        onPairing: updatePairing,
        onApps: setLibrary,
        onArtwork: setArtwork,
        onSession: setSession,
        onSolResource: setSolResource,
        onClientDisplays: setClientDisplays,
      }),
    [
      setArtwork,
      setBridge,
      setClientDisplays,
      setHostError,
      setHosts,
      setLibrary,
      setSession,
      setSolResource,
      updatePairing,
    ],
  )

  useEffect(() => {
    void applyUiDisplayMode(settings.uiDisplayMode).catch(() => {
      setHostError('Terra could not apply the selected GUI display mode.')
    })
  }, [setHostError, settings.uiDisplayMode])

  useEffect(() => {
    if (bridge.state !== 'ready') return
    void configureCoreDiscovery(settings.autoDiscoverHosts).catch(() => {
      setHostError('Terra could not update local-network discovery.')
    })
  }, [bridge.state, setHostError, settings.autoDiscoverHosts])

  useEffect(() => {
    if (selectedHostId && hosts.some((host) => host.id === selectedHostId)) return
    const nextHost =
      hosts.find((host) => host.paired && host.status === 'online') ??
      hosts.find((host) => host.paired) ??
      hosts[0]
    if (!nextHost) return
    setSelectedHostId(nextHost.id)
    if (nextHost.paired && nextHost.status === 'online' && library.hostId !== nextHost.id) {
      void loadCoreApps(nextHost.id).catch(() => {
        setHostError('Terra could not load the active host library.')
      })
    }
  }, [hosts, library.hostId, selectedHostId, setHostError])

  useEffect(() => {
    const host = hosts.find((candidate) => candidate.id === selectedHostId)
    if (bridge.state !== 'ready' || host?.status !== 'online' || host.apiVersion !== 1) return
    if (host.apiScopes?.includes('session.control')) {
      void loadCoreResource(host.id, 'sessions').catch(() => {
        setHostError('Terra could not load Sol logical sessions.')
      })
    }
    if (host.apiScopes?.includes('telemetry.read')) {
      void loadCoreResource(host.id, 'telemetry').catch(() => {
        setHostError('Terra could not load Sol telemetry.')
      })
    }
    if (host.apiScopes?.includes('catalog.read') && host.capabilities?.includes('profiles-v1')) {
      void loadCoreResource(host.id, 'profiles').catch(() => {
        setHostError('Terra could not load Sol launch profiles.')
      })
    }
  }, [bridge.state, hosts, selectedHostId, setHostError])

  useEffect(() => {
    if (appMode !== 'workstation') return
    const host = hosts.find((candidate) => candidate.id === selectedHostId)
    if (bridge.state !== 'ready' || host?.status !== 'online' || host.apiVersion !== 1) return
    const resources: Array<Parameters<typeof loadCoreResource>[1]> = []
    if (activeView === 'workspaces' && host.capabilities?.includes('workspaces-v1')) {
      resources.push('workspaces')
    }
    if (activeView === 'app-sandboxes' && host.capabilities?.includes('sandboxes-v1')) {
      resources.push('sandboxes')
      if (host.capabilities.includes('profiles-v1')) resources.push('profiles')
    }
    if (activeView === 'workstation-settings' && host.capabilities?.includes('profiles-v1')) {
      resources.push('profiles')
    }
    if (activeView === 'hardware' && host.capabilities?.includes('peripherals-v1')) {
      resources.push('peripherals', 'peripheral-claims')
      if (host.apiScopes?.includes('session.control')) resources.push('sessions')
      if (host.capabilities.includes('workspaces-v1')) resources.push('workspaces')
      if (host.capabilities.includes('sandboxes-v1')) resources.push('sandboxes')
    }
    for (const resource of resources) {
      void loadCoreResource(host.id, resource).catch(() => {
        setHostError(`Terra could not load Sol ${resource}.`)
      })
    }
  }, [activeView, appMode, bridge.state, hosts, selectedHostId, setHostError])

  async function handleAddHost(event: FormEvent<HTMLFormElement>) {
    event.preventDefault()
    const formData = new FormData(event.currentTarget)
    const name = String(formData.get('name') ?? '').trim()
    const address = String(formData.get('address') ?? '').trim()

    if (!name || !address) return

    setHostError(undefined)
    try {
      const dispatched = await saveCoreHost(name, address)
      if (!dispatched) addPreviewHost({ name, address })
      setShowAddHost(false)
    } catch (error) {
      setHostError(
        error instanceof Error ? error.message : 'Native core did not accept the host request.',
      )
    }
  }

  async function handlePair(hostId: string) {
    const random = new Uint16Array(1)
    let randomValue = 0
    do {
      crypto.getRandomValues(random)
      randomValue = random[0] ?? 0
    } while (randomValue >= 60000)
    const pin = (randomValue % 10000).toString().padStart(4, '0')
    startPairing(hostId, pin)
    try {
      await pairCoreHost(hostId, pin, appMode)
    } catch (error) {
      updatePairing({
        hostId,
        state: 'error',
        message: error instanceof Error ? error.message : 'Native core did not accept pairing.',
      })
    }
  }

  async function handleOpenLibrary(hostId: string) {
    setSelectedHostId(hostId)
    setSelectedAppId(undefined)
    setHostError(undefined)
    try {
      await loadCoreApps(hostId)
      if (appMode === 'gaming' && activeView !== 'game-library') setActiveView('game-library')
    } catch (error) {
      setHostError(
        error instanceof Error ? error.message : 'Native core did not accept app listing.',
      )
    }
  }

  async function handleWake(hostId: string) {
    setHostError(undefined)
    try {
      await wakeCoreHost(hostId)
    } catch (error) {
      setHostError(error instanceof Error ? error.message : 'Native core did not send wake packet.')
    }
  }

  async function handleLaunch(
    hostId: string,
    appId: number,
    launchProfileId = '',
    workspaceId = '',
  ) {
    setHostError(undefined)
    try {
      const app =
        library.hostId === hostId
          ? library.apps.find((candidate) => candidate.id === appId)
          : undefined
      const host = hosts.find((candidate) => candidate.id === hostId)
      const profileSnapshot = useClientStore.getState().resourcesByHost[hostId]?.profiles
      if (
        host?.capabilities?.includes('profiles-v1') &&
        (app?.streamProfileId || app?.launchProfiles?.length) &&
        !profileSnapshot
      ) {
        await loadCoreResource(hostId, 'profiles')
        setHostError('Sol launch profiles are still loading. Retry when profile data is ready.')
        return
      }
      const profiles = profileSnapshot?.profiles ?? []
      const resolvedLaunchProfileId =
        launchProfileId || app?.launchProfiles?.find((profile) => profile.default)?.id || ''
      const launchProfile = profiles.find(
        (profile) => profile.id === resolvedLaunchProfileId && profile.type === 'launch',
      )
      const launchStreamProfileId =
        typeof launchProfile?.configuration.streamProfileId === 'string'
          ? launchProfile.configuration.streamProfileId
          : undefined
      const streamProfile = profiles.find(
        (profile) =>
          profile.id === (launchStreamProfileId ?? app?.streamProfileId) &&
          profile.type === 'stream',
      )
      const configuration = streamProfile?.configuration
      const profiledSettings = configuration
        ? settingsSchema.safeParse({
            ...settings,
            width: configuration.width,
            height: configuration.height,
            fps: configuration.fps,
            bitrateKbps: configuration.bitrateKbps,
            videoCodec: configuration.codec,
            enableHdr: configuration.hdr,
            enableYuv444: configuration.yuv444,
            audioConfig: configuration.audioChannels,
            muteHostAudio:
              typeof configuration.hostAudio === 'boolean'
                ? !configuration.hostAudio
                : settings.muteHostAudio,
            absoluteMouseMode:
              configuration.inputMode === 'absolute'
                ? true
                : configuration.inputMode === 'relative'
                  ? false
                  : settings.absoluteMouseMode,
            gameOptimizations: configuration.gameOptimizations,
          })
        : undefined
      const clientState = useClientStore.getState()
      const virtualDisplays =
        appMode === 'workstation' && !workspaceId
          ? clientDisplayVirtualDisplays(
              clientState.clientDisplays,
              clientState.clientDisplayPreferences,
              clientState.clientPrimaryDisplayId,
            )
          : []
      if (appMode === 'workstation' && !workspaceId && virtualDisplays.length === 0) {
        setHostError('Enable at least one client display before launching a workstation app.')
        return
      }
      await launchCoreApp(
        hostId,
        appId,
        profiledSettings?.success ? profiledSettings.data : settings,
        resolvedLaunchProfileId,
        workspaceId,
        virtualDisplays,
      )
      closeAppDetails()
    } catch (error) {
      setHostError(error instanceof Error ? error.message : 'Native core did not accept launch.')
    }
  }

  async function handleStopSession(hostId: string, quitHost: boolean) {
    setHostError(undefined)
    try {
      await cancelCoreSession(hostId, quitHost)
    } catch (error) {
      setHostError(
        error instanceof Error ? error.message : 'Native core did not accept stop request.',
      )
    }
  }

  async function handleLogicalSession(
    hostId: string,
    sessionId: string,
    action: 'disconnect' | 'stop',
  ) {
    setHostError(undefined)
    try {
      await controlCoreLogicalSession(hostId, sessionId, action)
    } catch (error) {
      setHostError(
        error instanceof Error ? error.message : `Native core could not ${action} session.`,
      )
    }
  }

  const onlineCount = hosts.filter((host) => host.status === 'online').length
  const pairingHost = pairing ? hosts.find((host) => host.id === pairing.hostId) : undefined
  const selectedHost = hosts.find((host) => host.id === selectedHostId)
  const selectedApps = library.hostId === selectedHostId ? library.apps : []
  const selectedApp = selectedApps.find((app) => app.id === selectedAppId)
  const selectedProfiles = selectedHostId
    ? (resourcesByHost[selectedHostId]?.profiles?.profiles ?? [])
    : []
  const logicalSessions = Object.entries(logicalSessionsByHost).flatMap(([hostId, sessions]) =>
    sessions
      .filter((logicalSession) => !['stopped', 'failed'].includes(logicalSession.state))
      .map((logicalSession) => ({ hostId, session: logicalSession })),
  )
  const selectedTelemetry = selectedHostId ? telemetryByHost[selectedHostId] : undefined
  const activeSession = session?.hostId === selectedHostId ? session : undefined
  const sessionHost = session ? hosts.find((host) => host.id === session.hostId) : undefined
  const settingsView = activeView === 'stream-settings' || activeView === 'workstation-settings'
  const gamepadView = activeView === 'gamepad'
  const hostsView = activeView === 'paired-hosts'
  const sessionsView = activeView === 'active-sessions'
  const profileCodec =
    settings.videoCodec === 'automatic' ? 'AUTO' : settings.videoCodec.toUpperCase()
  const runningApp = selectedApps.find((app) => app.id === selectedHost?.currentGameId)
  const featuredApp = runningApp ?? selectedApps[0]
  const sessionBusy = Boolean(
    activeSession && !['stopped', 'terminated', 'error'].includes(activeSession.state),
  )
  const visibleApps = selectedApps
    .filter((app) => {
      if (libraryFilter === 'games' && !app.appCollectorGame) return false
      if (libraryFilter === 'apps' && app.appCollectorGame) return false
      if (libraryFilter === 'installed' && app.installed === false) return false
      if (libraryFilter === 'updates' && !app.updateAvailable) return false
      const query = deferredLibraryQuery.trim().toLocaleLowerCase()
      if (!query) return true
      return [app.name, app.publisher, app.source, app.description, ...(app.tags ?? [])]
        .filter(Boolean)
        .join(' ')
        .toLocaleLowerCase()
        .includes(query)
    })
    .sort((left, right) => {
      if (librarySort === 'name') return left.name.localeCompare(right.name)
      if (librarySort === 'updates') {
        return Number(Boolean(right.updateAvailable)) - Number(Boolean(left.updateAvailable))
      }
      return 0
    })
  const workstationAccessMissing = Boolean(
    selectedHost?.paired &&
      appMode === 'workstation' &&
      !selectedHost.apiScopes?.includes('host.control'),
  )
  const activeHostBar = (
    <section className={styles.hostCommandBar} aria-label="Active host">
      <div className={styles.hostCommandIcon}>
        <Monitor size={22} />
      </div>
      {selectedHost ? (
        <div className={styles.hostCommandIdentity}>
          <strong>
            {selectedHost.serverName || selectedHost.name}
            {selectedHost.appVersion && <span>· Host {selectedHost.appVersion}</span>}
          </strong>
          <p>
            <span className={`${styles.hostState} ${styles[selectedHost.status]}`}>
              {selectedHost.status}
            </span>
            <code>{selectedHost.address}</code>
            {selectedHost.paired && <span>· PAIRED</span>}
          </p>
        </div>
      ) : (
        <div className={styles.hostCommandIdentity}>
          <strong>No active host</strong>
          <p>Add and pair a Sol computer to populate this mode.</p>
        </div>
      )}
      <div className={styles.hostCommandActions}>
        {hosts.length > 1 && (
          <select
            aria-label="Active computer"
            value={selectedHostId}
            onChange={(event) => {
              setSelectedHostId(event.currentTarget.value)
              setSelectedAppId(undefined)
            }}
          >
            {hosts.map((host) => (
              <option value={host.id} key={host.id}>
                {host.serverName || host.name}
              </option>
            ))}
          </select>
        )}
        {selectedHost?.status === 'offline' && selectedHost.wakeable && (
          <button type="button" onClick={() => void handleWake(selectedHost.id)}>
            <Power size={14} /> Wake-on-LAN
          </button>
        )}
        {selectedHost && (
          <button type="button" onClick={() => void refreshCoreHost(selectedHost.id)}>
            <RefreshCw size={14} /> Refresh
          </button>
        )}
        {selectedHost?.status === 'online' &&
          (!selectedHost.paired || workstationAccessMissing) && (
            <button type="button" onClick={() => void handlePair(selectedHost.id)}>
              <KeyRound size={14} />
              {workstationAccessMissing ? 'Upgrade workstation access' : 'Pair computer'}
            </button>
          )}
        <button type="button" onClick={() => setShowAddHost(true)}>
          <Plus size={14} /> Add host
        </button>
      </div>
    </section>
  )

  return (
    <div className={styles.appShell} data-mode={appMode}>
      <aside className={styles.sidebar}>
        <div className={styles.sidebarUtility}>
          <span className={styles.windowDots} aria-hidden="true">
            <i />
            <i />
            <i />
          </span>
          <span className={styles.hostLinked}>
            <i /> {bridge.state === 'ready' ? 'HOST LINKED' : bridge.label.toUpperCase()}
          </span>
        </div>

        <div className={styles.brand}>
          <span className={styles.terraBrand}>
            <span className={styles.terraMark} aria-hidden="true" />
            <span>
              <strong>TERRA</strong>
              <small>TERRA / OPEN STREAM CLIENT</small>
            </span>
          </span>
        </div>

        <ModeSwitch value={appMode} onChange={setAppMode} label="Application mode" />

        <p className={styles.navigationLabel}>CLIENT NAVIGATION</p>

        <nav className={styles.navigation} aria-label="Primary navigation">
          {navigation[appMode].map(({ id, label, icon: Icon }) => (
            <button
              className={activeView === id ? styles.navActive : undefined}
              type="button"
              aria-label={label}
              aria-current={activeView === id ? 'page' : undefined}
              key={id}
              onClick={() => setActiveView(id)}
            >
              <Icon size={18} />
              {label}
              {id === 'game-library' && <span>{library.apps.length || hosts.length}</span>}
              {id === 'active-sessions' && <span>{logicalSessions.length}</span>}
              {id === 'paired-hosts' && <span>{onlineCount}</span>}
            </button>
          ))}
        </nav>

        <div className={styles.sidebarFooter}>
          <div className={styles.profileCard}>
            <span>ACTIVE PROFILE</span>
            <strong>
              {settings.width >= 3840
                ? '4K'
                : settings.height >= 1440
                  ? '1440P'
                  : `${settings.height}P`}{' '}
              • {settings.fps} FPS
            </strong>
            <code>
              {profileCodec} {settings.enableHdr ? 'HDR' : 'SDR'}
            </code>
          </div>
        </div>

        <div className={styles.sidebarStatus}>
          <div className={styles.statusHeading}>
            <Radio size={15} />
            Native core
          </div>
          <strong>{bridge.label}</strong>
          <p>{bridge.detail}</p>
          {bridge.moonlightCommonRevision && <code>common-c {bridge.moonlightCommonRevision}</code>}
        </div>

        <button className={styles.helpLink} type="button" disabled>
          <CircleHelp size={17} />
          Project guide
        </button>
      </aside>

      <main className={styles.main}>
        <header className={styles.header}>
          <div>
            <p className={styles.eyebrow}>
              {appMode === 'gaming' ? 'GAMING CLIENT / MAIN RIG' : 'WORKSTATION CLIENT / MAIN RIG'}
            </p>
            <h1>{viewTitles[activeView]}</h1>
          </div>
          <div className={styles.headerActions}>
            <ThemePicker
              themeLabel={(theme) =>
                theme === 'dark'
                  ? 'Terra / Dark'
                  : theme === 'auto'
                    ? 'System'
                    : theme
                        .split('-')
                        .map((word) => word.charAt(0).toUpperCase() + word.slice(1))
                        .join(' ')
              }
            />
            <div className={`${styles.corePill} ${styles[bridge.state]}`}>
              <span />
              {bridge.label}
            </div>
            {(hostsView || activeView === 'workspaces') && (
              <Button type="button" onClick={() => setShowAddHost(true)}>
                <Plus size={18} />
                Add computer
              </Button>
            )}
          </div>
        </header>

        {settingsView || gamepadView ? (
          <>
            <SettingsView section={gamepadView ? 'gamepad' : 'profile'} />
            {activeView === 'workstation-settings' && (
              <ProfileManager
                host={selectedHost}
                apps={selectedApps}
                settings={settings}
                onError={setHostError}
              />
            )}
          </>
        ) : activeView === 'display-topology' ? (
          <DisplayManager
            host={selectedHost}
            apps={selectedApps}
            settings={settings}
            onError={setHostError}
          />
        ) : activeView === 'app-sandboxes' ? (
          <SandboxManager
            host={selectedHost}
            apps={selectedApps}
            settings={settings}
            onError={setHostError}
          />
        ) : activeView === 'hardware' ? (
          <HardwareManager
            host={selectedHost}
            apps={selectedApps}
            settings={settings}
            onError={setHostError}
          />
        ) : sessionsView ? (
          <section className={styles.sessionsPage}>
            <div className={styles.sectionHeading}>
              <div>
                <p className={styles.eyebrow}>NATIVE STREAM CORE</p>
                <h2>Logical sessions</h2>
              </div>
              <span>{logicalSessions.length.toString().padStart(2, '0')} ACTIVE</span>
            </div>
            {hostError && <p className={styles.hostError}>{hostError}</p>}
            {logicalSessions.length > 0 ? (
              <div className={styles.logicalSessionGrid}>
                {logicalSessions.map(({ hostId, session: logicalSession }) => {
                  const host = hosts.find((candidate) => candidate.id === hostId)
                  const app =
                    library.hostId === hostId
                      ? library.apps.find(
                          (candidate) =>
                            candidate.uuid === logicalSession.appUuid ||
                            candidate.id === logicalSession.legacyAppId,
                        )
                      : undefined
                  const telemetry = telemetryByHost[hostId]?.sessions.find(
                    (sample) => sample.sessionId === logicalSession.id,
                  )
                  return (
                    <article className={styles.logicalSessionCard} key={logicalSession.id}>
                      <header>
                        <span className={styles.hostState}>{logicalSession.state}</span>
                        <code>REV {logicalSession.revision}</code>
                      </header>
                      <h3>{app?.name || `Application ${logicalSession.legacyAppId}`}</h3>
                      <p>{host?.serverName || host?.name || 'Sol host'}</p>
                      <dl>
                        <div>
                          <dt>Output</dt>
                          <dd>
                            {logicalSession.width} × {logicalSession.height} /{' '}
                            {logicalSession.refreshRate} Hz
                          </dd>
                        </div>
                        <div>
                          <dt>Encoder</dt>
                          <dd>{telemetry?.codec ?? 'Awaiting sample'}</dd>
                        </div>
                        <div>
                          <dt>Bitrate</dt>
                          <dd>
                            {telemetry?.bitrateKbps == null
                              ? '—'
                              : `${(telemetry.bitrateKbps / 1000).toFixed(1)} Mbps`}
                          </dd>
                        </div>
                        <div>
                          <dt>Encode</dt>
                          <dd>
                            {telemetry?.encodeLatencyMs == null
                              ? '—'
                              : `${telemetry.encodeLatencyMs.toFixed(1)} ms`}
                          </dd>
                        </div>
                        <div>
                          <dt>Transmit</dt>
                          <dd>
                            {telemetry?.transmitFps == null
                              ? '—'
                              : `${telemetry.transmitFps.toFixed(1)} FPS`}
                          </dd>
                        </div>
                        <div>
                          <dt>Frame drops</dt>
                          <dd>{telemetry?.droppedFrames ?? '—'}</dd>
                        </div>
                      </dl>
                      <footer>
                        {logicalSession.state === 'disconnected' && (
                          <button
                            type="button"
                            disabled={logicalSession.legacyAppId === 0}
                            onClick={() =>
                              library.hostId === hostId
                                ? void handleLaunch(hostId, logicalSession.legacyAppId)
                                : void handleOpenLibrary(hostId)
                            }
                          >
                            <Play size={13} />
                            {library.hostId === hostId ? 'Resume stream' : 'Open host library'}
                          </button>
                        )}
                        {logicalSession.state !== 'disconnected' && (
                          <button
                            type="button"
                            onClick={() =>
                              void handleLogicalSession(hostId, logicalSession.id, 'disconnect')
                            }
                          >
                            Disconnect
                          </button>
                        )}
                        <button
                          type="button"
                          onClick={() =>
                            void handleLogicalSession(hostId, logicalSession.id, 'stop')
                          }
                        >
                          <Square size={13} /> Stop app
                        </button>
                      </footer>
                    </article>
                  )
                })}
              </div>
            ) : session && session.state !== 'stopped' ? (
              <div className={`${styles.sessionBanner} ${styles[session.state]}`}>
                <div>
                  <span>{session.state}</span>
                  <strong>{session.appName || 'Native session'}</strong>
                  <p>
                    {sessionHost?.name ? `${sessionHost.name} · ` : ''}
                    {session.message}
                  </p>
                </div>
                {sessionHost && (
                  <button
                    type="button"
                    onClick={() => void handleStopSession(sessionHost.id, settings.quitAppAfter)}
                  >
                    <Square size={13} />
                    {settings.quitAppAfter ? 'Stop host app' : 'Disconnect'}
                  </button>
                )}
              </div>
            ) : (
              <div className={styles.emptyState}>
                <div className={styles.emptyIcon}>
                  <Activity size={28} />
                </div>
                <div>
                  <h3>No active stream</h3>
                  <p>Launch or resume an application from Game Library.</p>
                </div>
              </div>
            )}
            {selectedTelemetry && (
              <section className={styles.telemetryPanel} aria-label="Selected host telemetry">
                <span>{selectedTelemetry.host.healthy ? 'HOST HEALTHY' : 'HOST DEGRADED'}</span>
                <strong>{selectedTelemetry.host.encoderCodec ?? 'Encoder idle'}</strong>
                <code>
                  Uptime {Math.floor(selectedTelemetry.host.uptimeMs / 3_600_000)}h /{' '}
                  {selectedTelemetry.host.captureFps == null
                    ? 'No active capture'
                    : `${selectedTelemetry.host.captureFps.toFixed(1)} FPS capture`}
                  {selectedTelemetry.host.encoderLatencyMs == null
                    ? ''
                    : ` / ${selectedTelemetry.host.encoderLatencyMs.toFixed(1)} ms encode`}
                </code>
                <button
                  type="button"
                  onClick={() =>
                    selectedHostId && void loadCoreResource(selectedHostId, 'telemetry')
                  }
                >
                  <RefreshCw size={13} /> Refresh sample
                </button>
              </section>
            )}
          </section>
        ) : activeView === 'game-library' ? (
          <div className={styles.primaryPage}>
            {activeHostBar}
            {hostError && <p className={styles.hostError}>{hostError}</p>}

            {!selectedHost ? (
              <section className={`${styles.emptyState} ${styles.primaryEmpty}`}>
                <div className={styles.emptyIcon}>
                  <Server size={28} />
                </div>
                <div>
                  <h3>Connect your first gaming rig</h3>
                  <p>Add a Sol host to discover games and desktop applications.</p>
                </div>
                <button type="button" onClick={() => setShowAddHost(true)}>
                  Add computer <ArrowRight size={17} />
                </button>
              </section>
            ) : (
              <>
                <WorkspaceManager
                  host={selectedHost}
                  apps={selectedApps}
                  settings={settings}
                  onError={setHostError}
                  onLaunch={(workspaceId, appUuid) => {
                    const app = selectedApps.find((candidate) => candidate.uuid === appUuid)
                    if (!app) {
                      setHostError('Workspace application is unavailable in current library.')
                      return
                    }
                    void handleLaunch(selectedHost.id, app.id, '', workspaceId)
                  }}
                />
                {activeSession && activeSession.state !== 'stopped' && (
                  <div className={`${styles.sessionBanner} ${styles[activeSession.state]}`}>
                    <div>
                      <span>{activeSession.state}</span>
                      <strong>{activeSession.appName || 'Native session'}</strong>
                      <p>{activeSession.message}</p>
                    </div>
                    <button
                      type="button"
                      onClick={() => void handleStopSession(selectedHost.id, settings.quitAppAfter)}
                    >
                      <Square size={13} />
                      {settings.quitAppAfter ? 'Stop host app' : 'Disconnect'}
                    </button>
                  </div>
                )}

                {(!activeSession || activeSession.state === 'stopped') &&
                  selectedHost.currentGameId !== 0 && (
                    <div className={`${styles.sessionBanner} ${styles.connected}`}>
                      <div>
                        <span>host busy</span>
                        <strong>Running host application</strong>
                        <p>Resume it below or stop the host application.</p>
                      </div>
                      <button
                        type="button"
                        onClick={() => void handleStopSession(selectedHost.id, true)}
                      >
                        <Square size={13} /> Stop host app
                      </button>
                    </div>
                  )}

                {featuredApp ? (
                  <section className={styles.gameHero} aria-labelledby="featured-game-title">
                    {featuredApp.artDataUrl && (
                      <img className={styles.gameHeroArtwork} src={featuredApp.artDataUrl} alt="" />
                    )}
                    <div className={styles.gameHeroShade} />
                    <div className={styles.gameHeroContent}>
                      <p>
                        <span>{runningApp ? 'READY TO RESUME' : 'READY TO PLAY'}</span>
                        {settings.height >= 2160 ? '4K' : `${settings.height}P`} · {settings.fps}{' '}
                        FPS {settings.enableHdr ? 'HDR' : 'SDR'}
                      </p>
                      <h2 id="featured-game-title">{featuredApp.name}</h2>
                    </div>
                    <div className={styles.gameHeroActions}>
                      <button
                        className={styles.heroLaunch}
                        type="button"
                        disabled={sessionBusy}
                        onClick={() => void handleLaunch(selectedHost.id, featuredApp.id)}
                      >
                        <Play size={18} fill="currentColor" />
                        {sessionBusy ? 'Streaming' : runningApp ? 'Resume' : 'Launch'}
                      </button>
                      <button
                        type="button"
                        aria-label={`View ${featuredApp.name} details`}
                        onClick={() => openAppDetails(featuredApp.id)}
                      >
                        <EllipsisVertical size={19} />
                      </button>
                    </div>
                  </section>
                ) : (
                  <section className={styles.libraryLoading}>
                    {library.state === 'loading' ? <span /> : <Library size={20} />}
                    <div>
                      <strong>
                        {library.state === 'loading'
                          ? `Reading library from ${selectedHost.name}`
                          : 'Library is ready to sync'}
                      </strong>
                      {library.state !== 'loading' && (
                        <button
                          type="button"
                          onClick={() => void handleOpenLibrary(selectedHost.id)}
                        >
                          Load applications
                        </button>
                      )}
                    </div>
                  </section>
                )}

                {selectedApps.length > 0 && (
                  <>
                    <div className={styles.libraryToolbar}>
                      <label className={styles.librarySearch}>
                        <Search size={16} />
                        <span className={styles.visuallyHidden}>Filter library</span>
                        <input
                          value={libraryQuery}
                          placeholder={`Filter games or applications on ${selectedHost.name}...`}
                          onChange={(event) => setLibraryQuery(event.currentTarget.value)}
                        />
                      </label>
                      <fieldset className={styles.libraryFilters} aria-label="Library type filters">
                        {(['all', 'games', 'apps', 'installed', 'updates'] as const).map(
                          (filter) => (
                            <button
                              type="button"
                              key={filter}
                              aria-pressed={libraryFilter === filter}
                              onClick={() => setLibraryFilter(filter)}
                            >
                              {filter === 'all'
                                ? `All (${selectedApps.length})`
                                : filter === 'updates'
                                  ? `Updates (${selectedApps.filter((app) => app.updateAvailable).length})`
                                  : filter}
                            </button>
                          ),
                        )}
                      </fieldset>
                      <label className={styles.librarySort}>
                        <span className={styles.visuallyHidden}>Sort library</span>
                        <select
                          aria-label="Sort library"
                          value={librarySort}
                          onChange={(event) =>
                            setLibrarySort(event.currentTarget.value as 'host' | 'name' | 'updates')
                          }
                        >
                          <option value="host">Host order</option>
                          <option value="name">A–Z</option>
                          <option value="updates">Updates first</option>
                        </select>
                      </label>
                    </div>

                    <section className={styles.appsSection} aria-labelledby="apps-heading">
                      <h2 className={styles.visuallyHidden} id="apps-heading">
                        Applications
                      </h2>
                      {visibleApps.length === 0 ? (
                        <div className={styles.libraryLoading}>
                          No applications match this filter.
                        </div>
                      ) : (
                        <div className={styles.gameGrid}>
                          {visibleApps.map((app, index) => {
                            const running = selectedHost.currentGameId === app.id
                            const launching =
                              activeSession?.appId === app.id && activeSession.state === 'launching'
                            const blocked =
                              sessionBusy ||
                              (selectedHost.currentGameId !== 0 &&
                                selectedHost.currentGameId !== app.id)
                            return (
                              <article className={styles.gameCard} key={app.id}>
                                <div className={styles.gameArtwork}>
                                  {app.artDataUrl ? (
                                    <img src={app.artDataUrl} alt="" />
                                  ) : (
                                    <div className={styles.artPlaceholder}>
                                      <span>{String(index + 1).padStart(2, '0')}</span>
                                      {app.artError ? (
                                        <ImageOff size={25} />
                                      ) : (
                                        <span className={styles.artPulse} />
                                      )}
                                    </div>
                                  )}
                                  <span className={styles.appId}>
                                    {app.hdrSupported ? 'HDR' : `${settings.height}P`}
                                  </span>
                                  {running && (
                                    <strong className={styles.runningBadge}>RUNNING</strong>
                                  )}
                                </div>
                                <div className={styles.gameMeta}>
                                  <div>
                                    <h3>{app.name || `Application ${app.id}`}</h3>
                                    <p>
                                      {app.kind && app.kind !== 'unknown'
                                        ? app.kind.toUpperCase()
                                        : app.appCollectorGame
                                          ? 'GAME'
                                          : 'DESKTOP APP'}
                                      {app.updateAvailable ? ' · UPDATE' : ''}
                                    </p>
                                  </div>
                                  <span className={styles.gameCardActions}>
                                    <button
                                      type="button"
                                      onClick={() => openAppDetails(app.id)}
                                      aria-label={`View ${app.name} details`}
                                    >
                                      <Info size={14} />
                                    </button>
                                    <button
                                      type="button"
                                      disabled={launching || blocked || app.installed === false}
                                      onClick={() => void handleLaunch(selectedHost.id, app.id)}
                                      aria-label={`${running ? 'Resume' : 'Launch'} ${app.name}`}
                                    >
                                      <Play size={14} fill="currentColor" />
                                    </button>
                                  </span>
                                </div>
                              </article>
                            )
                          })}
                        </div>
                      )}
                    </section>
                  </>
                )}

                <section className={styles.performanceStrip} aria-label="Active stream profile">
                  <span>
                    <Gauge className={styles.performanceIcon} size={14} /> TARGET{' '}
                    <strong>
                      {settings.width} × {settings.height}
                    </strong>
                  </span>
                  <span>
                    FRAME RATE <strong>{settings.fps} FPS</strong>
                  </span>
                  <span>
                    BITRATE <strong>{(settings.bitrateKbps / 1000).toFixed(1)} Mbps</strong>
                  </span>
                  <span>
                    CODEC <strong>{profileCodec}</strong>
                  </span>
                  <span>
                    INPUT <strong>1000 Hz</strong>
                  </span>
                  {selectedTelemetry && (
                    <span>
                      HOST{' '}
                      <strong>
                        {selectedTelemetry.host.healthy ? 'HEALTHY' : 'DEGRADED'} ·{' '}
                        {selectedTelemetry.host.activeLogicalSessions} SESSION
                        {selectedTelemetry.host.activeLogicalSessions === 1 ? '' : 'S'}
                      </strong>
                    </span>
                  )}
                </section>
              </>
            )}
          </div>
        ) : activeView === 'workspaces' ? (
          <div className={styles.primaryPage}>
            {activeHostBar}
            {hostError && <p className={styles.hostError}>{hostError}</p>}
            {!selectedHost ? (
              <section className={`${styles.emptyState} ${styles.primaryEmpty}`}>
                <div className={styles.emptyIcon}>
                  <Monitor size={28} />
                </div>
                <div>
                  <h3>No workstation linked</h3>
                  <p>Add a Sol host to build your remote workspace.</p>
                </div>
                <button type="button" onClick={() => setShowAddHost(true)}>
                  Add workstation <ArrowRight size={17} />
                </button>
              </section>
            ) : (
              <>
                {activeSession && activeSession.state !== 'stopped' && (
                  <div className={`${styles.sessionBanner} ${styles[activeSession.state]}`}>
                    <div>
                      <span>{activeSession.state}</span>
                      <strong>{activeSession.appName || 'Remote desktop'}</strong>
                      <p>{activeSession.message}</p>
                    </div>
                    <button
                      type="button"
                      onClick={() => void handleStopSession(selectedHost.id, settings.quitAppAfter)}
                    >
                      <Square size={13} /> Disconnect
                    </button>
                  </div>
                )}

                <div className={styles.workspaceLayout}>
                  <section className={styles.spatialPanel} aria-labelledby="spatial-heading">
                    <div className={styles.spatialHeading}>
                      <p id="spatial-heading">
                        <i /> SPATIAL ARRANGEMENT
                      </p>
                      <code>HOST ID: {selectedHost.serverUniqueId || selectedHost.id}</code>
                    </div>
                    <div className={styles.displayStage}>
                      <article className={styles.localDisplay}>
                        <span>#2</span>
                        <Keyboard size={17} />
                        <strong>Local Dock</strong>
                        <small>INPUT BRIDGE</small>
                      </article>
                      <article className={styles.primaryDisplay}>
                        <span>
                          <b>#1 PRIMARY</b> ACTIVE STREAM
                        </span>
                        <Maximize2 size={17} />
                        <strong>{selectedHost.serverName || selectedHost.name}</strong>
                        <p>
                          {settings.width} × {settings.height} (
                          {settings.width / settings.height > 2 ? '21:9' : '16:9'})
                        </p>
                        <small>
                          {settings.fps} HZ · {settings.enableHdr ? 'HDR10' : 'SDR'}
                        </small>
                        <footer>
                          {profileCodec} · DISPLAY {settings.displayIndex + 1}
                        </footer>
                      </article>
                      <button className={styles.virtualDisplay} type="button" disabled>
                        <Monitor size={20} />
                        <span>+ Virtual display</span>
                        <small>COMING SOON</small>
                      </button>
                    </div>
                    <div className={styles.canvasStatus}>
                      <span>
                        Stream Canvas: {settings.width} × {settings.height} px
                      </span>
                      <i />
                      <strong>
                        Absolute Cursor: {settings.absoluteMouseMode ? 'Enabled' : 'Disabled'}
                      </strong>
                    </div>
                  </section>

                  <aside className={styles.workspaceRail} aria-label="Workspace applications">
                    <div className={styles.workspaceRailHeading}>
                      <div>
                        <p>DESKTOP LAUNCH</p>
                        <h2>Applications</h2>
                      </div>
                      <button
                        type="button"
                        aria-label="Refresh workspace applications"
                        onClick={() => void handleOpenLibrary(selectedHost.id)}
                      >
                        <RefreshCw size={14} />
                      </button>
                    </div>
                    {library.state === 'loading' || library.hostId !== selectedHost.id ? (
                      <div className={styles.workspaceLoading}>
                        <span /> Loading workspace
                      </div>
                    ) : selectedApps.length === 0 ? (
                      <div className={styles.workspaceLoading}>No applications available.</div>
                    ) : (
                      <div className={styles.workspaceApps}>
                        {selectedApps.map((app) => {
                          const running = selectedHost.currentGameId === app.id
                          return (
                            <button
                              type="button"
                              key={app.id}
                              disabled={sessionBusy && activeSession?.appId !== app.id}
                              onClick={() => void handleLaunch(selectedHost.id, app.id)}
                            >
                              <span className={styles.workspaceAppArt}>
                                {app.artDataUrl ? (
                                  <img src={app.artDataUrl} alt="" />
                                ) : (
                                  <Monitor size={18} />
                                )}
                              </span>
                              <span className={styles.workspaceAppCopy}>
                                <strong>{app.name}</strong>
                                <small>{running ? 'READY TO RESUME' : 'OPEN REMOTELY'}</small>
                              </span>
                              <Play
                                className={styles.workspaceAppLaunchIcon}
                                size={13}
                                fill="currentColor"
                              />
                            </button>
                          )
                        })}
                      </div>
                    )}
                  </aside>
                </div>
              </>
            )}
          </div>
        ) : (
          <section className={styles.librarySection}>
            <div className={styles.sectionHeading}>
              <div>
                <p className={styles.eyebrow}>PAIRED COMPUTERS</p>
                <h2>Available hosts</h2>
              </div>
              <span>{onlineCount.toString().padStart(2, '0')} ONLINE</span>
            </div>
            {hostError && <p className={styles.hostError}>{hostError}</p>}
            {hosts.length === 0 ? (
              <div className={styles.emptyState}>
                <div className={styles.emptyIcon}>
                  <Radio size={28} />
                </div>
                <div>
                  <h3>No computers added</h3>
                  <p>Enter a host address. Native core will probe and remember it.</p>
                </div>
                <button type="button" onClick={() => setShowAddHost(true)}>
                  Add first computer <ArrowRight size={17} />
                </button>
              </div>
            ) : (
              <div className={styles.hostGrid}>
                {hosts.map((host) => (
                  <article
                    className={`${styles.hostCard} ${selectedHostId === host.id ? styles.hostSelected : ''}`}
                    key={host.id}
                  >
                    <div className={styles.hostScreen}>
                      <span>HOST</span>
                      <MonitorUp size={34} />
                    </div>
                    <div className={styles.hostDetails}>
                      <span className={`${styles.onlineDot} ${styles[host.status]}`}>
                        {host.status}
                      </span>
                      <h3>{host.name}</h3>
                      <code>{host.address}</code>
                      {host.serverName && (
                        <small>
                          {host.serverName}
                          {host.appVersion ? ` / ${host.appVersion}` : ''}
                        </small>
                      )}
                      {host.apiVersion === 1 && (
                        <small>
                          SOL API V1 / {host.apiScopes?.length ?? 0} SCOPE
                          {host.apiScopes?.length === 1 ? '' : 'S'}
                        </small>
                      )}
                      {host.error && <p className={styles.probeError}>{host.error}</p>}
                      <div className={styles.hostActions}>
                        {host.status === 'offline' && host.wakeable && (
                          <button type="button" onClick={() => void handleWake(host.id)}>
                            <Power size={13} /> Wake
                          </button>
                        )}
                        <button type="button" onClick={() => void refreshCoreHost(host.id)}>
                          <RefreshCw size={13} /> Refresh
                        </button>
                        <button type="button" onClick={() => void removeCoreHost(host.id)}>
                          <Trash2 size={13} /> Remove
                        </button>
                      </div>
                      <div className={styles.hostPrimaryActions}>
                        <button
                          type="button"
                          disabled={host.paired || host.status !== 'online'}
                          onClick={() => void handlePair(host.id)}
                        >
                          <KeyRound size={13} />
                          {host.paired
                            ? 'Paired'
                            : host.status === 'pairing'
                              ? 'Pairing...'
                              : 'Pair computer'}
                        </button>
                        <button
                          className={styles.libraryButton}
                          type="button"
                          disabled={!host.paired || host.status !== 'online'}
                          onClick={() => void handleOpenLibrary(host.id)}
                        >
                          <Library size={13} /> Browse games
                        </button>
                      </div>
                    </div>
                  </article>
                ))}
              </div>
            )}
          </section>
        )}
      </main>

      {showAddHost && (
        <div className={styles.dialogBackdrop} role="presentation">
          <section
            className={styles.dialog}
            role="dialog"
            aria-modal="true"
            aria-labelledby="add-host-title"
          >
            <button
              className={styles.dialogClose}
              type="button"
              aria-label="Close"
              onClick={() => setShowAddHost(false)}
            >
              <X size={20} />
            </button>
            <p className={styles.panelLabel}>MANUAL HOST</p>
            <h2 id="add-host-title">Add computer</h2>
            <p>Save a Sol endpoint. Native core probes server information and persists it.</p>
            {hostError && <p className={styles.dialogError}>{hostError}</p>}
            <form onSubmit={handleAddHost}>
              <label>
                Display name
                <input name="name" placeholder="Studio PC" autoComplete="off" required />
              </label>
              <label>
                Host or IP address
                <input name="address" placeholder="192.168.1.40" autoComplete="off" required />
              </label>
              <Button type="submit">
                Save computer
                <ArrowRight size={17} />
              </Button>
            </form>
          </section>
        </div>
      )}

      {pairing && (
        <div className={styles.dialogBackdrop} role="presentation">
          <section
            className={`${styles.dialog} ${styles.pairingDialog}`}
            role="dialog"
            aria-modal="true"
            aria-labelledby="pairing-title"
          >
            <button
              className={styles.dialogClose}
              type="button"
              aria-label="Close pairing"
              onClick={clearPairing}
            >
              <X size={20} />
            </button>
            <p className={styles.panelLabel}>SECURE PAIRING</p>
            <h2 id="pairing-title">Pair {pairingHost?.name ?? 'computer'}</h2>
            {pairing.state === 'pairing' && (
              <>
                <p>
                  Open Sol web interface, review this {appMode} access request, then enter the PIN.
                </p>
                <div className={styles.pairingAccess}>
                  <strong>
                    {appMode === 'workstation' ? 'WORKSTATION CONTROL' : 'GAMING ACCESS'}
                  </strong>
                  <span>
                    {appMode === 'workstation'
                      ? 'Catalog, sessions, telemetry, displays, virtual displays, peripherals, sandboxes, and host control'
                      : 'Catalog, stream launch, session control, and telemetry'}
                  </span>
                </div>
                <output className={styles.pinCode} aria-label={`Pairing PIN ${pairing.pin}`}>
                  {(['thousands', 'hundreds', 'tens', 'ones'] as const).map((position, index) => (
                    <span key={position}>{pairing.pin.charAt(index)}</span>
                  ))}
                </output>
                <p className={styles.pairingMessage}>{pairing.message}</p>
              </>
            )}
            {pairing.state === 'paired' && (
              <div className={`${styles.pairingResult} ${styles.pairingSuccess}`}>
                <KeyRound size={24} />
                <strong>Identity verified</strong>
                <p>{pairing.message}</p>
              </div>
            )}
            {pairing.state === 'error' && (
              <div className={`${styles.pairingResult} ${styles.pairingFailure}`}>
                <X size={24} />
                <strong>Pairing failed</strong>
                <p>{pairing.message}</p>
              </div>
            )}
          </section>
        </div>
      )}

      {selectedApp && selectedHost && (
        <div className={styles.dialogBackdrop} role="presentation">
          <section
            className={`${styles.dialog} ${styles.gameDetailDialog}`}
            role="dialog"
            aria-modal="true"
            aria-labelledby="game-detail-title"
          >
            <button
              className={styles.dialogClose}
              type="button"
              aria-label="Close application details"
              onClick={closeAppDetails}
            >
              <X size={20} />
            </button>
            <div className={styles.gameDetailLayout}>
              <div className={styles.gameDetailArtwork}>
                {selectedApp.artDataUrl ? (
                  <img src={selectedApp.artDataUrl} alt="" />
                ) : (
                  <Library size={38} />
                )}
              </div>
              <div className={styles.gameDetailCopy}>
                <p className={styles.panelLabel}>
                  {selectedApp.kind?.toUpperCase() || 'APPLICATION'} /{' '}
                  {selectedHost.serverName || selectedHost.name}
                </p>
                <h2 id="game-detail-title">{selectedApp.name}</h2>
                <p>
                  {selectedApp.description ||
                    'Sol did not publish a description for this application.'}
                </p>
                <dl className={styles.gameDetailFacts}>
                  <div>
                    <dt>Publisher</dt>
                    <dd>{selectedApp.publisher || 'Unknown'}</dd>
                  </div>
                  <div>
                    <dt>Source</dt>
                    <dd>{selectedApp.source || 'Host catalog'}</dd>
                  </div>
                  <div>
                    <dt>Availability</dt>
                    <dd>{selectedApp.installed === false ? 'Not installed' : 'Installed'}</dd>
                  </div>
                  <div>
                    <dt>Input</dt>
                    <dd>{selectedApp.inputRequirements?.join(', ') || 'No special requirement'}</dd>
                  </div>
                </dl>
                {selectedApp.tags && selectedApp.tags.length > 0 && (
                  <div className={styles.gameDetailTags}>
                    {selectedApp.tags.map((tag) => (
                      <span key={tag}>{tag}</span>
                    ))}
                  </div>
                )}
                <div className={styles.gameProfileSummary}>
                  <ShieldCheck size={18} />
                  <div>
                    <strong>
                      {selectedApp.streamProfileId
                        ? selectedProfiles.find(
                            (profile) => profile.id === selectedApp.streamProfileId,
                          )?.name || 'Sol stream profile'
                        : 'Terra local stream profile'}
                    </strong>
                    <small>
                      {selectedApp.streamProfileId
                        ? 'Host profile synchronized with Terra transport at launch.'
                        : `${settings.width}×${settings.height} / ${settings.fps} Hz / ${profileCodec}`}
                    </small>
                  </div>
                </div>
                {selectedApp.launchProfiles && selectedApp.launchProfiles.length > 0 && (
                  <label className={styles.gameLaunchProfile}>
                    Launch configuration
                    <select
                      value={selectedLaunchProfileId}
                      onChange={(event) => setSelectedLaunchProfileId(event.currentTarget.value)}
                    >
                      <option value="">Sol default</option>
                      {selectedApp.launchProfiles.map((profile) => (
                        <option value={profile.id} key={profile.id}>
                          {profile.name}
                          {profile.default ? ' (default)' : ''}
                        </option>
                      ))}
                    </select>
                  </label>
                )}
                {selectedApp.updateAvailable && (
                  <p className={styles.gameUpdateNotice}>Update available on host.</p>
                )}
                <footer className={styles.gameDetailActions}>
                  <button type="button" onClick={closeAppDetails}>
                    Back
                  </button>
                  <button
                    className={styles.heroLaunch}
                    type="button"
                    disabled={sessionBusy || selectedApp.installed === false}
                    onClick={() =>
                      void handleLaunch(selectedHost.id, selectedApp.id, selectedLaunchProfileId)
                    }
                  >
                    <Play size={16} fill="currentColor" />
                    {selectedHost.currentGameId === selectedApp.id
                      ? 'Resume stream'
                      : 'Launch stream'}
                  </button>
                </footer>
              </div>
            </div>
          </section>
        </div>
      )}
    </div>
  )
}
