import { app, events, init, window as neutralinoWindow, storage } from '@neutralinojs/lib'
import {
  Gamepad2,
  Info,
  LogOut,
  MonitorUp,
  Network,
  PanelRightClose,
  PanelRightOpen,
  Power,
  SlidersHorizontal,
  Volume2,
  X,
} from 'lucide-react'
import {
  type KeyboardEvent as ReactKeyboardEvent,
  useEffect,
  useEffectEvent,
  useRef,
  useState,
} from 'react'
import {
  isStreamOverlayShortcut,
  type OverlayCloseAction,
  overlayClosedStorageKey,
  overlayHiddenStorageKey,
  overlayReadyStorageKey,
  overlayRequestStorageKey,
  overlayStatisticsStorageKey,
  type StoredOverlayRequest,
  type StreamStatisticsUpdate,
  storedOverlayRequestSchema,
  streamStatisticsUpdateSchema,
} from './overlayProtocol'
import { StreamCharts } from './StreamCharts'
import styles from './StreamOverlay.module.css'

let neutralinoInitialized = false

declare global {
  interface Window {
    TERRA_OVERLAY_NATIVE?: { close: (action?: OverlayCloseAction) => void }
  }
}

function hasNeutralinoRuntime() {
  return typeof window !== 'undefined' && 'NL_OS' in window
}

export function StreamOverlay() {
  const [request, setRequest] = useState<StoredOverlayRequest>()
  const [statistics, setStatistics] = useState<StreamStatisticsUpdate[]>([])
  const [lastStatisticsAt, setLastStatisticsAt] = useState(0)
  const [telemetryNow, setTelemetryNow] = useState(Date.now())
  const [activeTab, setActiveTab] = useState<TabId>('general')
  const [statsOpen, setStatsOpen] = useState(false)
  const [closeError, setCloseError] = useState(false)
  const requestRef = useRef<StoredOverlayRequest | undefined>(undefined)
  const exitingRef = useRef(false)
  const readyRevisionRef = useRef<string | undefined>(undefined)
  const route = /^\/stream-overlay\/([^/]+)$/.exec(window.location.pathname)
  const expectedRequestId = route?.[1] ? decodeURIComponent(route[1]) : undefined
  const nativeBridge = window.TERRA_OVERLAY_NATIVE

  const exitOverlay = useEffectEvent(
    async (notifyParent: boolean, action: OverlayCloseAction = 'resume') => {
      if (exitingRef.current) return
      exitingRef.current = true
      if (nativeBridge) {
        nativeBridge.close(action)
        return
      }
      const current = requestRef.current
      if (notifyParent && current) {
        try {
          await storage.setData(
            overlayClosedStorageKey,
            JSON.stringify({ schemaVersion: 1, requestId: current.requestId, action }),
          )
          exitingRef.current = false
          setCloseError(false)
          return
        } catch {
          exitingRef.current = false
          setCloseError(true)
          return
        }
      }
      if (current) {
        await neutralinoWindow.hide()
        await storage.setData(
          overlayHiddenStorageKey,
          JSON.stringify({
            schemaVersion: 1,
            requestId: current.requestId,
            revision: current.revision,
          }),
        )
      }
      await app.exit()
    },
  )

  const acceptStatistics = useEffectEvent((value: unknown) => {
    const result = streamStatisticsUpdateSchema.safeParse(value)
    const current = requestRef.current
    if (
      !result.success ||
      !current ||
      current.hostId !== result.data.hostId ||
      current.generation !== result.data.generation
    ) {
      return
    }
    setStatistics((history) => {
      if ((history.at(-1)?.sequence ?? -1) >= result.data.sequence) return history
      return [...history, result.data].slice(-60)
    })
    setLastStatisticsAt(Date.now())
  })

  useEffect(() => {
    const handleStatistics = (event: Event) =>
      acceptStatistics((event as CustomEvent<unknown>).detail)
    window.addEventListener('terra-overlay-statistics', handleStatistics)
    if (nativeBridge || !hasNeutralinoRuntime()) {
      return () => window.removeEventListener('terra-overlay-statistics', handleStatistics)
    }
    const refresh = async () => {
      try {
        acceptStatistics(JSON.parse(await storage.getData(overlayStatisticsStorageKey)))
      } catch {
        // Statistics storage appears after first native sample.
      }
    }
    void refresh()
    const timer = setInterval(() => void refresh(), 200)
    return () => {
      clearInterval(timer)
      window.removeEventListener('terra-overlay-statistics', handleStatistics)
    }
  }, [])

  useEffect(() => {
    const timer = setInterval(() => setTelemetryNow(Date.now()), 1000)
    return () => clearInterval(timer)
  }, [])

  useEffect(() => {
    const handleKeyDown = (event: KeyboardEvent) => {
      if (!isStreamOverlayShortcut(event)) return
      event.preventDefault()
      event.stopPropagation()
      if (!event.repeat) void exitOverlay(true)
    }

    if (nativeBridge) {
      if (expectedRequestId === 'prewarm') {
        const handleRequest = (event: Event) => {
          const result = storedOverlayRequestSchema.safeParse(
            (event as CustomEvent<unknown>).detail,
          )
          if (!result.success || !result.data.visible) return
          exitingRef.current = false
          if (requestRef.current?.requestId !== result.data.requestId) {
            setStatistics([])
            setLastStatisticsAt(0)
          }
          requestRef.current = result.data
          readyRevisionRef.current = result.data.revision
          setRequest(result.data)
        }
        window.addEventListener('terra-overlay-request', handleRequest)
        window.addEventListener('keydown', handleKeyDown, true)
        return () => {
          window.removeEventListener('terra-overlay-request', handleRequest)
          window.removeEventListener('keydown', handleKeyDown, true)
        }
      }
      try {
        const result = storedOverlayRequestSchema.safeParse(
          JSON.parse(decodeURIComponent(window.location.hash.slice(1))),
        )
        if (
          !result.success ||
          result.data.requestId !== expectedRequestId ||
          !result.data.visible
        ) {
          nativeBridge.close()
          return
        }
        if (requestRef.current?.requestId !== result.data.requestId) {
          setStatistics([])
          setLastStatisticsAt(0)
        }
        requestRef.current = result.data
        readyRevisionRef.current = result.data.revision
        setRequest(result.data)
        window.addEventListener('keydown', handleKeyDown, true)
        return () => window.removeEventListener('keydown', handleKeyDown, true)
      } catch {
        nativeBridge.close()
        return
      }
    }

    if (!hasNeutralinoRuntime()) return
    if (!neutralinoInitialized) {
      init()
      neutralinoInitialized = true
    }

    let refreshing = false
    const refresh = async () => {
      if (refreshing) return
      refreshing = true
      try {
        const result = storedOverlayRequestSchema.safeParse(
          JSON.parse(await storage.getData(overlayRequestStorageKey)),
        )
        if (!result.success) return
        if (result.data.requestId !== expectedRequestId) {
          await exitOverlay(false)
          return
        }
        if (
          requestRef.current &&
          BigInt(result.data.revision) <= BigInt(requestRef.current.revision)
        ) {
          return
        }
        if (!result.data.visible) {
          requestRef.current = result.data
          await exitOverlay(false)
          return
        }
        const previousBounds = requestRef.current?.bounds
        if (
          previousBounds &&
          (previousBounds.x !== result.data.bounds.x || previousBounds.y !== result.data.bounds.y)
        ) {
          await neutralinoWindow.move(result.data.bounds.x, result.data.bounds.y)
        }
        if (
          previousBounds &&
          (previousBounds.width !== result.data.bounds.width ||
            previousBounds.height !== result.data.bounds.height)
        ) {
          await neutralinoWindow.setSize({
            width: result.data.bounds.width,
            height: result.data.bounds.height,
          })
        }
        if (readyRevisionRef.current !== result.data.revision) {
          await storage.setData(
            overlayReadyStorageKey,
            JSON.stringify({
              schemaVersion: 1,
              requestId: result.data.requestId,
              revision: result.data.revision,
            }),
          )
          readyRevisionRef.current = result.data.revision
        }
        if (requestRef.current?.requestId !== result.data.requestId) {
          setStatistics([])
          setLastStatisticsAt(0)
        }
        requestRef.current = result.data
        setRequest(result.data)
      } catch {
        // Parent writes request before creating child; retry handles startup races.
      } finally {
        refreshing = false
      }
    }

    const handleWindowClose = () => void exitOverlay(true)

    void events.on('windowClose', handleWindowClose)
    window.addEventListener('keydown', handleKeyDown, true)
    void refresh()
    const timer = setInterval(() => void refresh(), 200)

    return () => {
      clearInterval(timer)
      window.removeEventListener('keydown', handleKeyDown, true)
      void events.off('windowClose', handleWindowClose)
    }
  }, [expectedRequestId])

  const close = (action: OverlayCloseAction = 'resume') => {
    if (!requestRef.current) return
    void exitOverlay(true, action)
  }

  const selectTab = (tab: TabId) => setActiveTab(tab)
  const handleTabKeyDown = (event: ReactKeyboardEvent<HTMLButtonElement>) => {
    const index = tabs.findIndex((tab) => tab.id === activeTab)
    const nextIndex =
      event.key === 'ArrowDown' || event.key === 'ArrowRight'
        ? (index + 1) % tabs.length
        : event.key === 'ArrowUp' || event.key === 'ArrowLeft'
          ? (index - 1 + tabs.length) % tabs.length
          : event.key === 'Home'
            ? 0
            : event.key === 'End'
              ? tabs.length - 1
              : -1
    if (nextIndex < 0) return
    event.preventDefault()
    const nextTab = tabs[nextIndex]
    if (!nextTab) return
    selectTab(nextTab.id)
    document.getElementById(`overlay-tab-${nextTab.id}`)?.focus()
  }

  const stream = request?.stream
  const currentStatistics = statistics.at(-1)?.statistics
  const statisticsLive = lastStatisticsAt > 0 && telemetryNow - lastStatisticsAt < 2500

  return (
    <section className={styles.overlay} role="dialog" aria-modal="true" aria-label="Stream overlay">
      <header className={styles.header}>
        <div className={styles.identity}>
          <span className={styles.signal} aria-hidden="true" />
          <div>
            <strong>TERRA</strong>
            <span>STREAM OVERLAY</span>
          </div>
        </div>
        <div className={styles.headerActions}>
          <button
            type="button"
            className={styles.statsToggle}
            onClick={() => setStatsOpen((open) => !open)}
            aria-expanded={statsOpen}
            aria-controls="overlay-statistics"
          >
            {statsOpen ? <PanelRightClose size={17} /> : <PanelRightOpen size={17} />}
            Stats
          </button>
          <button
            type="button"
            className={styles.close}
            onClick={() => close()}
            aria-label="Close overlay"
          >
            <X size={20} strokeWidth={1.8} />
          </button>
        </div>
      </header>

      <main className={styles.workspace} aria-label="Overlay workspace">
        <div
          className={styles.tabRail}
          role="tablist"
          aria-label="Quick menu sections"
          aria-orientation="vertical"
        >
          <span className={styles.appName}>ACTIVE / {request?.appName || 'STREAM'}</span>
          {tabs.map(({ id, label, icon: Icon }) => (
            <button
              type="button"
              role="tab"
              id={`overlay-tab-${id}`}
              aria-controls={`overlay-panel-${id}`}
              aria-selected={activeTab === id}
              aria-label={label}
              tabIndex={activeTab === id ? 0 : -1}
              onClick={() => selectTab(id)}
              onKeyDown={handleTabKeyDown}
              key={id}
            >
              <Icon size={17} strokeWidth={1.7} />
              <span>{label}</span>
            </button>
          ))}
        </div>

        <section
          className={styles.tabPanel}
          role="tabpanel"
          id={`overlay-panel-${activeTab}`}
          aria-labelledby={`overlay-tab-${activeTab}`}
        >
          <TabContent
            activeTab={activeTab}
            request={request}
            currentStatistics={currentStatistics}
            disconnect={() => close('disconnect')}
            quit={() => close('quit')}
          />
        </section>

        <aside
          className={styles.statisticsPanel}
          data-open={statsOpen}
          id="overlay-statistics"
          aria-label="Live stream statistics"
        >
          <div className={styles.statisticsHeading}>
            <div>
              <span>LIVE TELEMETRY</span>
              <strong>Last 60 seconds</strong>
            </div>
            <span className={styles.liveStatus}>
              {statisticsLive ? 'LIVE' : statistics.length > 0 ? 'STALE' : 'WAITING'}
            </span>
          </div>
          <StreamCharts
            samples={statistics}
            targetFps={stream?.fps}
            bitrateCapKbps={stream?.bitrateKbps}
          />
        </aside>
      </main>

      <footer className={styles.footer}>
        <span>{closeError ? 'CLOSE FAILED / RETRY' : 'CAPTURE LAYER'}</span>
        <kbd>CTRL + SHIFT + ALT + O</kbd>
      </footer>
    </section>
  )
}

type TabId = 'general' | 'video' | 'audio' | 'controllers' | 'network' | 'about'

const tabs = [
  { id: 'general', label: 'General', icon: SlidersHorizontal },
  { id: 'video', label: 'Video & Display', icon: MonitorUp },
  { id: 'audio', label: 'Audio', icon: Volume2 },
  { id: 'controllers', label: 'Controllers & USB', icon: Gamepad2 },
  { id: 'network', label: 'Network', icon: Network },
  { id: 'about', label: 'About', icon: Info },
] as const satisfies readonly { id: TabId; label: string; icon: typeof Info }[]

function flag(value: boolean | undefined) {
  return value === undefined ? 'Unavailable' : value ? 'On' : 'Off'
}

function Row({ label, value, note }: { label: string; value: string; note?: string | undefined }) {
  return (
    <div className={styles.settingRow}>
      <div>
        <strong>{label}</strong>
        {note ? <span>{note}</span> : null}
      </div>
      <output>{value}</output>
    </div>
  )
}

interface TabContentProps {
  activeTab: TabId
  request: StoredOverlayRequest | undefined
  currentStatistics: StreamStatisticsUpdate['statistics'] | undefined
  disconnect: () => void
  quit: () => void
}

function TabContent({ activeTab, request, currentStatistics, disconnect, quit }: TabContentProps) {
  const stream = request?.stream
  if (activeTab === 'general') {
    return (
      <>
        <PanelTitle
          eyebrow="CURRENT SESSION"
          title={request?.appName || 'Waiting for stream'}
          description="Session controls act immediately. Configuration shown in other tabs was fixed when stream launched."
        />
        <div className={styles.summaryGrid}>
          <Summary
            label="Resolution"
            value={stream ? `${stream.width} x ${stream.height}` : '--'}
          />
          <Summary label="Target" value={stream ? `${stream.fps} FPS` : '--'} />
          <Summary label="Codec" value={stream?.codec ?? '--'} />
          <Summary label="Display" value={stream?.displayMode ?? '--'} />
        </div>
        <div className={styles.actionGrid}>
          <button type="button" onClick={disconnect}>
            <LogOut size={18} />
            Disconnect<span>Keep host app running</span>
          </button>
          <button type="button" className={styles.dangerAction} onClick={quit}>
            <Power size={18} />
            End host app<span>Close active app on host</span>
          </button>
        </div>
      </>
    )
  }
  if (activeTab === 'video') {
    return (
      <>
        <PanelTitle
          eyebrow="LAUNCH PROFILE"
          title="Video & Display"
          description="Read-only for active stream. Change these values in Settings before next launch."
        />
        <div className={styles.settingsList}>
          <Row
            label="Resolution"
            value={
              stream ? `${stream.width} x ${stream.height} @ ${stream.fps} FPS` : 'Unavailable'
            }
          />
          <Row
            label="Display mode"
            value={stream?.displayMode ?? 'Unavailable'}
            note={stream ? `Display ${stream.displayIndex + 1}` : undefined}
          />
          <Row label="Video codec" value={stream?.codec ?? 'Unavailable'} />
          <Row label="V-sync" value={flag(stream?.enableVsync)} />
          <Row label="HDR" value={flag(stream?.enableHdr)} />
          <Row label="YUV 4:4:4" value={flag(stream?.enableYuv444)} />
        </div>
      </>
    )
  }
  if (activeTab === 'audio') {
    return (
      <>
        <PanelTitle
          eyebrow="AUDIO ROUTING"
          title="Audio"
          description="Active stream audio format and host playback policy."
        />
        <div className={styles.settingsList}>
          <Row label="Speaker layout" value={stream?.audioConfig ?? 'Unavailable'} />
          <Row label="Mute host speakers" value={flag(stream?.muteHostAudio)} />
          <Row
            label="Live volume control"
            value="Unavailable"
            note="Runtime audio gain is not exposed by native renderer."
          />
        </div>
      </>
    )
  }
  if (activeTab === 'controllers') {
    const controllers = stream
      ? stream.controllerMask.toString(2).replaceAll('0', '').length
      : undefined
    return (
      <>
        <PanelTitle
          eyebrow="INPUT DEVICES"
          title="Controllers & USB"
          description="Controller state captured when quick menu opened."
        />
        <div className={styles.settingsList}>
          <Row
            label="Connected controllers"
            value={controllers === undefined ? 'Unavailable' : String(controllers)}
          />
          <Row
            label="Mouse mode"
            value={stream?.absoluteMouseMode ? 'Absolute' : stream ? 'Relative' : 'Unavailable'}
          />
          <Row label="Capture system keys" value={stream?.captureSystemKeys ?? 'Unavailable'} />
          <Row label="Touchscreen as trackpad" value={flag(stream?.touchscreenTrackpad)} />
          <Row
            label="USB forwarding"
            value="Unavailable"
            note="Terra does not expose USB redirection in this build."
          />
        </div>
      </>
    )
  }
  if (activeTab === 'network') {
    return (
      <>
        <PanelTitle
          eyebrow="TRANSPORT"
          title="Network"
          description="Low-rate telemetry from native Moonlight transport. Loss values describe video frames, not raw IP packets."
        />
        <div className={styles.settingsList}>
          <Row
            label="Configured bitrate"
            value={stream ? `${(stream.bitrateKbps / 1000).toFixed(1)} Mbps` : 'Unavailable'}
          />
          <Row
            label="Current bitrate"
            value={
              currentStatistics ? `${currentStatistics.bitrateMbps.toFixed(1)} Mbps` : 'Waiting'
            }
          />
          <Row
            label="Round-trip latency"
            value={currentStatistics?.rttMs ? `${currentStatistics.rttMs} ms` : 'Waiting'}
          />
          <Row
            label="Network frame loss"
            value={
              currentStatistics ? `${currentStatistics.frameLossPercent.toFixed(2)}%` : 'Waiting'
            }
          />
          <Row
            label="Jitter frame loss"
            value={
              currentStatistics ? `${currentStatistics.jitterLossPercent.toFixed(2)}%` : 'Waiting'
            }
          />
        </div>
      </>
    )
  }
  return (
    <>
      <PanelTitle
        eyebrow="CLIENT"
        title="About Terra"
        description="Terra streams from Sol and compatible hosts, powered by Moonlight common."
      />
      <div className={styles.settingsList}>
        <Row label="Terra client" value="0.1.0" />
        <Row label="Overlay protocol" value="Version 1" />
        <Row label="Telemetry window" value="60 seconds / 1 Hz" />
        <Row label="Session generation" value={request?.generation ?? 'Unavailable'} />
      </div>
    </>
  )
}

function PanelTitle({
  eyebrow,
  title,
  description,
}: {
  eyebrow: string
  title: string
  description: string
}) {
  return (
    <header className={styles.panelTitle}>
      <span>{eyebrow}</span>
      <h1>{title}</h1>
      <p>{description}</p>
    </header>
  )
}

function Summary({ label, value }: { label: string; value: string }) {
  return (
    <div className={styles.summary}>
      <span>{label}</span>
      <strong>{value}</strong>
    </div>
  )
}
