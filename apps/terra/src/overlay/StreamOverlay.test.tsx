import { fireEvent, render, screen, waitFor } from '@testing-library/react'
import { beforeEach, describe, expect, it, vi } from 'vitest'
import {
  overlayClosedStorageKey,
  overlayHiddenStorageKey,
  overlayReadyStorageKey,
  overlayRequestStorageKey,
  overlayStatisticsStorageKey,
  type StoredOverlayRequest,
} from './overlayProtocol'

const neutralino = vi.hoisted(() => {
  const data = new Map<string, string>()
  return {
    data,
    exit: vi.fn(() => Promise.resolve()),
    hide: vi.fn(() => Promise.resolve()),
    move: vi.fn((_x: number, _y: number) => Promise.resolve()),
    setSize: vi.fn((_options: { width: number; height: number }) => Promise.resolve()),
    getData: vi.fn((key: string) =>
      data.has(key)
        ? Promise.resolve(data.get(key) as string)
        : Promise.reject(new Error('missing')),
    ),
    setData: vi.fn((key: string, value: string) => {
      data.set(key, value)
      return Promise.resolve()
    }),
    on: vi.fn(() => Promise.resolve({ success: true, message: '' })),
    off: vi.fn(() => Promise.resolve({ success: true, message: '' })),
    init: vi.fn(),
  }
})

vi.mock('@neutralinojs/lib', () => ({
  app: { exit: neutralino.exit },
  events: { on: neutralino.on, off: neutralino.off },
  init: neutralino.init,
  storage: { getData: neutralino.getData, setData: neutralino.setData },
  window: { hide: neutralino.hide, move: neutralino.move, setSize: neutralino.setSize },
}))

import { StreamOverlay } from './StreamOverlay'

function request(visible = true): StoredOverlayRequest {
  return {
    schemaVersion: 1,
    requestId: 'overlay-1',
    visible,
    hostId: 'host-1',
    appId: 7,
    appName: 'Portal',
    generation: '4',
    revision: '1',
    bounds: { x: -1200, y: 40, width: 1280, height: 720, scaleFactor: 1.25 },
  }
}

function statistics(sequence = 1, hostId = 'host-1') {
  return {
    schemaVersion: 1,
    hostId,
    generation: '4',
    sequence,
    elapsedMs: sequence * 1000,
    statistics: {
      totalFps: 60,
      receivedFps: 60,
      decodedFps: 59.5,
      presentedFps: 59,
      bitrateMbps: 18.5,
      frameLossPercent: 0.2,
      jitterLossPercent: 0.1,
      minimumHostLatencyMs: 1,
      maximumHostLatencyMs: 3,
      averageHostLatencyMs: 2,
      averageReassemblyMs: 0.4,
      averageDecodeMs: 1.7,
      averagePresentMs: 0.5,
      averageQueueDelayMs: 0.8,
      hasHostLatency: true,
      queueDrops: 0,
      rttMs: 12,
      rttVarianceMs: 3,
    },
  }
}

describe('StreamOverlay', () => {
  beforeEach(() => {
    neutralino.data.clear()
    neutralino.exit.mockClear()
    neutralino.move.mockClear()
    neutralino.setSize.mockClear()
    neutralino.getData.mockClear()
    neutralino.setData.mockClear()
    neutralino.on.mockClear()
    neutralino.off.mockClear()
    window.history.replaceState({}, '', '/stream-overlay/overlay-1')
    Object.defineProperty(window, 'NL_OS', { configurable: true, value: 'Linux' })
    Object.defineProperty(window, 'TERRA_OVERLAY_NATIVE', {
      configurable: true,
      value: undefined,
    })
  })

  it('loads standalone Wayland request and closes through native bridge', async () => {
    const close = vi.fn()
    const waylandRequest = { ...request(), wayland: true, fullscreen: true }
    Object.defineProperty(window, 'TERRA_OVERLAY_NATIVE', {
      configurable: true,
      value: { close },
    })
    window.history.replaceState(
      {},
      '',
      `/stream-overlay/overlay-1#${encodeURIComponent(JSON.stringify(waylandRequest))}`,
    )
    render(<StreamOverlay />)

    expect(await screen.findByText('Portal')).toBeVisible()
    fireEvent.click(screen.getByRole('button', { name: 'Close overlay' }))
    await waitFor(() => expect(close).toHaveBeenCalledOnce())
    expect(neutralino.setData).not.toHaveBeenCalled()
  })

  it('accepts requests in a prewarmed native overlay', async () => {
    const close = vi.fn()
    Object.defineProperty(window, 'TERRA_OVERLAY_NATIVE', {
      configurable: true,
      value: { close },
    })
    window.history.replaceState({}, '', '/stream-overlay/prewarm')
    render(<StreamOverlay />)

    window.dispatchEvent(
      new CustomEvent('terra-overlay-request', {
        detail: { ...request(), wayland: true, fullscreen: false },
      }),
    )

    expect(await screen.findByText('Portal')).toBeVisible()
    fireEvent.click(screen.getByRole('button', { name: 'Close overlay' }))
    await waitFor(() => expect(close).toHaveBeenCalledOnce())
  })

  it('loads shared request and closes through X', async () => {
    neutralino.data.set(overlayRequestStorageKey, JSON.stringify(request()))
    render(<StreamOverlay />)

    expect(await screen.findByText('Portal')).toBeVisible()
    expect(JSON.parse(neutralino.data.get(overlayReadyStorageKey) ?? '')).toEqual({
      schemaVersion: 1,
      requestId: 'overlay-1',
      revision: '1',
    })
    fireEvent.click(screen.getByRole('button', { name: 'Close overlay' }))

    expect(neutralino.exit).not.toHaveBeenCalled()
    expect(JSON.parse(neutralino.data.get(overlayClosedStorageKey) ?? '')).toEqual({
      schemaVersion: 1,
      requestId: 'overlay-1',
      action: 'resume',
    })
    neutralino.data.set(
      overlayRequestStorageKey,
      JSON.stringify({ ...request(false), revision: '2' }),
    )
    await waitFor(() => expect(neutralino.exit).toHaveBeenCalledOnce())
    expect(JSON.parse(neutralino.data.get(overlayHiddenStorageKey) ?? '')).toMatchObject({
      requestId: 'overlay-1',
      revision: '2',
    })
  })

  it('applies revisioned stream-window bounds to shared child', async () => {
    neutralino.data.set(overlayRequestStorageKey, JSON.stringify(request()))
    render(<StreamOverlay />)
    await screen.findByText('Portal')

    neutralino.data.set(
      overlayRequestStorageKey,
      JSON.stringify({
        ...request(),
        revision: '2',
        bounds: { x: 40, y: 60, width: 960, height: 540, scaleFactor: 1 },
      }),
    )

    await waitFor(() => expect(neutralino.move).toHaveBeenCalledWith(40, 60))
    expect(neutralino.setSize).toHaveBeenCalledWith({ width: 960, height: 540 })
  })

  it('switches tabs and exposes unavailable USB forwarding honestly', async () => {
    neutralino.data.set(overlayRequestStorageKey, JSON.stringify(request()))
    render(<StreamOverlay />)
    await screen.findByRole('heading', { name: 'Portal' })

    fireEvent.click(screen.getByRole('tab', { name: 'Controllers & USB' }))

    expect(screen.getByRole('heading', { name: 'Controllers & USB' })).toBeVisible()
    expect(screen.getByText('USB forwarding')).toBeVisible()
    expect(screen.getAllByText('Unavailable').length).toBeGreaterThan(0)
  })

  it('toggles narrow statistics drawer state', async () => {
    neutralino.data.set(overlayRequestStorageKey, JSON.stringify(request()))
    render(<StreamOverlay />)
    await screen.findByRole('heading', { name: 'Portal' })
    const toggle = screen.getByText('Stats').closest('button')
    expect(toggle).not.toBeNull()

    fireEvent.click(toggle as HTMLButtonElement)

    expect(toggle).toHaveAttribute('aria-expanded', 'true')
  })

  it('renders matching statistics and ignores stale host data', async () => {
    neutralino.data.set(overlayRequestStorageKey, JSON.stringify(request()))
    neutralino.data.set(overlayStatisticsStorageKey, JSON.stringify(statistics(1, 'other-host')))
    render(<StreamOverlay />)
    await screen.findByRole('heading', { name: 'Portal' })
    expect(screen.getByText('WAITING')).toBeVisible()

    neutralino.data.set(overlayStatisticsStorageKey, JSON.stringify(statistics()))

    expect(await screen.findByText('59.0 fps')).toBeVisible()
    expect(screen.getByText('18.5 Mbps')).toBeVisible()
    expect(screen.getByText('LIVE')).toBeVisible()
  })

  it('writes disconnect intent before closing child', async () => {
    neutralino.data.set(overlayRequestStorageKey, JSON.stringify(request()))
    render(<StreamOverlay />)
    await screen.findByRole('heading', { name: 'Portal' })

    fireEvent.click(screen.getByRole('button', { name: /Disconnect/ }))

    expect(neutralino.exit).not.toHaveBeenCalled()
    expect(JSON.parse(neutralino.data.get(overlayClosedStorageKey) ?? '')).toMatchObject({
      requestId: 'overlay-1',
      action: 'disconnect',
    })
  })

  it('consumes exact shortcut and notifies parent on O press', async () => {
    neutralino.data.set(overlayRequestStorageKey, JSON.stringify(request()))
    render(<StreamOverlay />)
    await screen.findByText('Portal')

    const dispatched = fireEvent.keyDown(window, {
      key: 'O',
      ctrlKey: true,
      altKey: true,
      shiftKey: true,
    })

    expect(dispatched).toBe(false)
    expect(neutralino.exit).not.toHaveBeenCalled()
    await waitFor(() => expect(neutralino.data.has(overlayClosedStorageKey)).toBe(true))
    fireEvent.keyUp(window, {
      key: 'O',
      ctrlKey: true,
      altKey: true,
      shiftKey: true,
    })
    expect(neutralino.exit).not.toHaveBeenCalled()
  })

  it('exits without resuming stream when parent hides overlay', async () => {
    neutralino.data.set(overlayRequestStorageKey, JSON.stringify(request(false)))
    render(<StreamOverlay />)

    await waitFor(() => expect(neutralino.exit).toHaveBeenCalledOnce())
    expect(neutralino.data.has(overlayClosedStorageKey)).toBe(false)
  })

  it('stays open when close handshake fails and allows retry', async () => {
    neutralino.data.set(overlayRequestStorageKey, JSON.stringify(request()))
    render(<StreamOverlay />)
    await screen.findByText('Portal')
    neutralino.setData.mockRejectedValueOnce(new Error('storage unavailable'))

    fireEvent.click(screen.getByRole('button', { name: 'Close overlay' }))
    expect(await screen.findByText('CLOSE FAILED / RETRY')).toBeVisible()
    expect(neutralino.exit).not.toHaveBeenCalled()

    fireEvent.click(screen.getByRole('button', { name: 'Close overlay' }))
    await waitFor(() => expect(neutralino.data.has(overlayClosedStorageKey)).toBe(true))
    expect(screen.queryByText('CLOSE FAILED / RETRY')).not.toBeInTheDocument()
  })
})
