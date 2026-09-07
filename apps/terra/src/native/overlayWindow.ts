import { events, window as neutralinoWindow, os, storage } from '@neutralinojs/lib'
import {
  type OverlayCloseAction,
  overlayClosedSchema,
  overlayClosedStorageKey,
  overlayReadyStorageKey,
  overlayRequestStorageKey,
  overlayStatisticsStorageKey,
  type StoredOverlayRequest,
  type StreamOverlayRequest,
  type StreamStatisticsUpdate,
} from '../overlay/overlayProtocol'

type ClosedHandler = (action: OverlayCloseAction) => Promise<void>

let activeRequest: StoredOverlayRequest | undefined
let closedHandler: ClosedHandler | undefined
let pollTimer: ReturnType<typeof setInterval> | undefined
let pollRunning = false
let childReady = false
let readyDeadline = 0
let latestStatisticsSequence = -1
let finishRunning = false
let finishRetryAt = 0
let pendingCloseAction: OverlayCloseAction | undefined
let childProcess: { id: number; requestId: string; persistent: boolean } | undefined
let warmProcess: { id: number; ready: boolean } | undefined
let prewarmStarting = false
let watchingSpawnedProcesses = false
const earlyProcessExits = new Set<number>()
const earlyProcessReady = new Set<number>()
const earlyProcessPrewarmed = new Set<number>()

function requestId() {
  return typeof crypto.randomUUID === 'function'
    ? crypto.randomUUID()
    : `${Date.now()}-${Math.random().toString(16).slice(2)}`
}

function shellQuote(value: string) {
  return `'${value.replaceAll("'", `'"'"'`)}'`
}

function waylandChildCommand(url: string, prewarm = false) {
  const executable = `${window.NL_PATH}/extensions/terra-core/bin/terra-wayland-overlay`
  return [executable, ...(prewarm ? ['--prewarm'] : []), url].map(shellQuote).join(' ')
}

async function storeVisibility(request: StoredOverlayRequest, visible: boolean) {
  await storage.setData(
    overlayRequestStorageKey,
    JSON.stringify({ ...request, visible } satisfies StoredOverlayRequest),
  )
}

async function finishOverlay(
  expectedRequestId: string,
  resumeStream: boolean,
  action: OverlayCloseAction = 'resume',
) {
  if (
    finishRunning ||
    Date.now() < finishRetryAt ||
    !activeRequest ||
    activeRequest.requestId !== expectedRequestId
  ) {
    return false
  }
  finishRunning = true
  const request = activeRequest
  const handler = closedHandler
  try {
    if (resumeStream && handler) {
      try {
        await handler(action)
      } catch {
        finishRetryAt = Date.now() + 500
        return false
      }
    }
    activeRequest = { ...request, visible: false }
    if (pollTimer) clearInterval(pollTimer)
    pollTimer = undefined
    closedHandler = undefined
    childReady = false
    readyDeadline = 0
    latestStatisticsSequence = -1
    finishRetryAt = 0
    pendingCloseAction = undefined
    const process = childProcess?.requestId === expectedRequestId ? childProcess : undefined
    if (process) childProcess = undefined
    try {
      await storeVisibility(request, false)
    } catch {
      // Stream focus restoration must not depend on storage cleanup succeeding.
    } finally {
      if (activeRequest?.requestId === expectedRequestId) activeRequest = undefined
    }
    if (process) {
      await os
        .updateSpawnedProcess(process.id, process.persistent ? 'stdIn' : 'exit', 'HIDE\n')
        .catch(() => undefined)
    }
    return true
  } finally {
    finishRunning = false
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
    if (
      detail.action === 'stdOut' &&
      'data' in detail &&
      typeof detail.data === 'string' &&
      detail.data.includes('TERRA_OVERLAY_PREWARMED')
    ) {
      if (detail.id === warmProcess?.id) {
        warmProcess.ready = true
      } else {
        earlyProcessPrewarmed.add(detail.id)
      }
      return
    }
    if (
      detail.action === 'stdOut' &&
      'data' in detail &&
      typeof detail.data === 'string' &&
      detail.data.includes('TERRA_OVERLAY_READY')
    ) {
      if (detail.id === childProcess?.id) {
        childReady = true
      } else {
        earlyProcessReady.add(detail.id)
        if (earlyProcessReady.size > 32) {
          const oldest = earlyProcessReady.values().next().value
          if (oldest !== undefined) earlyProcessReady.delete(oldest)
        }
      }
      return
    }
    if (
      detail.action === 'stdOut' &&
      'data' in detail &&
      typeof detail.data === 'string' &&
      detail.data.includes('TERRA_OVERLAY_CLOSED') &&
      detail.id === childProcess?.id
    ) {
      const action = detail.data.includes(' quit')
        ? 'quit'
        : detail.data.includes(' disconnect')
          ? 'disconnect'
          : 'resume'
      pendingCloseAction = action
      void finishOverlay(childProcess.requestId, true, action)
      return
    }
    if (detail.action !== 'exit') return
    if (detail.id === warmProcess?.id) warmProcess = undefined
    if (detail.id === childProcess?.id) {
      pendingCloseAction = 'resume'
      void finishOverlay(childProcess.requestId, true)
      return
    }
    earlyProcessExits.add(detail.id)
    if (earlyProcessExits.size > 32) {
      const oldest = earlyProcessExits.values().next().value
      if (oldest !== undefined) earlyProcessExits.delete(oldest)
    }
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
      await finishOverlay(requestId, true, pendingCloseAction)
      return
    }
    try {
      const closed = overlayClosedSchema.safeParse(
        JSON.parse(await storage.getData(overlayClosedStorageKey)),
      )
      if (closed.success && closed.data.requestId === requestId) {
        await finishOverlay(requestId, true, closed.data.action)
        return
      }
    } catch {
      // Close state is absent until child requests dismissal.
    }

    if (!childProcess) {
      try {
        const ready = overlayClosedSchema.safeParse(
          JSON.parse(await storage.getData(overlayReadyStorageKey)),
        )
        childReady = ready.success && ready.data.requestId === requestId
      } catch {
        // Child may still be initializing its webview.
      }
    }

    if (!childReady && readyDeadline !== 0 && Date.now() >= readyDeadline) {
      await finishOverlay(requestId, true)
    }
  } finally {
    pollRunning = false
  }
}

export async function openStreamOverlay(request: StreamOverlayRequest, onClosed: ClosedHandler) {
  if (activeRequest) {
    const boundsChanged =
      activeRequest.bounds.x !== request.bounds.x ||
      activeRequest.bounds.y !== request.bounds.y ||
      activeRequest.bounds.width !== request.bounds.width ||
      activeRequest.bounds.height !== request.bounds.height ||
      activeRequest.bounds.scaleFactor !== request.bounds.scaleFactor
    const sameSession =
      activeRequest.hostId === request.hostId && activeRequest.generation === request.generation
    if (sameSession) {
      if (boundsChanged) await finishOverlay(activeRequest.requestId, true)
      return
    }
    await finishOverlay(activeRequest.requestId, false)
    if (activeRequest) return
  }

  activeRequest = {
    ...request,
    requestId: requestId(),
    visible: true,
  }
  const openingRequest = activeRequest
  closedHandler = onClosed
  pendingCloseAction = undefined

  try {
    await storage.removeData(overlayClosedStorageKey).catch(() => undefined)
    await storage.removeData(overlayReadyStorageKey).catch(() => undefined)
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
      if (activeRequest?.requestId !== openingRequest.requestId || !activeRequest.visible) return
      if (warmProcess?.ready) {
        childProcess = {
          id: warmProcess.id,
          requestId: openingRequest.requestId,
          persistent: true,
        }
        await os.updateSpawnedProcess(warmProcess.id, 'stdIn', `SHOW ${standaloneRequest}\n`)
      } else {
        if (warmProcess) await stopStreamOverlayPrewarm()
        const child = await os.spawnProcess(waylandChildCommand(url))
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
          await finishOverlay(openingRequest.requestId, true)
          return
        }
      }
      if (activeRequest?.requestId !== openingRequest.requestId || !activeRequest.visible) {
        await finishOverlay(openingRequest.requestId, false)
        return
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
  } catch (error) {
    const active = await finishOverlay(openingRequest.requestId, true)
    if (!active) return
    throw error
  }
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

export async function dismissStreamOverlay() {
  if (activeRequest) await finishOverlay(activeRequest.requestId, false)
}
