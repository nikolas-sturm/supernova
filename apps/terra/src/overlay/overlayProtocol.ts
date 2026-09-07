import { z } from 'zod'

export const overlayRequestStorageKey = 'terra_stream_overlay_request'
export const overlayClosedStorageKey = 'terra_stream_overlay_closed'
export const overlayReadyStorageKey = 'terra_stream_overlay_ready'
export const overlayStatisticsStorageKey = 'terra_stream_overlay_statistics'

export const overlayCloseActionSchema = z.enum(['resume', 'disconnect', 'quit'])

const nonnegativeMetric = z.number().finite().nonnegative()

export const streamStatisticsUpdateSchema = z.object({
  schemaVersion: z.literal(1),
  hostId: z.string().min(1),
  generation: z.string().regex(/^\d+$/),
  sequence: z.number().int().nonnegative(),
  elapsedMs: z.number().int().nonnegative(),
  statistics: z.object({
    totalFps: nonnegativeMetric,
    receivedFps: nonnegativeMetric,
    decodedFps: nonnegativeMetric,
    presentedFps: nonnegativeMetric,
    bitrateMbps: nonnegativeMetric,
    frameLossPercent: nonnegativeMetric,
    jitterLossPercent: nonnegativeMetric,
    minimumHostLatencyMs: nonnegativeMetric,
    maximumHostLatencyMs: nonnegativeMetric,
    averageHostLatencyMs: nonnegativeMetric,
    averageReassemblyMs: nonnegativeMetric,
    averageDecodeMs: nonnegativeMetric,
    averagePresentMs: nonnegativeMetric,
    averageQueueDelayMs: nonnegativeMetric,
    hasHostLatency: z.boolean(),
    queueDrops: z.number().int().nonnegative(),
    rttMs: z.number().int().nonnegative(),
    rttVarianceMs: z.number().int().nonnegative(),
  }),
})

const activeStreamSchema = z.object({
  width: z.number().int().positive(),
  height: z.number().int().positive(),
  fps: z.number().int().positive(),
  bitrateKbps: z.number().int().positive(),
  codec: z.string().min(1),
  displayMode: z.enum(['fullscreen', 'borderless', 'windowed']),
  displayIndex: z.number().int().nonnegative(),
  enableVsync: z.boolean(),
  audioConfig: z.enum(['stereo', '5.1', '7.1']),
  muteHostAudio: z.boolean(),
  gameOptimizations: z.boolean(),
  quitAppAfter: z.boolean(),
  enableHdr: z.boolean(),
  enableYuv444: z.boolean(),
  absoluteMouseMode: z.boolean(),
  captureSystemKeys: z.enum(['off', 'fullscreen', 'always']),
  touchscreenTrackpad: z.boolean(),
  swapMouseButtons: z.boolean(),
  reverseScrollDirection: z.boolean(),
  swapFaceButtons: z.boolean(),
  forceGamepad: z.boolean(),
  backgroundGamepad: z.boolean(),
  controllerMask: z.number().int().nonnegative(),
})

export const streamOverlayRequestSchema = z.object({
  schemaVersion: z.literal(1),
  hostId: z.string().min(1),
  appId: z.number().int().nonnegative(),
  appName: z.string(),
  generation: z.string().regex(/^\d+$/),
  bounds: z.object({
    x: z.number().int(),
    y: z.number().int(),
    width: z.number().int().positive(),
    height: z.number().int().positive(),
    scaleFactor: z.number().positive(),
  }),
  wayland: z.boolean().optional(),
  fullscreen: z.boolean().optional(),
  stream: activeStreamSchema.optional(),
})

export const storedOverlayRequestSchema = streamOverlayRequestSchema.extend({
  requestId: z.string().min(1),
  visible: z.boolean(),
})

export const overlayClosedSchema = z.object({
  schemaVersion: z.literal(1),
  requestId: z.string().min(1),
  action: overlayCloseActionSchema.optional().default('resume'),
})

export type StreamOverlayRequest = z.infer<typeof streamOverlayRequestSchema>
export type StoredOverlayRequest = z.infer<typeof storedOverlayRequestSchema>
export type OverlayCloseAction = z.infer<typeof overlayCloseActionSchema>
export type StreamStatisticsUpdate = z.infer<typeof streamStatisticsUpdateSchema>

export function isStreamOverlayShortcut(event: KeyboardEvent) {
  return event.key.toLowerCase() === 'o' && event.ctrlKey && event.altKey && event.shiftKey
}
