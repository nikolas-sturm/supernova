import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import {
  overlayClosedStorageKey,
  overlayReadyStorageKey,
  overlayRequestStorageKey,
  overlayStatisticsStorageKey,
  storedOverlayRequestSchema,
} from '../overlay/overlayProtocol'

const neutralino = vi.hoisted(() => {
  const data = new Map<string, string>()
  const eventHandlers = new Map<string, (event: CustomEvent<unknown>) => void>()
  return {
    data,
    eventHandlers,
    create: vi.fn((_url: string, _options?: unknown) => Promise.resolve()),
    eventsOn: vi.fn((name: string, handler: (event: CustomEvent<unknown>) => void) => {
      eventHandlers.set(name, handler)
      return Promise.resolve()
    }),
    spawnProcess: vi.fn((_command: string, _options?: unknown) =>
      Promise.resolve({ id: 1, pid: 1 }),
    ),
    updateSpawnedProcess: vi.fn(() => Promise.resolve()),
    getData: vi.fn((key: string) =>
      data.has(key)
        ? Promise.resolve(data.get(key) as string)
        : Promise.reject(new Error('missing')),
    ),
    removeData: vi.fn((key: string) => {
      data.delete(key)
      return Promise.resolve()
    }),
    setData: vi.fn((key: string, value: string) => {
      data.set(key, value)
      return Promise.resolve()
    }),
  }
})

vi.mock('@neutralinojs/lib', () => ({
  events: { on: neutralino.eventsOn },
  storage: {
    getData: neutralino.getData,
    removeData: neutralino.removeData,
    setData: neutralino.setData,
  },
  os: {
    spawnProcess: neutralino.spawnProcess,
    updateSpawnedProcess: neutralino.updateSpawnedProcess,
  },
  window: { create: neutralino.create },
}))

import {
  dismissStreamOverlay,
  openStreamOverlay,
  prewarmStreamOverlay,
  stopStreamOverlayPrewarm,
  updateStreamOverlayStatistics,
} from './overlayWindow'

function statistics(hostId = 'host-1', sequence = 1) {
  return {
    schemaVersion: 1 as const,
    hostId,
    generation: '4',
    sequence,
    elapsedMs: sequence * 1000,
    statistics: {
      totalFps: 60,
      receivedFps: 60,
      decodedFps: 60,
      presentedFps: 60,
      bitrateMbps: 10,
      frameLossPercent: 0,
      jitterLossPercent: 0,
      minimumHostLatencyMs: 1,
      maximumHostLatencyMs: 2,
      averageHostLatencyMs: 1.5,
      averageReassemblyMs: 0.5,
      averageDecodeMs: 1,
      averagePresentMs: 0.5,
      averageQueueDelayMs: 0.5,
      hasHostLatency: true,
      queueDrops: 0,
      rttMs: 10,
      rttVarianceMs: 2,
    },
  }
}

describe('overlayWindow', () => {
  afterEach(async () => {
    await dismissStreamOverlay()
    await stopStreamOverlayPrewarm()
    neutralino.data.clear()
    neutralino.create.mockClear()
    neutralino.spawnProcess.mockClear()
    neutralino.updateSpawnedProcess.mockClear()
  })

  beforeEach(() => {
    Object.defineProperties(window, {
      NL_ARGS: {
        configurable: true,
        value: ['/opt/terra', '--path=/opt/app resources', '--window-width=1280'],
      },
      NL_PATH: { configurable: true, value: '/opt/app' },
    })
  })

  it('opens fullscreen Wayland child through dedicated layer-shell host', async () => {
    await openStreamOverlay(
      {
        schemaVersion: 1,
        hostId: 'host-1',
        appId: 7,
        appName: 'Portal',
        generation: '4',
        bounds: { x: 0, y: 0, width: 1280, height: 720, scaleFactor: 1 },
        wayland: true,
        fullscreen: true,
      },
      vi.fn(() => Promise.resolve()),
    )

    expect(neutralino.create).not.toHaveBeenCalled()
    expect(neutralino.spawnProcess).toHaveBeenCalledWith(
      expect.stringContaining(
        "'/opt/app/extensions/terra-core/bin/terra-wayland-overlay' 'http://localhost:",
      ),
    )
    expect(neutralino.spawnProcess.mock.calls[0]?.[0]).toContain('/stream-overlay/')
    expect(neutralino.spawnProcess.mock.calls[0]?.[0]).toContain('#%7B')
  })

  it('opens a Wayland overlay outside fullscreen for testing', async () => {
    const onClosed = vi.fn(() => Promise.resolve())
    await openStreamOverlay(
      {
        schemaVersion: 1,
        hostId: 'host-1',
        appId: 7,
        appName: 'Portal',
        generation: '4',
        bounds: { x: 0, y: 0, width: 1280, height: 720, scaleFactor: 1 },
        wayland: true,
        fullscreen: false,
      },
      onClosed,
    )

    expect(onClosed).not.toHaveBeenCalled()
    expect(neutralino.create).not.toHaveBeenCalled()
    expect(neutralino.spawnProcess).toHaveBeenCalledOnce()
  })

  it('reuses a prewarmed Wayland helper after closing and reopening', async () => {
    await prewarmStreamOverlay()
    expect(neutralino.spawnProcess.mock.calls[0]?.[0]).toContain("'--prewarm'")
    expect(neutralino.spawnProcess.mock.calls[0]?.[0]).toContain('/stream-overlay/prewarm')
    neutralino.eventHandlers.get('spawnedProcess')?.(
      new CustomEvent('spawnedProcess', {
        detail: { id: 1, action: 'stdOut', data: 'TERRA_OVERLAY_PREWARMED\n' },
      }),
    )

    const onClosed = vi.fn(() => Promise.resolve())
    await openStreamOverlay(
      {
        schemaVersion: 1,
        hostId: 'host-1',
        appId: 7,
        appName: 'Portal',
        generation: '4',
        bounds: { x: 0, y: 0, width: 1280, height: 720, scaleFactor: 1 },
        wayland: true,
        fullscreen: false,
      },
      onClosed,
    )

    expect(neutralino.spawnProcess).toHaveBeenCalledOnce()
    expect(neutralino.updateSpawnedProcess).toHaveBeenCalledWith(
      1,
      'stdIn',
      expect.stringMatching(/^SHOW .+\n$/),
    )
    neutralino.eventHandlers.get('spawnedProcess')?.(
      new CustomEvent('spawnedProcess', {
        detail: { id: 1, action: 'stdOut', data: 'TERRA_OVERLAY_CLOSED\n' },
      }),
    )

    await vi.waitFor(() => expect(onClosed).toHaveBeenCalledOnce())
    await vi.waitFor(() =>
      expect(neutralino.updateSpawnedProcess).toHaveBeenCalledWith(1, 'stdIn', 'HIDE\n'),
    )
    await openStreamOverlay(
      {
        schemaVersion: 1,
        hostId: 'host-1',
        appId: 7,
        appName: 'Portal',
        generation: '4',
        bounds: { x: 0, y: 0, width: 1280, height: 720, scaleFactor: 1 },
        wayland: true,
        fullscreen: false,
      },
      vi.fn(() => Promise.resolve()),
    )

    expect(neutralino.spawnProcess).toHaveBeenCalledOnce()
    expect(neutralino.updateSpawnedProcess).toHaveBeenCalledWith(
      1,
      'stdIn',
      expect.stringMatching(/^SHOW .+\n$/),
    )
    expect(neutralino.updateSpawnedProcess).toHaveBeenCalledTimes(3)
    expect(neutralino.updateSpawnedProcess).toHaveBeenNthCalledWith(
      3,
      1,
      'stdIn',
      expect.stringMatching(/^SHOW .+\n$/),
    )
  })

  it('resumes stream when Wayland child exits unexpectedly', async () => {
    const onClosed = vi.fn(() => Promise.resolve())
    await openStreamOverlay(
      {
        schemaVersion: 1,
        hostId: 'host-1',
        appId: 7,
        appName: 'Portal',
        generation: '4',
        bounds: { x: 0, y: 0, width: 1280, height: 720, scaleFactor: 1 },
        wayland: true,
        fullscreen: true,
      },
      onClosed,
    )

    neutralino.eventHandlers.get('spawnedProcess')?.(
      new CustomEvent('spawnedProcess', {
        detail: { id: 1, action: 'exit', data: 1 },
      }),
    )

    await vi.waitFor(() => expect(onClosed).toHaveBeenCalledOnce())
  })

  it('keeps Wayland helper open after its ready signal', async () => {
    vi.useFakeTimers()
    const onClosed = vi.fn(() => Promise.resolve())
    try {
      await openStreamOverlay(
        {
          schemaVersion: 1,
          hostId: 'host-1',
          appId: 7,
          appName: 'Portal',
          generation: '4',
          bounds: { x: 0, y: 0, width: 1280, height: 720, scaleFactor: 1 },
          wayland: true,
          fullscreen: true,
        },
        onClosed,
      )
      neutralino.eventHandlers.get('spawnedProcess')?.(
        new CustomEvent('spawnedProcess', {
          detail: { id: 1, action: 'stdOut', data: 'TERRA_OVERLAY_READY\n' },
        }),
      )

      await vi.advanceTimersByTimeAsync(3100)
      expect(onClosed).not.toHaveBeenCalled()
    } finally {
      await dismissStreamOverlay()
      vi.useRealTimers()
    }
  })

  it('closes overlay if stream geometry changes', async () => {
    const onClosed = vi.fn(() => Promise.resolve())
    const active = {
      schemaVersion: 1 as const,
      hostId: 'host-1',
      appId: 7,
      appName: 'Portal',
      generation: '4',
      bounds: { x: 0, y: 0, width: 1280, height: 720, scaleFactor: 1 },
    }
    await openStreamOverlay(active, onClosed)

    await openStreamOverlay(
      { ...active, bounds: { ...active.bounds, width: 1200 } },
      vi.fn(() => Promise.resolve()),
    )

    expect(onClosed).toHaveBeenCalledOnce()
  })

  it('terminates a delayed Wayland child after its request is dismissed', async () => {
    let resolveSpawn: ((child: { id: number; pid: number }) => void) | undefined
    neutralino.spawnProcess.mockImplementationOnce(
      () =>
        new Promise((resolve) => {
          resolveSpawn = resolve
        }),
    )
    const firstOpen = openStreamOverlay(
      {
        schemaVersion: 1,
        hostId: 'host-1',
        appId: 7,
        appName: 'Portal',
        generation: '4',
        bounds: { x: 0, y: 0, width: 1280, height: 720, scaleFactor: 1 },
        wayland: true,
        fullscreen: true,
      },
      vi.fn(() => Promise.resolve()),
    )
    await vi.waitFor(() => expect(resolveSpawn).toBeTypeOf('function'))
    await dismissStreamOverlay()

    await openStreamOverlay(
      {
        schemaVersion: 1,
        hostId: 'host-2',
        appId: 8,
        appName: 'Control',
        generation: '5',
        bounds: { x: 0, y: 0, width: 960, height: 540, scaleFactor: 1 },
      },
      vi.fn(() => Promise.resolve()),
    )
    resolveSpawn?.({ id: 9, pid: 9 })
    await firstOpen

    expect(neutralino.updateSpawnedProcess).toHaveBeenCalledWith(9, 'exit')
    expect(neutralino.create).toHaveBeenCalledOnce()
  })

  it('creates one fixed child and resumes after matching close record', async () => {
    const onClosed = vi.fn(() => Promise.resolve())
    await openStreamOverlay(
      {
        schemaVersion: 1,
        hostId: 'host-1',
        appId: 7,
        appName: 'Portal',
        generation: '4',
        bounds: { x: -1200, y: 40, width: 1280, height: 720, scaleFactor: 1.25 },
      },
      onClosed,
    )

    expect(neutralino.create).toHaveBeenCalledWith(
      expect.stringMatching(/^\/stream-overlay\/.+/),
      expect.objectContaining({
        x: -1200,
        y: 40,
        width: 1280,
        height: 720,
        alwaysOnTop: true,
        borderless: true,
        resizable: false,
        processArgs:
          '--enable-extensions=false --window-transparent=true --window-skip-taskbar=true',
      }),
    )

    const stored = storedOverlayRequestSchema.parse(
      JSON.parse(neutralino.data.get(overlayRequestStorageKey) ?? ''),
    )
    neutralino.data.set(
      overlayReadyStorageKey,
      JSON.stringify({ schemaVersion: 1, requestId: stored.requestId }),
    )
    neutralino.data.set(
      overlayClosedStorageKey,
      JSON.stringify({ schemaVersion: 1, requestId: stored.requestId }),
    )

    await vi.waitFor(() => expect(onClosed).toHaveBeenCalledOnce())
    expect(
      storedOverlayRequestSchema.parse(
        JSON.parse(neutralino.data.get(overlayRequestStorageKey) ?? ''),
      ).visible,
    ).toBe(false)
  })

  it('stores matching statistics for Neutralino child and rejects another host', async () => {
    await openStreamOverlay(
      {
        schemaVersion: 1,
        hostId: 'host-1',
        appId: 7,
        appName: 'Portal',
        generation: '4',
        bounds: { x: 0, y: 0, width: 1280, height: 720, scaleFactor: 1 },
      },
      vi.fn(() => Promise.resolve()),
    )

    await updateStreamOverlayStatistics(statistics('other-host'))
    expect(neutralino.data.has(overlayStatisticsStorageKey)).toBe(false)
    await updateStreamOverlayStatistics(statistics())

    expect(JSON.parse(neutralino.data.get(overlayStatisticsStorageKey) ?? '')).toMatchObject({
      hostId: 'host-1',
      sequence: 1,
    })
  })

  it('retries a failed close dispatch without losing its action', async () => {
    const onClosed = vi
      .fn<(action: 'resume' | 'disconnect' | 'quit') => Promise<void>>()
      .mockRejectedValueOnce(new Error('extension unavailable'))
      .mockResolvedValue(undefined)
    await openStreamOverlay(
      {
        schemaVersion: 1,
        hostId: 'host-1',
        appId: 7,
        appName: 'Portal',
        generation: '4',
        bounds: { x: 0, y: 0, width: 1280, height: 720, scaleFactor: 1 },
      },
      onClosed,
    )
    const stored = storedOverlayRequestSchema.parse(
      JSON.parse(neutralino.data.get(overlayRequestStorageKey) ?? ''),
    )
    neutralino.data.set(
      overlayClosedStorageKey,
      JSON.stringify({ schemaVersion: 1, requestId: stored.requestId, action: 'disconnect' }),
    )

    await vi.waitFor(() => expect(onClosed).toHaveBeenCalledTimes(2), { timeout: 1500 })

    expect(onClosed).toHaveBeenNthCalledWith(1, 'disconnect')
    expect(onClosed).toHaveBeenNthCalledWith(2, 'disconnect')
  })

  it('relays matching statistics to Wayland helper stdin', async () => {
    await openStreamOverlay(
      {
        schemaVersion: 1,
        hostId: 'host-1',
        appId: 7,
        appName: 'Portal',
        generation: '4',
        bounds: { x: 0, y: 0, width: 1280, height: 720, scaleFactor: 1 },
        wayland: true,
      },
      vi.fn(() => Promise.resolve()),
    )
    neutralino.updateSpawnedProcess.mockClear()

    await updateStreamOverlayStatistics(statistics())

    expect(neutralino.updateSpawnedProcess).toHaveBeenCalledWith(
      1,
      'stdIn',
      expect.stringMatching(/^STATS .+\n$/),
    )
  })

  it('does not let a delayed child adopt a replacement request', async () => {
    let finishFirstCreate: (() => void) | undefined
    neutralino.create.mockImplementationOnce(
      () =>
        new Promise<void>((resolve) => {
          finishFirstCreate = resolve
        }),
    )
    const firstOpen = openStreamOverlay(
      {
        schemaVersion: 1,
        hostId: 'host-1',
        appId: 7,
        appName: 'Portal',
        generation: '4',
        bounds: { x: 0, y: 0, width: 1280, height: 720, scaleFactor: 1 },
      },
      vi.fn(() => Promise.resolve()),
    )
    await vi.waitFor(() => expect(finishFirstCreate).toBeTypeOf('function'))
    await dismissStreamOverlay()

    await openStreamOverlay(
      {
        schemaVersion: 1,
        hostId: 'host-2',
        appId: 8,
        appName: 'Control',
        generation: '5',
        bounds: { x: 10, y: 10, width: 960, height: 540, scaleFactor: 1 },
      },
      vi.fn(() => Promise.resolve()),
    )
    finishFirstCreate?.()
    await firstOpen

    const current = storedOverlayRequestSchema.parse(
      JSON.parse(neutralino.data.get(overlayRequestStorageKey) ?? ''),
    )
    expect(current).toMatchObject({ hostId: 'host-2', generation: '5', visible: true })
    expect(neutralino.create.mock.calls[0]?.[0]).not.toBe(neutralino.create.mock.calls[1]?.[0])
  })
})
