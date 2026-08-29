import {
  Activity,
  ArrowRight,
  CircleHelp,
  Gamepad2,
  ImageOff,
  KeyRound,
  Library,
  MonitorUp,
  Play,
  Plus,
  Radio,
  RefreshCw,
  Settings,
  Square,
  Trash2,
  Wifi,
  X,
} from 'lucide-react'
import { type FormEvent, useEffect, useState } from 'react'
import styles from './App.module.css'
import {
  applyUiDisplayMode,
  cancelCoreSession,
  launchCoreApp,
  loadCoreApps,
  pairCoreHost,
  refreshCoreHost,
  removeCoreHost,
  saveCoreHost,
  startCoreBridge,
} from './native/coreBridge'
import { SettingsView } from './SettingsView'
import { useClientStore } from './store/clientStore'

export function App() {
  const bridge = useClientStore((state) => state.bridge)
  const hosts = useClientStore((state) => state.hosts)
  const hostError = useClientStore((state) => state.hostError)
  const pairing = useClientStore((state) => state.pairing)
  const library = useClientStore((state) => state.library)
  const session = useClientStore((state) => state.session)
  const settings = useClientStore((state) => state.settings)
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
  const [activeView, setActiveView] = useState<'library' | 'settings'>('library')

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
      setHostError('Eclipse could not apply the selected GUI display mode.')
    })
  }, [setHostError, settings.uiDisplayMode])

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
    } catch (error) {
      setHostError(
        error instanceof Error ? error.message : 'Native core did not accept app listing.',
      )
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

  async function handleStopSession(hostId: string) {
    setHostError(undefined)
    try {
      await cancelCoreSession(hostId)
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

  return (
    <div className={styles.appShell}>
      <aside className={styles.sidebar}>
        <div className={styles.brand}>
          <span className={styles.brandMark} aria-hidden="true">
            <span />
          </span>
          <div>
            <strong>ECLIPSE</strong>
            <small>OPEN STREAM CLIENT</small>
          </div>
        </div>

        <nav className={styles.navigation} aria-label="Primary navigation">
          <button
            className={activeView === 'library' ? styles.navActive : undefined}
            type="button"
            onClick={() => setActiveView('library')}
          >
            <Library size={18} />
            Library
          </button>
          <button type="button" disabled>
            <Activity size={18} />
            Sessions
            <span>SOON</span>
          </button>
          <button
            className={activeView === 'settings' ? styles.navActive : undefined}
            type="button"
            onClick={() => setActiveView('settings')}
          >
            <Settings size={18} />
            Settings
          </button>
        </nav>

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
              {activeView === 'settings'
                ? 'LOCAL PROFILE / CLIENT 001'
                : 'LOCAL NETWORK / CLIENT 001'}
            </p>
            <h1>{activeView === 'settings' ? 'Settings' : 'Stream library'}</h1>
          </div>
          <div className={styles.headerActions}>
            <div className={`${styles.corePill} ${styles[bridge.state]}`}>
              <span />
              {bridge.label}
            </div>
            {activeView === 'library' && (
              <button
                className={styles.primaryButton}
                type="button"
                onClick={() => setShowAddHost(true)}
              >
                <Plus size={18} />
                Add computer
              </button>
            )}
          </div>
        </header>

        {activeView === 'settings' ? (
          <SettingsView />
        ) : (
          <>
            <section className={styles.signalPanel}>
              <div className={styles.orbit} aria-hidden="true">
                <span className={styles.orbitMoon} />
                <span className={styles.orbitSignal} />
              </div>
              <div className={styles.signalCopy}>
                <p className={styles.panelLabel}>SUNSHINE READY</p>
                <h2>Your games. One quiet hop away.</h2>
                <p>
                  Add a Sunshine host by address. Native probes, identity verification, and pairing
                  are live; streaming stays native without moving video through the webview.
                </p>
                <div className={styles.capabilities}>
                  <span>
                    <Wifi size={15} /> LAN first
                  </span>
                  <span>
                    <MonitorUp size={15} /> Native render
                  </span>
                  <span>
                    <Gamepad2 size={15} /> Direct input
                  </span>
                </div>
              </div>
            </section>

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
                    Add first computer
                    <ArrowRight size={17} />
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
                            <Library size={13} />
                            Browse games
                          </button>
                        </div>
                      </div>
                    </article>
                  ))}
                </div>
              )}
            </section>

            {selectedHost && (
              <section className={styles.appsSection} aria-labelledby="apps-heading">
                <div className={styles.sectionHeading}>
                  <div>
                    <p className={styles.eyebrow}>
                      LIBRARY / {selectedHost.serverName || selectedHost.name}
                    </p>
                    <h2 id="apps-heading">Applications</h2>
                  </div>
                  <button
                    className={styles.refreshLibrary}
                    type="button"
                    disabled={library.state === 'loading'}
                    onClick={() => void handleOpenLibrary(selectedHost.id)}
                  >
                    <RefreshCw size={14} />
                    Refresh library
                  </button>
                </div>

                {activeSession && activeSession.state !== 'stopped' && (
                  <div className={`${styles.sessionBanner} ${styles[activeSession.state]}`}>
                    <div>
                      <span>{activeSession.state}</span>
                      <strong>
                        {activeSession.appName ||
                          selectedApps.find((app) => app.id === activeSession.appId)?.name ||
                          'Native session'}
                      </strong>
                      <p>{activeSession.message}</p>
                    </div>
                    {(['connected', 'receiving', 'rendering', 'terminated'].includes(
                      activeSession.state,
                    ) ||
                      (activeSession.state === 'error' && selectedHost.currentGameId !== 0)) && (
                      <button type="button" onClick={() => void handleStopSession(selectedHost.id)}>
                        <Square size={13} />
                        {activeSession.state === 'terminated' ? 'Close session' : 'Stop host app'}
                      </button>
                    )}
                  </div>
                )}

                {!activeSession && selectedHost.currentGameId !== 0 && (
                  <div className={`${styles.sessionBanner} ${styles.connected}`}>
                    <div>
                      <span>host busy</span>
                      <strong>Running Sunshine application</strong>
                      <p>Session belongs to an earlier client process and can be stopped safely.</p>
                    </div>
                    <button type="button" onClick={() => void handleStopSession(selectedHost.id)}>
                      <Square size={13} />
                      Stop host app
                    </button>
                  </div>
                )}

                {library.hostId === selectedHost.id && library.state === 'error' && (
                  <p className={styles.hostError}>{library.message}</p>
                )}

                {library.state === 'error' ? null : library.hostId !== selectedHost.id ||
                  library.state === 'loading' ? (
                  <div className={styles.libraryLoading}>
                    <span />
                    Reading paired library from {selectedHost.name}
                  </div>
                ) : selectedApps.length === 0 ? (
                  <div className={styles.libraryLoading}>Sunshine returned no applications.</div>
                ) : (
                  <div className={styles.appGrid}>
                    {selectedApps.map((app, index) => {
                      const running = selectedHost.currentGameId === app.id
                      const launching =
                        activeSession?.appId === app.id && activeSession.state === 'launching'
                      const sessionBusy =
                        activeSession &&
                        activeSession.state !== 'stopped' &&
                        (activeSession.state !== 'error' || selectedHost.currentGameId !== 0)
                      const blockedByAnotherApp =
                        selectedHost.currentGameId !== 0 && selectedHost.currentGameId !== app.id
                      return (
                        <article className={styles.appCard} key={app.id}>
                          <div className={styles.appArtwork}>
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
                            <span className={styles.appId}>APP {app.id}</span>
                            {running && <strong className={styles.runningBadge}>RUNNING</strong>}
                          </div>
                          <div className={styles.appMeta}>
                            <div>
                              <h3>{app.name || `Application ${app.id}`}</h3>
                              <p>
                                {app.hdrSupported ? 'HDR READY' : 'SDR'}
                                {app.appCollectorGame ? ' / COLLECTION' : ''}
                              </p>
                            </div>
                            <button
                              type="button"
                              disabled={launching || sessionBusy || blockedByAnotherApp}
                              onClick={() => void handleLaunch(selectedHost.id, app.id)}
                              aria-label={`${running ? 'Resume' : 'Launch'} ${app.name}`}
                            >
                              <Play size={15} fill="currentColor" />
                              {launching
                                ? 'Starting'
                                : sessionBusy
                                  ? 'Streaming'
                                  : running
                                    ? 'Resume'
                                    : 'Launch'}
                            </button>
                          </div>
                        </article>
                      )
                    })}
                  </div>
                )}
              </section>
            )}
          </>
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
            <p>Save a Sunshine endpoint. Native core probes server information and persists it.</p>
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
              <button className={styles.primaryButton} type="submit">
                Save computer
                <ArrowRight size={17} />
              </button>
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
                <p>Open Sunshine web interface, select PIN, then enter this code now.</p>
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
