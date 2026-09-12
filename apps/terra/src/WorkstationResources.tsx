import {
  Cable,
  CircleAlert,
  Monitor,
  Play,
  Plus,
  RefreshCw,
  RotateCw,
  Save,
  Square,
  Trash2,
} from 'lucide-react'
import type { FormEvent, ReactNode } from 'react'
import styles from './App.module.css'
import {
  clientDisplayVirtualDisplays,
  effectiveClientDisplay,
  enabledClientDisplays,
  resolvePrimaryClientDisplayId,
} from './clientDisplays'
import { loadCoreResource, mutateCoreResource, rescanClientDisplays } from './native/coreBridge'
import type { StreamSettings } from './settings'
import type {
  ClientDisplayOutput,
  ClientDisplayPreference,
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

interface DisplayModeOption {
  width: number
  height: number
  refreshRate: number
}

function uniqueResolutions(modes: DisplayModeOption[]) {
  const seen = new Set<string>()
  const resolutions: Array<{ width: number; height: number }> = []
  for (const mode of modes) {
    const key = `${mode.width}x${mode.height}`
    if (seen.has(key)) continue
    seen.add(key)
    resolutions.push({ width: mode.width, height: mode.height })
  }
  return resolutions
}

export function DisplayManager({ host, onError }: CommonProps) {
  const outputs = useClientStore((state) => state.clientDisplays)
  const preferences = useClientStore((state) => state.clientDisplayPreferences)
  const primaryDisplayId = useClientStore((state) => state.clientPrimaryDisplayId)
  const setPreference = useClientStore((state) => state.setClientDisplayPreference)
  const setPrimaryDisplay = useClientStore((state) => state.setClientPrimaryDisplay)
  const resetPreferences = useClientStore((state) => state.resetClientDisplayPreferences)
  const streamedDisplays = enabledClientDisplays(outputs, preferences)
  const streamedCount = streamedDisplays.length
  const primaryId = resolvePrimaryClientDisplayId(outputs, preferences, primaryDisplayId)

  function updatePreference(output: ClientDisplayOutput, next: Partial<ClientDisplayPreference>) {
    const current = effectiveClientDisplay(output, preferences[output.id])
    const candidate = { ...current, ...next }
    const modes = output.modes.length
      ? output.modes
      : [{ width: output.width, height: output.height, refreshRate: output.refreshRate }]
    if (!modes.some((mode) => mode.width === candidate.width && mode.height === candidate.height)) {
      candidate.width = output.width
      candidate.height = output.height
    }
    const resolutionModes = modes.filter(
      (mode) => mode.width === candidate.width && mode.height === candidate.height,
    )
    if (!resolutionModes.some((mode) => mode.refreshRate === candidate.refreshRate)) {
      candidate.refreshRate = resolutionModes[0]?.refreshRate ?? output.refreshRate
    }
    setPreference(output.id, candidate)
  }

  return (
    <section className={styles.resourcePage}>
      <div className={styles.sectionHeading}>
        <div>
          <p className={styles.eyebrow}>CLIENT DISPLAY OUTPUTS</p>
          <h2>Streamed display topology</h2>
        </div>
        <div className={styles.resourceHeadingActions}>
          <span>
            {streamedCount.toString().padStart(2, '0')} OF{' '}
            {outputs.length.toString().padStart(2, '0')} STREAMED
          </span>
          <button
            type="button"
            onClick={() => resetPreferences()}
            disabled={Object.keys(preferences).length === 0}
          >
            <RotateCw size={14} /> Reset
          </button>
          <button
            type="button"
            onClick={() =>
              void rescanClientDisplays().catch((error: unknown) =>
                onError(
                  error instanceof Error ? error.message : 'Terra could not rescan displays.',
                ),
              )
            }
          >
            <RefreshCw size={14} /> Rescan
          </button>
        </div>
      </div>
      <p className={styles.resourceFootnote}>
        Terra mirrors the displays reported by this client. Enable a subset to stream, leave the
        rest on the local desktop, and pick a resolution, refresh rate, HDR, and the streamed
        primary per output. Set the primary manually on Linux clients, where the operating system
        does not reliably report it. Up to four outputs stream at once. Adding, removing, or
        rearranging outputs is not supported because the host topology always follows the client.
      </p>
      {outputs.length === 0 ? (
        <div className={styles.resourceEmpty}>
          <CircleAlert size={24} />
          <div>
            <h2>No client displays detected</h2>
            <p>Connect a display and rescan to build a streamed topology.</p>
          </div>
        </div>
      ) : (
        <div className={styles.displayResourceGrid}>
          {outputs.map((output, index) => {
            const preference = effectiveClientDisplay(output, preferences[output.id])
            const modes = output.modes.length
              ? output.modes
              : [{ width: output.width, height: output.height, refreshRate: output.refreshRate }]
            const resolutions = uniqueResolutions(modes)
            const refreshRates = Array.from(
              new Set(
                modes
                  .filter(
                    (mode) => mode.width === preference.width && mode.height === preference.height,
                  )
                  .map((mode) => mode.refreshRate),
              ),
            ).sort((left, right) => left - right)
            const canToggle = preference.enabled ? streamedCount > 1 : streamedCount < 4
            const isPrimary = output.id === primaryId
            return (
              <article className={styles.displayResourceCard} key={output.id}>
                <header>
                  <span>OUTPUT {index + 1}</span>
                  <code>{isPrimary ? 'PRIMARY' : 'SECONDARY'}</code>
                </header>
                <Monitor size={26} />
                <h3>{output.name || `Display ${index + 1}`}</h3>
                <p>
                  {output.width} × {output.height} / {output.refreshRate} Hz
                </p>
                <small>{preference.enabled ? 'STREAMED' : 'LOCAL ONLY'}</small>
                <footer>
                  <label className={styles.displayToggle}>
                    <input
                      type="checkbox"
                      checked={preference.enabled}
                      disabled={!canToggle}
                      onChange={(event) =>
                        updatePreference(output, { enabled: event.currentTarget.checked })
                      }
                    />{' '}
                    Stream this display
                  </label>
                  {preference.enabled && (
                    <>
                      <select
                        aria-label={`Resolution for ${output.name || `Display ${index + 1}`}`}
                        value={`${preference.width}x${preference.height}`}
                        onChange={(event) => {
                          const parts = event.currentTarget.value.split('x')
                          const width = Number(parts[0] ?? 0)
                          const height = Number(parts[1] ?? 0)
                          updatePreference(output, { width, height })
                        }}
                      >
                        {resolutions.map((mode) => (
                          <option
                            value={`${mode.width}x${mode.height}`}
                            key={`${mode.width}x${mode.height}`}
                          >
                            {mode.width}×{mode.height}
                          </option>
                        ))}
                      </select>
                      <select
                        aria-label={`Refresh rate for ${output.name || `Display ${index + 1}`}`}
                        value={preference.refreshRate}
                        onChange={(event) =>
                          updatePreference(output, {
                            refreshRate: Number(event.currentTarget.value),
                          })
                        }
                      >
                        {refreshRates.map((refreshRate) => (
                          <option value={refreshRate} key={refreshRate}>
                            {refreshRate} Hz
                          </option>
                        ))}
                      </select>
                      <label className={styles.displayToggle}>
                        <input
                          type="radio"
                          name="client-primary-display"
                          checked={isPrimary}
                          onChange={() => setPrimaryDisplay(output.id)}
                        />{' '}
                        Primary
                      </label>
                      <label className={styles.displayToggle}>
                        <input
                          type="checkbox"
                          checked={preference.hdr}
                          onChange={(event) =>
                            updatePreference(output, { hdr: event.currentTarget.checked })
                          }
                        />{' '}
                        HDR
                      </label>
                    </>
                  )}
                </footer>
              </article>
            )
          })}
        </div>
      )}
      {host && !host.capabilities?.includes('multi-display-streaming-v1') && (
        <p className={styles.resourceFootnote}>
          This host does not advertise multi-display streaming. Only the primary enabled output is
          used until the host supports it.
        </p>
      )}
    </section>
  )
}

export function WorkspaceManager({
  host,
  apps,
  onError,
  onLaunch,
}: CommonProps & { onLaunch?: (workspaceId: string, appUuid: string) => void }) {
  const resources = useClientStore((state) => (host ? state.resourcesByHost[host.id] : undefined))
  const operation = useClientStore((state) => (host ? state.operationsByHost[host.id] : undefined))
  const workspaces = resources?.workspaces?.workspaces ?? []
  const clientDisplays = useClientStore((state) => state.clientDisplays)
  const clientDisplayPreferences = useClientStore((state) => state.clientDisplayPreferences)
  const clientPrimaryDisplayId = useClientStore((state) => state.clientPrimaryDisplayId)
  const virtualDisplays = clientDisplayVirtualDisplays(
    clientDisplays,
    clientDisplayPreferences,
    clientPrimaryDisplayId,
  )

  async function createWorkspace(event: FormEvent<HTMLFormElement>) {
    event.preventDefault()
    if (!host) return
    const form = event.currentTarget
    const data = new FormData(form)
    const appUuid = String(data.get('appUuid') ?? '')
    const name = String(data.get('name') ?? '').trim()
    if (!appUuid || !name) return
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
        virtualDisplays: [],
        peripheralPolicy: {
          requiredDeviceIds: [],
          requiredClasses: [],
          disconnectPolicy: 'release',
        },
        persistent: true,
        cleanupPolicy: 'on-stop',
      }),
    )
    form.reset()
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
                          disabled={virtualDisplays.length === 0 || virtualDisplays.length > 4}
                          onClick={() =>
                            void runMutation(onError, () =>
                              mutateCoreResource(
                                host.id,
                                'POST',
                                `/eclipse/v1/workspaces/${workspace.id}/start`,
                                { virtualDisplays },
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
            <p className={styles.resourceFootnote}>
              Displays are taken from the client topology on the Display &amp; Topology tab.{' '}
              {virtualDisplays.length} display{virtualDisplays.length === 1 ? '' : 's'} currently
              streamed.
            </p>
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
    const form = event.currentTarget
    const data = new FormData(form)
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
    form.reset()
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
    const form = event.currentTarget
    const data = new FormData(form)
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
    form.reset()
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
    const form = event.currentTarget
    const data = new FormData(form)
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
    form.reset()
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
