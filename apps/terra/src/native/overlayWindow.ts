import { events, window as neutralinoWindow, os, storage } from '@neutralinojs/lib'
import {
  type OverlayCloseAction,
  overlayClosedSchema,
  overlayClosedStorageKey,
  overlayHiddenStorageKey,
  overlayPresentationSchema,
  overlayReadyStorageKey,
  overlayRequestStorageKey,
  overlayStatisticsStorageKey,
  type StoredOverlayRequest,
  type StreamOverlayRequest,
  type StreamStatisticsUpdate,
} from '../overlay/overlayProtocol'

type ClosedHandler = (action: OverlayCloseAction) => Promise<void>
type PresentedHandler = (revision: string, visible: boolean) => Promise<void>

let activeRequest: StoredOverlayRequest | undefined
let closedHandler: ClosedHandler | undefined
let presentedHandler: PresentedHandler | undefined
let pollTimer: ReturnType<typeof setInterval> | undefined
let pollRunning = false
let childReady = false
let visibleAcknowledgedRevision: string | undefined
let readyDeadline = 0
let latestStatisticsSequence = -1
let closeRequestRunning = false
let closeRetryAt = 0
let pendingCloseAction: OverlayCloseAction | undefined
let childProcess: { id: number; requestId: string; persistent: boolean } | undefined
let warmProcess: { id: number; ready: boolean } | undefined
let prewarmStarting = false
let watchingSpawnedProcesses = false
const earlyProcessExits = new Set<number>()
const earlyProcessReady = new Set<number>()
const earlyProcessPrewarmed = new Set<number>()
const processOutput = new Map<number, string>()
const hiddenWaiters = new Map<number, { revision: string; resolve: () => void }>()
const confirmedHiddenProcesses = new Set<number>()
const confirmedHiddenRequests = new Set<string>()
let presenterQueue = Promise.resolve()

function requestId() {
  return typeof crypto.randomUUID === 'function'
    ? crypto.randomUUID()
    : `${Date.now()}-${Math.random().toString(16).slice(2)}`
}

function shellQuote(value: string) {
  return `'${value.replaceAll("'", `'"'"'`)}'`
}

function waylandChildCommand(url: string, prewarm = false, revision?: string) {
  const executable = `${window.NL_PATH}/extensions/terra-core/bin/terra-wayland-overlay`
  return [executable, ...(prewarm ? ['--prewarm'] : [revision ?? '0']), url]
    .map(shellQuote)
    .join(' ')
}

async function storeVisibility(request: StoredOverlayRequest, visible: boolean) {
  await storage.setData(
    overlayRequestStorageKey,
    JSON.stringify({ ...request, visible } satisfies StoredOverlayRequest),
  )
}

function enqueuePresentation(operation: () => Promise<void>) {
  const result = presenterQueue.then(operation, operation)
  presenterQueue = result.catch(() => undefined)
  return result
}

async function waitForStoredPresentation(key: string, requestId: string, revision?: string) {
  const deadline = Date.now() + 3000
  while (Date.now() < deadline) {
    try {
      const result = overlayPresentationSchema.safeParse(JSON.parse(await storage.getData(key)))
      if (
        result.success &&
        result.data.requestId === requestId &&
        (revision === undefined || result.data.revision === revision)
      ) {
        return
      }
    } catch {
      // Child writes presentation state after applying window visibility.
    }
    await new Promise((resolve) => setTimeout(resolve, 25))
  }
  throw new Error('Stream overlay did not confirm that its input surface was hidden.')
}

async function hideProcess(
  process: { id: number; requestId: string; persistent: boolean },
  revision: string,
) {
  if (confirmedHiddenProcesses.delete(process.id)) return
  const hidden = new Promise<void>((resolve) => {
    hiddenWaiters.set(process.id, { revision, resolve })
  })
  await os.updateSpawnedProcess(process.id, 'stdIn', `HIDE ${revision}\n`)
  let timeoutId: ReturnType<typeof setTimeout> | undefined
  try {
    await Promise.race([
      hidden,
      new Promise<never>((_, reject) => {
        timeoutId = setTimeout(
          () => reject(new Error('Wayland overlay did not confirm its hidden state.')),
          3000,
        )
      }),
    ])
  } finally {
    if (timeoutId) clearTimeout(timeoutId)
    const waiter = hiddenWaiters.get(process.id)
    if (waiter?.revision === revision) hiddenWaiters.delete(process.id)
  }
}

async function finishOverlay(expectedRequestId: string, revision: string) {
  if (!activeRequest || activeRequest.requestId !== expectedRequestId) return false
  const request = activeRequest
  const closingRequest = { ...request, revision, visible: false }
  activeRequest = closingRequest
  const process = childProcess?.requestId === expectedRequestId ? childProcess : undefined
  await storeVisibility(closingRequest, false).catch(() => undefined)
  if (confirmedHiddenRequests.delete(expectedRequestId)) {
    // UI-originated close may hide before native publishes its hidden revision.
  } else if (process) {
    await hideProcess(process, revision)
  } else {
    await waitForStoredPresentation(overlayHiddenStorageKey, expectedRequestId)
  }
  if (pollTimer) clearInterval(pollTimer)
  pollTimer = undefined
  closedHandler = undefined
  presentedHandler = undefined
  childReady = false
  readyDeadline = 0
  latestStatisticsSequence = -1
  closeRetryAt = 0
  pendingCloseAction = undefined
  if (process) childProcess = undefined
  confirmedHiddenRequests.delete(expectedRequestId)
  if (activeRequest?.requestId === expectedRequestId) activeRequest = undefined
  return true
}

async function requestOverlayClose(action: OverlayCloseAction) {
  if (
    closeRequestRunning ||
    Date.now() < closeRetryAt ||
    !activeRequest?.visible ||
    !closedHandler
  ) {
    return false
  }
  closeRequestRunning = true
  try {
    await closedHandler(action)
    closeRetryAt = 0
    if (pendingCloseAction === action) pendingCloseAction = undefined
    return true
  } catch {
    closeRetryAt = Date.now() + 500
    return false
  } finally {
    closeRequestRunning = false
  }
}

function boundEarlySet(values: Set<number>) {
  if (values.size <= 32) return
  const oldest = values.values().next().value
  if (oldest !== undefined) values.delete(oldest)
}

function acknowledgeVisible() {
  const request = activeRequest
  if (
    !request?.visible ||
    !childReady ||
    visibleAcknowledgedRevision === request.revision ||
    !presentedHandler
  ) {
    return
  }
  visibleAcknowledgedRevision = request.revision
  void presentedHandler(request.revision, true)
}

function handleProcessLine(id: number, line: string) {
  if (line === 'TERRA_OVERLAY_PREWARMED') {
    if (id === warmProcess?.id) warmProcess.ready = true
    else {
      earlyProcessPrewarmed.add(id)
      boundEarlySet(earlyProcessPrewarmed)
    }
    return
  }
  if (line === 'TERRA_OVERLAY_READY') {
    confirmedHiddenProcesses.delete(id)
    if (id === childProcess?.id) confirmedHiddenRequests.delete(childProcess.requestId)
    if (id === childProcess?.id) {
      childReady = true
      acknowledgeVisible()
    } else {
      earlyProcessReady.add(id)
      boundEarlySet(earlyProcessReady)
    }
    return
  }
  if (line.startsWith('TERRA_OVERLAY_HIDDEN ')) {
    const revision = line.slice('TERRA_OVERLAY_HIDDEN '.length)
    const waiter = hiddenWaiters.get(id)
    if (id === childProcess?.id) confirmedHiddenRequests.add(childProcess.requestId)
    if (waiter?.revision === revision) {
      hiddenWaiters.delete(id)
      waiter.resolve()
    } else {
      confirmedHiddenProcesses.add(id)
    }
    return
  }
  if (line.startsWith('TERRA_OVERLAY_CLOSED ') && id === childProcess?.id) {
    const actionText = line.slice('TERRA_OVERLAY_CLOSED '.length)
    const action =
      actionText === 'quit' ? 'quit' : actionText === 'disconnect' ? 'disconnect' : 'resume'
    pendingCloseAction = action
    void requestOverlayClose(action)
  }
}

function watchSpawnedProcesses() {
  if (watchingSpawnedProcesses) return
  watchingSpawnedProcesses = true
  void events.on('spawnedProcess', (event: CustomEvent<unknown>) => {
    const detail = event.detail
    if (
      !detail ||
      typeof detail !== 'object' ||
      !('id' in detail) ||
      typeof detail.id !== 'number' ||
      !('action' in detail)
    ) {
      return
    }
    if (detail.action === 'stdOut' && 'data' in detail && typeof detail.data === 'string') {
      const buffered = (processOutput.get(detail.id) ?? '') + detail.data
      const lines = buffered.split('\n')
      processOutput.set(detail.id, lines.pop() ?? '')
      for (const line of lines) handleProcessLine(detail.id, line.trimEnd())
      return
    }
    if (detail.action !== 'exit') return
    processOutput.delete(detail.id)
    confirmedHiddenProcesses.add(detail.id)
    const hidden = hiddenWaiters.get(detail.id)
    if (hidden) {
      hiddenWaiters.delete(detail.id)
      hidden.resolve()
    }
    if (detail.id === warmProcess?.id) warmProcess = undefined
    if (detail.id === childProcess?.id) {
      confirmedHiddenRequests.add(childProcess.requestId)
      childProcess = undefined
      if (!hidden) {
        pendingCloseAction = 'resume'
        void requestOverlayClose('resume')
      }
      return
    }
    earlyProcessExits.add(detail.id)
    boundEarlySet(earlyProcessExits)
    earlyProcessReady.delete(detail.id)
    earlyProcessPrewarmed.delete(detail.id)
  })
}

export async function prewarmStreamOverlay() {
  if (warmProcess || prewarmStarting) return
  prewarmStarting = true
  watchSpawnedProcesses()
  try {
    const url = `${window.location.origin}/stream-overlay/prewarm`
    const child = await os.spawnProcess(waylandChildCommand(url, true))
    if (earlyProcessExits.delete(child.id)) return
    warmProcess = { id: child.id, ready: earlyProcessPrewarmed.delete(child.id) }
  } catch {
    // Unsupported display servers fall back to on-demand overlay startup.
  } finally {
    prewarmStarting = false
  }
}

export async function stopStreamOverlayPrewarm() {
  const process = warmProcess
  warmProcess = undefined
  if (process) await os.updateSpawnedProcess(process.id, 'exit').catch(() => undefined)
}

async function pollForClose() {
  if (pollRunning || !activeRequest) return
  pollRunning = true
  try {
    const requestId = activeRequest.requestId
    if (pendingCloseAction) {
      await requestOverlayClose(pendingCloseAction)
      return
    }
    try {
      const closed = overlayClosedSchema.safeParse(
        JSON.parse(await storage.getData(overlayClosedStorageKey)),
      )
      if (closed.success && closed.data.requestId === requestId) {
        pendingCloseAction = closed.data.action
        await requestOverlayClose(closed.data.action)
        return
      }
    } catch {
      // Close state is absent until child requests dismissal.
    }

    if (!childProcess) {
      try {
        const ready = overlayPresentationSchema.safeParse(
          JSON.parse(await storage.getData(overlayReadyStorageKey)),
        )
        childReady =
          ready.success &&
          ready.data.requestId === requestId &&
          ready.data.revision === activeRequest.revision
        acknowledgeVisible()
      } catch {
        // Child may still be initializing its webview.
      }
    }

    if (!childReady && readyDeadline !== 0 && Date.now() >= readyDeadline) {
      pendingCloseAction = 'resume'
      await requestOverlayClose('resume')
    }
  } finally {
    pollRunning = false
  }
}

async function applyStreamOverlayState(
  request: StreamOverlayRequest,
  onClosed: ClosedHandler,
  onPresented: PresentedHandler,
) {
  const sameSession =
    activeRequest?.hostId === request.hostId && activeRequest.generation === request.generation
  if (!request.visible) {
    if (!activeRequest || !sameSession) {
      await onPresented(request.revision, false)
      return
    }
    if (BigInt(request.revision) <= BigInt(activeRequest.revision)) return
    const activeId = activeRequest.requestId
    if (await finishOverlay(activeId, request.revision)) {
      await onPresented(request.revision, false)
    }
    return
  }

  if (activeRequest && !sameSession) {
    await finishOverlay(activeRequest.requestId, activeRequest.revision)
  }
  if (activeRequest) {
    if (BigInt(request.revision) <= BigInt(activeRequest.revision)) {
      acknowledgeVisible()
      return
    }
    activeRequest = { ...request, requestId: activeRequest.requestId }
    confirmedHiddenRequests.delete(activeRequest.requestId)
    closedHandler = onClosed
    presentedHandler = onPresented
    visibleAcknowledgedRevision = undefined
    pendingCloseAction = undefined
    await storeVisibility(activeRequest, true)
    if (activeRequest.wayland && childProcess) {
      childReady = false
      const encoded = encodeURIComponent(JSON.stringify(activeRequest)).replaceAll("'", '%27')
      await os.updateSpawnedProcess(
        childProcess.id,
        'stdIn',
        `SHOW ${activeRequest.revision} ${encoded}\n`,
      )
      readyDeadline = Date.now() + 3000
    }
    return
  }

  activeRequest = { ...request, requestId: requestId() }
  const openingRequest = activeRequest
  closedHandler = onClosed
  presentedHandler = onPresented
  visibleAcknowledgedRevision = undefined
  pendingCloseAction = undefined
  latestStatisticsSequence = -1

  try {
    await storage.removeData(overlayClosedStorageKey).catch(() => undefined)
    await storage.removeData(overlayReadyStorageKey).catch(() => undefined)
    await storage.removeData(overlayHiddenStorageKey).catch(() => undefined)
    await storage.removeData(overlayStatisticsStorageKey).catch(() => undefined)
    await storeVisibility(openingRequest, true)
    const route = `/stream-overlay/${encodeURIComponent(openingRequest.requestId)}`
    childReady = false
    readyDeadline = 0
    if (request.wayland) {
      watchSpawnedProcesses()
      const standaloneRequest = encodeURIComponent(JSON.stringify(openingRequest)).replaceAll(
        "'",
        '%27',
      )
      const url = `${window.location.origin}${route}#${standaloneRequest}`
      if (warmProcess?.ready) {
        childProcess = {
          id: warmProcess.id,
          requestId: openingRequest.requestId,
          persistent: true,
        }
        await os.updateSpawnedProcess(
          warmProcess.id,
          'stdIn',
          `SHOW ${openingRequest.revision} ${standaloneRequest}\n`,
        )
      } else {
        if (warmProcess) await stopStreamOverlayPrewarm()
        const child = await os.spawnProcess(
          waylandChildCommand(url, false, openingRequest.revision),
        )
        if (activeRequest?.requestId !== openingRequest.requestId || !activeRequest.visible) {
          earlyProcessExits.delete(child.id)
          await os.updateSpawnedProcess(child.id, 'exit').catch(() => undefined)
          return
        }
        childProcess = {
          id: child.id,
          requestId: openingRequest.requestId,
          persistent: false,
        }
        if (earlyProcessReady.delete(child.id)) childReady = true
        if (earlyProcessExits.delete(child.id)) {
          childProcess = undefined
          pendingCloseAction = 'resume'
          await requestOverlayClose('resume')
          return
        }
      }
    } else {
      await neutralinoWindow.create(route, {
        title: 'Terra Stream Overlay',
        x: request.bounds.x,
        y: request.bounds.y,
        width: request.bounds.width,
        height: request.bounds.height,
        center: false,
        alwaysOnTop: true,
        borderless: true,
        resizable: false,
        maximizable: false,
        enableInspector: false,
        exitProcessOnClose: false,
        injectGlobals: true,
        processArgs:
          '--enable-extensions=false --window-transparent=true --window-skip-taskbar=true',
      })
    }
    if (activeRequest?.requestId !== openingRequest.requestId || !activeRequest.visible) return
    readyDeadline = Date.now() + 3000
    pollTimer = setInterval(() => void pollForClose(), 100)
    acknowledgeVisible()
  } catch (error) {
    pendingCloseAction = 'resume'
    await requestOverlayClose('resume')
    throw error
  }
}

export function openStreamOverlay(
  request: StreamOverlayRequest,
  onClosed: ClosedHandler,
  onPresented: PresentedHandler = async () => undefined,
) {
  return enqueuePresentation(() => applyStreamOverlayState(request, onClosed, onPresented))
}

export async function updateStreamOverlayStatistics(update: StreamStatisticsUpdate) {
  const request = activeRequest
  if (
    !request?.visible ||
    request.hostId !== update.hostId ||
    request.generation !== update.generation ||
    update.sequence <= latestStatisticsSequence
  ) {
    return
  }
  latestStatisticsSequence = update.sequence
  const serialized = JSON.stringify(update)
  if (request.wayland) {
    const process = childProcess
    if (!process || process.requestId !== request.requestId) return
    const encoded = encodeURIComponent(serialized).replaceAll("'", '%27')
    await os.updateSpawnedProcess(process.id, 'stdIn', `STATS ${encoded}\n`).catch(() => undefined)
    return
  }
  await storage.setData(overlayStatisticsStorageKey, serialized).catch(() => undefined)
}

export function dismissStreamOverlay() {
  return enqueuePresentation(async () => {
    const request = activeRequest
    if (!request) return
    activeRequest = undefined
    if (pollTimer) clearInterval(pollTimer)
    pollTimer = undefined
    closedHandler = undefined
    presentedHandler = undefined
    pendingCloseAction = undefined
    latestStatisticsSequence = -1
    const process = childProcess?.requestId === request.requestId ? childProcess : undefined
    if (process) childProcess = undefined
    confirmedHiddenRequests.delete(request.requestId)
    await storeVisibility(request, false).catch(() => undefined)
    if (process) await hideProcess(process, request.revision).catch(() => undefined)
  })
}
