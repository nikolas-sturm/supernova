import {
  Cable,
  CircleAlert,
  HardDrive,
  Monitor,
  Play,
  Plus,
  RefreshCw,
  RotateCw,
  Save,
  Square,
  Trash2,
} from 'lucide-react'
import { type FormEvent, type ReactNode, useState } from 'react'
import styles from './App.module.css'
import { loadCoreResource, mutateCoreResource } from './native/coreBridge'
import type { StreamSettings } from './settings'
import type {
  GameApp,
  Host,
  OperationResource,
  ProfileResource,
  SolCollectionResource,
} from './store/clientStore'
import { useClientStore } from './store/clientStore'

interface CommonProps {
  host: Host | undefined
  apps: GameApp[]
  settings: StreamSettings
  onError: (message?: string) => void
}

const workspaceDisplaySlots = ['primary', 'secondary', 'tertiary', 'quaternary'] as const

function featureReason(host: Host, capability: string) {
  const feature = host.features?.[capability]
  if (!feature || typeof feature !== 'object') return undefined
  const value = feature as Record<string, unknown>
  return typeof value.reason === 'string'
    ? value.reason
    : typeof value.reasonCode === 'string'
      ? value.reasonCode.replaceAll('_', ' ')
      : undefined
}

function ResourceGate({
  host,
  scope,
  capability,
  children,
}: {
  host: Host | undefined
  scope: string
  capability: string
  children: ReactNode
}) {
  let title = ''
  let detail = ''
  if (!host) {
    title = 'No workstation selected'
    detail = 'Add and select a Sol host to use this workspace tool.'
  } else if (host.status !== 'online' || !host.paired) {
    title = 'Workstation unavailable'
    detail = 'Selected host must be online and paired.'
  } else if (host.apiVersion !== 1) {
    title = 'Sol API v1 required'
    detail = 'This host supports legacy streaming only.'
  } else if (!host.apiScopes?.includes(scope)) {
    title = 'Pairing permission missing'
    detail = `Re-pair in Workstation mode to grant ${scope}.`
  } else if (!host.capabilities?.includes(capability)) {
    title = 'Host capability unavailable'
    detail = featureReason(host, capability) ?? `${capability} is not operational on this host.`
  }
  if (!title) return children
  return (
    <section className={styles.resourceEmpty}>
      <CircleAlert size={24} />
      <div>
        <h2>{title}</h2>
        <p>{detail}</p>
      </div>
    </section>
  )
}

function OperationBanner({ operation }: { operation: OperationResource | undefined }) {
  if (!operation) return null
  return (
    <div className={`${styles.operationBanner} ${styles[operation.state]}`} role="status">
      <span>{operation.state}</span>
      <code>{operation.id}</code>
      <strong>
        {operation.error?.message ??
          (operation.state === 'succeeded' ? 'Host change applied.' : 'Applying host change...')}
      </strong>
    </div>
  )
}

function ResourceHeading({
  eyebrow,
  title,
  count,
  hostId,
  resources,
}: {
  eyebrow: string
  title: string
  count: number
  hostId: string
  resources: SolCollectionResource[]
}) {
  return (
    <div className={styles.sectionHeading}>
      <div>
        <p className={styles.eyebrow}>{eyebrow}</p>
        <h2>{title}</h2>
      </div>
      <div className={styles.resourceHeadingActions}>
        <span>{count.toString().padStart(2, '0')} RESOURCES</span>
        <button
          type="button"
          aria-label={`Refresh ${title.toLowerCase()}`}
          onClick={() => resources.forEach((resource) => void loadCoreResource(hostId, resource))}
        >
          <RefreshCw size={14} /> Refresh
        </button>
      </div>
    </div>
  )
}

async function runMutation(onError: CommonProps['onError'], mutation: () => Promise<void>) {
  onError(undefined)
  try {
    await mutation()
  } catch (error) {
    onError(error instanceof Error ? error.message : 'Sol rejected workstation change.')
  }
}

export function DisplayManager({ host, settings, onError }: CommonProps) {
  const resources = useClientStore((state) => (host ? state.resourcesByHost[host.id] : undefined))
  const operation = useClientStore((state) => (host ? state.operationsByHost[host.id] : undefined))
  const displays = resources?.displays?.displays ?? []
  const virtualDisplays = resources?.['virtual-displays']?.virtualDisplays ?? []
  const canManage = host?.apiScopes?.includes('display.manage') ?? false

  async function patchDisplay(id: string, revision: number, body: Record<string, unknown>) {
    if (!host) return
    await runMutation(onError, () =>
      mutateCoreResource(host.id, 'PATCH', `/eclipse/v1/displays/${id}`, body, revision),
    )
  }

  async function createVirtualDisplay(event: FormEvent<HTMLFormElement>) {
    event.preventDefault()
    if (!host) return
    const data = new FormData(event.currentTarget)
    const name = String(data.get('name') ?? '').trim()
    if (!name) return
    await runMutation(onError, () =>
      mutateCoreResource(host.id, 'POST', '/eclipse/v1/virtual-displays', {
        name,
        mode: {
          width: settings.width,
          height: settings.height,
          refreshNumerator: settings.fps,
          refreshDenominator: 1,
          bitDepth: settings.enableHdr ? 10 : 8,
          hdr: settings.enableHdr,
        },
        position: { x: settings.width, y: 0 },
        scale: 1,
        rotation: 0,
        primary: false,
        hdr: settings.enableHdr,
        persistent: true,
        workspaceId: null,
      }),
    )
    event.currentTarget.reset()
  }

  return (
    <ResourceGate host={host} scope="display.read" capability="displays-v1">
      {host && (
        <section className={styles.resourcePage}>
          <ResourceHeading
            eyebrow="HOST DISPLAY PLANE"
            title="Connected displays"
            count={displays.length}
            hostId={host.id}
            resources={['displays', 'display-topology', 'virtual-displays']}
          />
          <OperationBanner operation={operation} />
          <div className={styles.displayResourceGrid}>
            {displays.map((display) => (
              <article className={styles.displayResourceCard} key={display.id}>
                <header>
                  <span>{display.kind.toUpperCase()}</span>
                  <code>REV {display.revision}</code>
                </header>
                <Monitor size={26} />
                <h3>{display.name}</h3>
                <p>
                  {display.currentMode
                    ? `${display.currentMode.width} × ${display.currentMode.height} / ${(
                        display.currentMode.refreshNumerator /
                          display.currentMode.refreshDenominator
                      ).toFixed(0)} Hz`
                    : 'Display disabled'}
                </p>
                <small>
                  {display.primary ? 'PRIMARY' : 'SECONDARY'} ·{' '}
                  {display.captureEligible ? 'CAPTURE READY' : 'NOT CAPTURABLE'}
                </small>
                {canManage && display.kind === 'physical' && (
                  <footer>
                    <select
                      aria-label={`Mode for ${display.name}`}
                      value={display.currentMode?.id ?? ''}
                      onChange={(event) =>
                        void patchDisplay(display.id, display.revision, {
                          modeId: event.currentTarget.value,
                        })
                      }
                    >
                      {display.supportedModes.map((mode) => (
                        <option value={mode.id} key={mode.id}>
                          {mode.width}×{mode.height} @{' '}
                          {(mode.refreshNumerator / mode.refreshDenominator).toFixed(0)} Hz
                        </option>
                      ))}
                    </select>
                    {!display.primary && (
                      <button
                        type="button"
                        onClick={() =>
                          void patchDisplay(display.id, display.revision, { primary: true })
                        }
                      >
                        Make primary
                      </button>
                    )}
                    {display.hdr.supported && (
                      <button
                        type="button"
                        onClick={() =>
                          void patchDisplay(display.id, display.revision, {
                            hdr: !display.hdr.enabled,
                          })
                        }
                      >
                        {display.hdr.enabled ? 'Disable HDR' : 'Enable HDR'}
                      </button>
                    )}
                  </footer>
                )}
              </article>
            ))}
          </div>
          {canManage && host.capabilities?.includes('virtual-displays-v1') && (
            <section className={styles.resourceComposer}>
              <div>
                <p className={styles.panelLabel}>VIRTUAL OUTPUT</p>
                <h3>Create from active stream profile</h3>
                <p>
                  {settings.width} × {settings.height} / {settings.fps} Hz ·{' '}
                  {settings.enableHdr ? 'HDR' : 'SDR'}
                </p>
              </div>
              <form onSubmit={createVirtualDisplay}>
                <label>
                  Display name
                  <input name="name" placeholder="Terra virtual display" required />
                </label>
                <button type="submit">
                  <Plus size={14} /> Create display
                </button>
              </form>
            </section>
          )}
          {virtualDisplays.length > 0 && (
            <div className={styles.compactResourceList}>
              {virtualDisplays.map((display) => (
                <article key={display.id}>
                  <HardDrive size={17} />
                  <div>
                    <strong>{display.name}</strong>
                    <small>
                      {display.state} · {display.actualMode.width}×{display.actualMode.height} · REV{' '}
                      {display.revision}
                    </small>
                  </div>
                  {(display.workspaceId || display.sessionId) && (
                    <button
                      type="button"
                      onClick={() =>
                        void runMutation(onError, () =>
                          mutateCoreResource(
                            host.id,
                            'POST',
                            `/eclipse/v1/virtual-displays/${display.id}/detach`,
                            {},
                            display.revision,
                          ),
                        )
                      }
                    >
                      Detach
                    </button>
                  )}
                  <button
                    type="button"
                    aria-label={`Delete ${display.name}`}
                    onClick={() => {
                      if (!window.confirm(`Delete virtual display ${display.name}?`)) return
                      void runMutation(onError, () =>
                        mutateCoreResource(
                          host.id,
                          'DELETE',
                          `/eclipse/v1/virtual-displays/${display.id}`,
                          {},
                          display.revision,
                        ),
                      )
                    }}
                  >
                    <Trash2 size={13} />
                  </button>
                </article>
              ))}
            </div>
          )}
        </section>
      )}
    </ResourceGate>
  )
}

export function WorkspaceManager({
  host,
  apps,
  settings,
  onError,
  onLaunch,
}: CommonProps & { onLaunch?: (workspaceId: string, appUuid: string) => void }) {
  const resources = useClientStore((state) => (host ? state.resourcesByHost[host.id] : undefined))
  const operation = useClientStore((state) => (host ? state.operationsByHost[host.id] : undefined))
  const workspaces = resources?.workspaces?.workspaces ?? []
  const [displayCount, setDisplayCount] = useState(1)
  const [primaryDisplay, setPrimaryDisplay] = useState(0)

  async function createWorkspace(event: FormEvent<HTMLFormElement>) {
    event.preventDefault()
    if (!host) return
    const data = new FormData(event.currentTarget)
    const appUuid = String(data.get('appUuid') ?? '')
    const name = String(data.get('name') ?? '').trim()
    if (!appUuid || !name) return
    const primaryIndex = Number(data.get('primaryDisplay') ?? 0)
    const primaryX = Number(data.get(`displayX-${primaryIndex}`) ?? 0)
    const primaryY = Number(data.get(`displayY-${primaryIndex}`) ?? 0)
    await runMutation(onError, () =>
      mutateCoreResource(host.id, 'POST', '/eclipse/v1/workspaces', {
        name,
        description: '',
        shared: false,
        desktopAppUuid: appUuid,
        permittedAppUuids: [],
        displayProfileId: null,
        streamProfileId: null,
        launchProfileId: null,
        sandboxProfileId: null,
        virtualDisplays: Array.from({ length: displayCount }, (_, index) => {
          const hdr = Boolean(data.get(`displayHdr-${index}`))
          return {
            name: String(data.get(`displayName-${index}`) ?? `Display ${index + 1}`).trim(),
            mode: {
              width: Number(data.get(`displayWidth-${index}`) ?? settings.width),
              height: Number(data.get(`displayHeight-${index}`) ?? settings.height),
              refreshNumerator: Number(data.get(`displayFps-${index}`) ?? settings.fps),
              refreshDenominator: 1,
              bitDepth: hdr ? 10 : 8,
              hdr,
            },
            position: {
              x: Number(data.get(`displayX-${index}`) ?? index * settings.width) - primaryX,
              y: Number(data.get(`displayY-${index}`) ?? 0) - primaryY,
            },
            scale: 1,
            rotation: 0,
            primary: index === primaryIndex,
            hdr,
            persistent: true,
          }
        }),
        peripheralPolicy: {
          requiredDeviceIds: [],
          requiredClasses: [],
          disconnectPolicy: 'release',
        },
        persistent: true,
        cleanupPolicy: 'on-stop',
      }),
    )
    event.currentTarget.reset()
  }

  return (
    <ResourceGate host={host} scope="host.control" capability="workspaces-v1">
      {host && (
        <section className={styles.resourceSection}>
          <ResourceHeading
            eyebrow="DURABLE WORKSPACE DEFINITIONS"
            title="Managed workspaces"
            count={workspaces.length}
            hostId={host.id}
            resources={['workspaces']}
          />
          <OperationBanner operation={operation} />
          {workspaces.length > 0 && (
            <div className={styles.resourceCardGrid}>
              {workspaces.map((workspace) => {
                const app = apps.find((candidate) => candidate.uuid === workspace.desktopAppUuid)
                const active = ['preparing', 'ready', 'active', 'stopping'].includes(
                  workspace.state,
                )
                return (
                  <article className={styles.resourceCard} key={workspace.id}>
                    <header>
                      <span>{workspace.state}</span>
                      <code>REV {workspace.revision}</code>
                    </header>
                    <h3>{workspace.name}</h3>
                    <p>{app?.name ?? workspace.desktopAppUuid}</p>
                    <small>
                      {workspace.displayIds.length} displays · {workspace.peripheralClaimIds.length}{' '}
                      peripherals
                    </small>
                    <footer>
                      {workspace.state === 'ready' && onLaunch && (
                        <button
                          type="button"
                          onClick={() => onLaunch(workspace.id, workspace.desktopAppUuid)}
                        >
                          <Play size={13} /> Launch
                        </button>
                      )}
                      {active ? (
                        <button
                          type="button"
                          onClick={() =>
                            void runMutation(onError, () =>
                              mutateCoreResource(
                                host.id,
                                'POST',
                                `/eclipse/v1/workspaces/${workspace.id}/stop`,
                                { terminateApplication: true },
                                workspace.revision,
                              ),
                            )
                          }
                        >
                          <Square size={13} /> Stop
                        </button>
                      ) : (
                        <button
                          type="button"
                          onClick={() =>
                            void runMutation(onError, () =>
                              mutateCoreResource(
                                host.id,
                                'POST',
                                `/eclipse/v1/workspaces/${workspace.id}/start`,
                                {},
                                workspace.revision,
                              ),
                            )
                          }
                        >
                          <Play size={13} /> Start
                        </button>
                      )}
                      {!active && (
                        <button
                          type="button"
                          aria-label={`Delete workspace ${workspace.name}`}
                          onClick={() => {
                            if (!window.confirm(`Delete workspace ${workspace.name}?`)) return
                            void runMutation(onError, () =>
                              mutateCoreResource(
                                host.id,
                                'DELETE',
                                `/eclipse/v1/workspaces/${workspace.id}`,
                                {},
                                workspace.revision,
                              ),
                            )
                          }}
                        >
                          <Trash2 size={13} /> Delete
                        </button>
                      )}
                    </footer>
                  </article>
                )
              })}
            </div>
          )}
          <form className={styles.inlineResourceForm} onSubmit={createWorkspace}>
            <label>
              Workspace name
              <input name="name" placeholder="Design desk" required />
            </label>
            <label>
              Desktop application
              <select name="appUuid" required defaultValue="">
                <option value="" disabled>
                  Select application
                </option>
                {apps
                  .filter((app) => app.uuid)
                  .map((app) => (
                    <option value={app.uuid} key={app.uuid}>
                      {app.name}
                    </option>
                  ))}
              </select>
            </label>
            {workspaceDisplaySlots.slice(0, displayCount).map((slot, index) => (
              <fieldset key={slot}>
                <legend>{`Display ${index + 1}`}</legend>
                <label>
                  <input
                    name="primaryDisplay"
                    type="radio"
                    value={index}
                    checked={primaryDisplay === index}
                    onChange={() => setPrimaryDisplay(index)}
                  />{' '}
                  Primary
                </label>
                <label>
                  Name
                  <input
                    name={`displayName-${index}`}
                    defaultValue={`Display ${index + 1}`}
                    required
                  />
                </label>
                <label>
                  Width
                  <input
                    name={`displayWidth-${index}`}
                    type="number"
                    min="640"
                    max="16384"
                    defaultValue={settings.width}
                    required
                  />
                </label>
                <label>
                  Height
                  <input
                    name={`displayHeight-${index}`}
                    type="number"
                    min="480"
                    max="16384"
                    defaultValue={settings.height}
                    required
                  />
                </label>
                <label>
                  FPS
                  <input
                    name={`displayFps-${index}`}
                    type="number"
                    min="1"
                    max="480"
                    defaultValue={settings.fps}
                    required
                  />
                </label>
                <label>
                  X
                  <input
                    name={`displayX-${index}`}
                    type="number"
                    defaultValue={index * settings.width}
                    readOnly={index === 0}
                    required
                  />
                </label>
                <label>
                  Y
                  <input
                    name={`displayY-${index}`}
                    type="number"
                    defaultValue={0}
                    readOnly={index === 0}
                    required
                  />
                </label>
                <label>
                  <input
                    name={`displayHdr-${index}`}
                    type="checkbox"
                    defaultChecked={settings.enableHdr}
                  />{' '}
                  HDR
                </label>
              </fieldset>
            ))}
            <button
              type="button"
              disabled={displayCount >= 4}
              onClick={() => setDisplayCount((count) => Math.min(4, count + 1))}
            >
              <Plus size={14} /> Virtual display
            </button>
            {displayCount > 1 && (
              <button
                type="button"
                onClick={() => {
                  setDisplayCount((count) => count - 1)
                  setPrimaryDisplay((index) => Math.min(index, displayCount - 2))
                }}
              >
                Remove display
              </button>
            )}
            <button type="submit" disabled={!apps.some((app) => app.uuid)}>
              <Plus size={14} /> Create workspace
            </button>
          </form>
        </section>
      )}
    </ResourceGate>
  )
}

export function ProfileManager({ host, settings, onError }: CommonProps) {
  const resources = useClientStore((state) => (host ? state.resourcesByHost[host.id] : undefined))
  const operation = useClientStore((state) => (host ? state.operationsByHost[host.id] : undefined))
  const profiles = resources?.profiles?.profiles ?? []

  async function createProfile(event: FormEvent<HTMLFormElement>) {
    event.preventDefault()
    if (!host) return
    const data = new FormData(event.currentTarget)
    const name = String(data.get('name') ?? '').trim()
    if (!name) return
    await runMutation(onError, () =>
      mutateCoreResource(host.id, 'POST', '/eclipse/v1/profiles', {
        type: 'stream',
        name,
        shared: false,
        configuration: {
          width: settings.width,
          height: settings.height,
          fps: settings.fps,
          bitrateKbps: settings.bitrateKbps,
          codec: settings.videoCodec,
          hdr: settings.enableHdr,
          yuv444: settings.enableYuv444,
          audioChannels: settings.audioConfig,
          hostAudio: !settings.muteHostAudio,
          inputMode: settings.absoluteMouseMode ? 'absolute' : 'relative',
          requiredInputClasses: ['keyboard', 'mouse'],
          controllerLimit: settings.forceGamepad ? 4 : 0,
          encryptionRequired: true,
          gameOptimizations: settings.gameOptimizations,
        },
      }),
    )
    event.currentTarget.reset()
  }

  return (
    <ResourceGate host={host} scope="host.control" capability="profiles-v1">
      {host && (
        <section className={styles.resourceSection}>
          <ResourceHeading
            eyebrow="HOST PROFILE LIBRARY"
            title="Reusable host profiles"
            count={profiles.length}
            hostId={host.id}
            resources={['profiles']}
          />
          <OperationBanner operation={operation} />
          <div className={styles.compactResourceList}>
            {profiles.map((profile) => (
              <article key={profile.id}>
                <Save size={17} />
                <div>
                  <strong>{profile.name}</strong>
                  <small>
                    {profile.type.toUpperCase()} · {profile.shared ? 'SHARED' : 'PRIVATE'} · REV{' '}
                    {profile.revision}
                  </small>
                </div>
                <button
                  type="button"
                  aria-label={`Delete profile ${profile.name}`}
                  onClick={() => {
                    if (!window.confirm(`Delete profile ${profile.name}?`)) return
                    void runMutation(onError, () =>
                      mutateCoreResource(
                        host.id,
                        'DELETE',
                        `/eclipse/v1/profiles/${profile.id}`,
                        {},
                        profile.revision,
                      ),
                    )
                  }}
                >
                  <Trash2 size={13} />
                </button>
              </article>
            ))}
          </div>
          <form className={styles.inlineResourceForm} onSubmit={createProfile}>
            <label>
              Profile name
              <input name="name" placeholder="Terra workstation" required />
            </label>
            <p>
              Saves current {settings.width}×{settings.height}, {settings.fps} Hz stream settings on
              Sol.
            </p>
            <button type="submit">
              <Save size={14} /> Save stream profile
            </button>
          </form>
        </section>
      )}
    </ResourceGate>
  )
}

function profileName(profiles: ProfileResource[], id: string) {
  return profiles.find((profile) => profile.id === id)?.name ?? id
}

export function SandboxManager({ host, apps, settings, onError }: CommonProps) {
  const resources = useClientStore((state) => (host ? state.resourcesByHost[host.id] : undefined))
  const operation = useClientStore((state) => (host ? state.operationsByHost[host.id] : undefined))
  const sandboxes = resources?.sandboxes?.sandboxes ?? []
  const profiles = resources?.profiles?.profiles ?? []
  const sandboxProfiles = profiles.filter((profile) => profile.type === 'sandbox')

  async function createSandboxProfile() {
    if (!host) return
    await runMutation(onError, () =>
      mutateCoreResource(host.id, 'POST', '/eclipse/v1/profiles', {
        type: 'sandbox',
        name: 'Terra Windows process boundary',
        shared: false,
        configuration: {
          allowedAppUuids: apps.flatMap((app) => (app.uuid ? [app.uuid] : [])),
          executablePolicy: 'configured-only',
          filesystem: { readOnlyRoots: [], writableRoots: [], denyOther: false },
          environment: { allowedNames: [], values: {} },
          network: { mode: 'full', allowedHosts: [], allowedPorts: [] },
          resources: {
            cpuPercent: null,
            memoryBytes: null,
            processCount: null,
            storageBytes: null,
          },
          gpu: { mode: 'full', encoder: true },
          displays: { allowedIds: [], virtualOnly: false },
          input: { classes: [] },
          peripherals: { deviceIds: [], classes: [] },
          clipboard: 'bidirectional',
          hostIntegration: 'none',
          elevation: 'deny',
          persistentData: { enabled: false, name: null },
          timeoutMs: 0,
          cleanupPolicy: 'delete',
        },
      }),
    )
  }

  async function createSandbox(event: FormEvent<HTMLFormElement>) {
    event.preventDefault()
    if (!host) return
    const data = new FormData(event.currentTarget)
    const name = String(data.get('name') ?? '').trim()
    const profileId = String(data.get('profileId') ?? '')
    const appUuid = String(data.get('appUuid') ?? '')
    if (!name || !profileId) return
    await runMutation(onError, () =>
      mutateCoreResource(host.id, 'POST', '/eclipse/v1/sandboxes', {
        profileId,
        workspaceId: null,
        appUuid: appUuid || null,
        persistent: true,
        name,
      }),
    )
    event.currentTarget.reset()
  }

  return (
    <ResourceGate host={host} scope="sandbox.manage" capability="sandboxes-v1">
      {host && (
        <section className={styles.resourcePage}>
          <ResourceHeading
            eyebrow="RESTRICTED WINDOWS RUNTIMES"
            title="Application sandboxes"
            count={sandboxes.length}
            hostId={host.id}
            resources={['sandboxes', 'profiles']}
          />
          <OperationBanner operation={operation} />
          <div className={styles.resourceCardGrid}>
            {sandboxes.map((sandbox) => {
              const running = ['starting', 'running', 'stopping'].includes(sandbox.state)
              const app = apps.find((candidate) => candidate.uuid === sandbox.appUuid)
              return (
                <article className={styles.resourceCard} key={sandbox.id}>
                  <header>
                    <span>{sandbox.state}</span>
                    <code>REV {sandbox.revision}</code>
                  </header>
                  <h3>{sandbox.name}</h3>
                  <p>
                    {app?.name ??
                      (sandbox.appUuid ? sandbox.appUuid : 'Application selected on start')}
                  </p>
                  <small>POLICY · {profileName(profiles, sandbox.profileId)}</small>
                  {sandbox.error && <p className={styles.resourceError}>{sandbox.error.message}</p>}
                  <footer>
                    {running ? (
                      <>
                        <button
                          type="button"
                          onClick={() =>
                            void runMutation(onError, () =>
                              mutateCoreResource(
                                host.id,
                                'POST',
                                `/eclipse/v1/sandboxes/${sandbox.id}/stop`,
                                { force: true },
                                sandbox.revision,
                              ),
                            )
                          }
                        >
                          <Square size={13} /> Stop
                        </button>
                        <button
                          type="button"
                          onClick={() =>
                            void runMutation(onError, () =>
                              mutateCoreResource(
                                host.id,
                                'POST',
                                `/eclipse/v1/sandboxes/${sandbox.id}/restart`,
                                { force: true },
                                sandbox.revision,
                              ),
                            )
                          }
                        >
                          <RotateCw size={13} /> Restart
                        </button>
                      </>
                    ) : (
                      <button
                        type="button"
                        disabled={!sandbox.appUuid}
                        onClick={() =>
                          void runMutation(onError, () =>
                            mutateCoreResource(
                              host.id,
                              'POST',
                              `/eclipse/v1/sandboxes/${sandbox.id}/start`,
                              {},
                              sandbox.revision,
                            ),
                          )
                        }
                      >
                        <Play size={13} /> Start
                      </button>
                    )}
                    {!running && !sandbox.workspaceId && (
                      <button
                        type="button"
                        onClick={() => {
                          if (!window.confirm(`Delete sandbox ${sandbox.name}?`)) return
                          void runMutation(onError, () =>
                            mutateCoreResource(
                              host.id,
                              'DELETE',
                              `/eclipse/v1/sandboxes/${sandbox.id}`,
                              {},
                              sandbox.revision,
                            ),
                          )
                        }}
                      >
                        <Trash2 size={13} /> Delete
                      </button>
                    )}
                  </footer>
                </article>
              )
            })}
          </div>
          <form className={styles.inlineResourceForm} onSubmit={createSandbox}>
            <label>
              Sandbox name
              <input name="name" placeholder="Restricted application" required />
            </label>
            <label>
              Sandbox policy
              <select name="profileId" required defaultValue="">
                <option value="" disabled>
                  Select profile
                </option>
                {sandboxProfiles.map((profile) => (
                  <option value={profile.id} key={profile.id}>
                    {profile.name}
                  </option>
                ))}
              </select>
            </label>
            <label>
              Application
              <select name="appUuid" required defaultValue="">
                <option value="" disabled>
                  Select application
                </option>
                {apps
                  .filter((app) => app.uuid)
                  .map((app) => (
                    <option value={app.uuid} key={app.uuid}>
                      {app.name}
                    </option>
                  ))}
              </select>
            </label>
            <button type="submit" disabled={sandboxProfiles.length === 0}>
              <Plus size={14} /> Create sandbox
            </button>
            {sandboxProfiles.length === 0 && (
              <button
                type="button"
                disabled={!apps.some((app) => app.uuid)}
                onClick={() => void createSandboxProfile()}
              >
                Create baseline policy
              </button>
            )}
          </form>
          <p className={styles.resourceFootnote}>
            Active stream profile: {settings.width}×{settings.height} / {settings.fps} Hz. Sandbox
            policy uses restricted process token and Job Object limits. Windows provider does not
            enforce filesystem, network, GPU, display, clipboard, or peripheral isolation.
          </p>
        </section>
      )}
    </ResourceGate>
  )
}

export function HardwareManager({ host, onError }: CommonProps) {
  const resources = useClientStore((state) => (host ? state.resourcesByHost[host.id] : undefined))
  const operation = useClientStore((state) => (host ? state.operationsByHost[host.id] : undefined))
  const devices = resources?.peripherals?.peripherals ?? []
  const claims = resources?.['peripheral-claims']?.claims ?? []
  const targets = [
    ...(resources?.sessions?.sessions ?? []).map((session) => ({
      type: 'session' as const,
      id: session.id,
      name: `Session ${session.id.slice(0, 8)}`,
    })),
    ...(resources?.workspaces?.workspaces ?? []).map((workspace) => ({
      type: 'workspace' as const,
      id: workspace.id,
      name: workspace.name,
    })),
    ...(resources?.sandboxes?.sandboxes ?? []).map((sandbox) => ({
      type: 'sandbox' as const,
      id: sandbox.id,
      name: sandbox.name,
    })),
  ]

  async function registerPeripheral(event: FormEvent<HTMLFormElement>) {
    event.preventDefault()
    if (!host) return
    const data = new FormData(event.currentTarget)
    const deviceClass = String(data.get('class')) as 'keyboard' | 'mouse'
    const name = String(data.get('name') ?? '').trim()
    if (!name) return
    await runMutation(onError, () =>
      mutateCoreResource(host.id, 'POST', '/eclipse/v1/peripherals', {
        class: deviceClass,
        platformId: `terra-${deviceClass}-${crypto.randomUUID()}`,
        name,
        vendorId: 0,
        productId: 0,
        capabilities: [`${deviceClass}.hid`],
      }),
    )
    event.currentTarget.reset()
  }

  async function claimPeripheral(event: FormEvent<HTMLFormElement>) {
    event.preventDefault()
    if (!host) return
    const data = new FormData(event.currentTarget)
    const deviceId = String(data.get('deviceId') ?? '')
    const [type, id] = String(data.get('target') ?? '').split(':')
    const device = devices.find((candidate) => candidate.id === deviceId)
    if (!device || !type || !id) return
    await runMutation(onError, () =>
      mutateCoreResource(host.id, 'POST', '/eclipse/v1/peripherals/claims', {
        deviceId,
        target: { type, id },
        requestedCapabilities: device.capabilities,
        exclusive: true,
        disconnectPolicy: 'release',
      }),
    )
  }

  return (
    <ResourceGate host={host} scope="peripheral.forward" capability="peripherals-v1">
      {host && (
        <section className={styles.resourcePage}>
          <ResourceHeading
            eyebrow="REMOTE DEVICE ROUTING"
            title="Peripherals and claims"
            count={devices.length + claims.length}
            hostId={host.id}
            resources={['peripherals', 'peripheral-claims', 'sessions', 'workspaces', 'sandboxes']}
          />
          <OperationBanner operation={operation} />
          <div className={styles.resourceColumns}>
            <section>
              <h3>Registered devices</h3>
              <div className={styles.compactResourceList}>
                {devices.map((device) => (
                  <article key={device.id}>
                    <Cable size={17} />
                    <div>
                      <strong>{device.name}</strong>
                      <small>
                        {device.class.toUpperCase()} · {device.claimable ? 'AVAILABLE' : 'CLAIMED'}
                      </small>
                    </div>
                    <button
                      type="button"
                      aria-label={`Delete peripheral ${device.name}`}
                      onClick={() => {
                        if (!window.confirm(`Remove peripheral ${device.name}?`)) return
                        void runMutation(onError, () =>
                          mutateCoreResource(
                            host.id,
                            'DELETE',
                            `/eclipse/v1/peripherals/${device.id}`,
                            {},
                            device.revision,
                          ),
                        )
                      }}
                    >
                      <Trash2 size={13} />
                    </button>
                  </article>
                ))}
              </div>
              <form className={styles.inlineResourceForm} onSubmit={registerPeripheral}>
                <label>
                  Device name
                  <input name="name" placeholder="Terra keyboard" required />
                </label>
                <label>
                  Class
                  <select name="class">
                    <option value="keyboard">Keyboard</option>
                    <option value="mouse">Mouse</option>
                  </select>
                </label>
                <button type="submit">
                  <Plus size={14} /> Register
                </button>
              </form>
            </section>
            <section>
              <h3>Active claims</h3>
              <div className={styles.compactResourceList}>
                {claims.map((claim) => (
                  <article key={claim.id}>
                    <Cable size={17} />
                    <div>
                      <strong>{claim.deviceClass} route</strong>
                      <small>
                        {claim.state.toUpperCase()} · {claim.target.type}{' '}
                        {claim.target.id.slice(0, 8)}
                      </small>
                    </div>
                    <button
                      type="button"
                      aria-label={`Release claim ${claim.id}`}
                      onClick={() =>
                        void runMutation(onError, () =>
                          mutateCoreResource(
                            host.id,
                            'DELETE',
                            `/eclipse/v1/peripherals/claims/${claim.id}`,
                            {},
                            claim.revision,
                          ),
                        )
                      }
                    >
                      Release
                    </button>
                  </article>
                ))}
              </div>
              <form className={styles.inlineResourceForm} onSubmit={claimPeripheral}>
                <label>
                  Device
                  <select name="deviceId" required defaultValue="">
                    <option value="" disabled>
                      Select device
                    </option>
                    {devices
                      .filter((device) => device.claimable)
                      .map((device) => (
                        <option value={device.id} key={device.id}>
                          {device.name}
                        </option>
                      ))}
                  </select>
                </label>
                <label>
                  Target
                  <select name="target" required defaultValue="">
                    <option value="" disabled>
                      Select target
                    </option>
                    {targets.map((target) => (
                      <option
                        value={`${target.type}:${target.id}`}
                        key={`${target.type}:${target.id}`}
                      >
                        {target.type}: {target.name}
                      </option>
                    ))}
                  </select>
                </label>
                <button
                  type="submit"
                  disabled={!devices.some((device) => device.claimable) || targets.length === 0}
                >
                  Route device
                </button>
              </form>
            </section>
          </div>
          <p className={styles.resourceFootnote}>
            Manual descriptors and claims manage Sol inventory. Normal keyboard, mouse, touch, pen,
            and controller input continues through native GameStream transport.
          </p>
        </section>
      )}
    </ResourceGate>
  )
}
