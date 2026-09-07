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
  SlidersHorizontal,
  Square,
  Trash2,
  X,
} from 'lucide-react'
import { type FormEvent, useDeferredValue, useEffect, useState } from 'react'
import styles from './App.module.css'
import {
  applyUiDisplayMode,
  cancelCoreSession,
  configureCoreDiscovery,
  launchCoreApp,
  loadCoreApps,
  pairCoreHost,
  refreshCoreHost,
  removeCoreHost,
  saveCoreHost,
  startCoreBridge,
  wakeCoreHost,
} from './native/coreBridge'
import { SettingsView } from './SettingsView'
import { useClientStore } from './store/clientStore'

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

const workstationScaffolds: Partial<
  Record<AppView, { eyebrow: string; title: string; description: string }>
> = {
  'display-topology': {
    eyebrow: 'SPATIAL ARRANGEMENT',
    title: 'Display topology is coming online.',
    description:
      'Connected display modes are available today. Visual arrangement, virtual displays, and seamless cursor topology need native display-control support.',
  },
  'app-sandboxes': {
    eyebrow: 'ISOLATED WORKLOADS',
    title: 'App sandboxes need host support.',
    description:
      'This surface will manage isolated host applications and workspace policies once Sol exposes lifecycle and policy controls.',
  },
  hardware: {
    eyebrow: 'DEVICE BRIDGE',
    title: 'Peripheral inventory is not exposed yet.',
    description:
      'Keyboard, mouse, touch, pen, and controller forwarding already work during streams. Device discovery and USB routing require a native inventory protocol.',
  },
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
  const [showAddHost, setShowAddHost] = useState(false)
  const [selectedHostId, setSelectedHostId] = useState<string>()
  const [libraryQuery, setLibraryQuery] = useState('')
  const [libraryFilter, setLibraryFilter] = useState<'all' | 'games' | 'apps'>('all')
  const [librarySort, setLibrarySort] = useState<'host' | 'name'>('host')
  const deferredLibraryQuery = useDeferredValue(libraryQuery)
  const [activeViews, setActiveViews] = useState<Record<'gaming' | 'workstation', AppView>>({
    gaming: 'game-library',
    workstation: 'workspaces',
  })
  const activeView = activeViews[appMode]

  function setActiveView(activeView: AppView) {
    setActiveViews((current) => ({ ...current, [appMode]: activeView }))
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
      }),
    [setArtwork, setBridge, setHostError, setHosts, setLibrary, setSession, updatePairing],
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
      await pairCoreHost(hostId, pin)
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
    setHostError(undefined)
    try {
      await loadCoreApps(hostId)
      if (activeView === 'paired-hosts') setActiveView('game-library')
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

  async function handleLaunch(hostId: string, appId: number) {
    setHostError(undefined)
    try {
      await launchCoreApp(hostId, appId, settings)
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

  const onlineCount = hosts.filter((host) => host.status === 'online').length
  const pairingHost = pairing ? hosts.find((host) => host.id === pairing.hostId) : undefined
  const selectedHost = hosts.find((host) => host.id === selectedHostId)
  const selectedApps = library.hostId === selectedHostId ? library.apps : []
  const activeSession = session?.hostId === selectedHostId ? session : undefined
  const sessionHost = session ? hosts.find((host) => host.id === session.hostId) : undefined
  const settingsView = activeView === 'stream-settings' || activeView === 'workstation-settings'
  const gamepadView = activeView === 'gamepad'
  const hostsView = activeView === 'paired-hosts'
  const sessionsView = activeView === 'active-sessions'
  const placeholderView = ['display-topology', 'app-sandboxes', 'hardware'].includes(activeView)
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
      return app.name.toLocaleLowerCase().includes(deferredLibraryQuery.trim().toLocaleLowerCase())
    })
    .sort((left, right) => (librarySort === 'name' ? left.name.localeCompare(right.name) : 0))
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
              aria-current={activeView === id ? 'page' : undefined}
              key={id}
              onClick={() => setActiveView(id)}
            >
              <Icon size={18} />
              {label}
              {id === 'game-library' && <span>{library.apps.length || hosts.length}</span>}
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
          <SettingsView section={gamepadView ? 'gamepad' : 'profile'} />
        ) : placeholderView ? (
          <section className={styles.scaffoldPage}>
            <div className={styles.scaffoldGraphic} aria-hidden="true">
              <span />
              <span />
              <span />
            </div>
            <div>
              <p className={styles.panelLabel}>{workstationScaffolds[activeView]?.eyebrow}</p>
              <h2>{workstationScaffolds[activeView]?.title}</h2>
              <p>{workstationScaffolds[activeView]?.description}</p>
              <span className={styles.scaffoldStatus}>NATIVE CAPABILITY PENDING</span>
            </div>
          </section>
        ) : sessionsView ? (
          <section className={styles.sessionsPage}>
            <div className={styles.sectionHeading}>
              <div>
                <p className={styles.eyebrow}>NATIVE STREAM CORE</p>
                <h2>Current session</h2>
              </div>
              <span>{session && session.state !== 'stopped' ? '01 ACTIVE' : '00 ACTIVE'}</span>
            </div>
            {session && session.state !== 'stopped' ? (
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
                      <button type="button" aria-label="More game actions" disabled>
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
                        {(['all', 'games', 'apps'] as const).map((filter) => (
                          <button
                            type="button"
                            key={filter}
                            aria-pressed={libraryFilter === filter}
                            onClick={() => setLibraryFilter(filter)}
                          >
                            {filter === 'all' ? `All (${selectedApps.length})` : filter}
                          </button>
                        ))}
                      </fieldset>
                      <label className={styles.librarySort}>
                        <span className={styles.visuallyHidden}>Sort library</span>
                        <select
                          aria-label="Sort library"
                          value={librarySort}
                          onChange={(event) =>
                            setLibrarySort(event.currentTarget.value as 'host' | 'name')
                          }
                        >
                          <option value="host">Host order</option>
                          <option value="name">A–Z</option>
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
                                    <p>{app.appCollectorGame ? 'GAME' : 'DESKTOP APP'}</p>
                                  </div>
                                  <button
                                    type="button"
                                    disabled={launching || blocked}
                                    onClick={() => void handleLaunch(selectedHost.id, app.id)}
                                    aria-label={`${running ? 'Resume' : 'Launch'} ${app.name}`}
                                  >
                                    <Play size={14} fill="currentColor" />
                                  </button>
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
                <p>Open Sol web interface, select PIN, then enter this code now.</p>
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
    </div>
  )
}
