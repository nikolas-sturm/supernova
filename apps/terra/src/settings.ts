import { z } from 'zod'

export const appModeSchema = z.enum(['gaming', 'workstation'])

export type AppMode = z.infer<typeof appModeSchema>

export const resolutionOptions = [
  { label: '720p', width: 1280, height: 720 },
  { label: '1080p', width: 1920, height: 1080 },
  { label: '1440p', width: 2560, height: 1440 },
  { label: '4K', width: 3840, height: 2160 },
] as const

export const fpsOptions = [30, 60, 90, 120] as const

export const settingsSchema = z.object({
  width: z.number().int().min(640).max(7680),
  height: z.number().int().min(360).max(4320),
  fps: z.number().int().min(1).max(240),
  bitrateKbps: z.number().int().min(500).max(500_000),
  displayMode: z.enum(['fullscreen', 'borderless', 'windowed']),
  displayIndex: z.number().int().min(0).max(15),
  enableVsync: z.boolean(),
  framePacing: z.boolean(),
  audioConfig: z.enum(['stereo', '5.1', '7.1']),
  muteHostAudio: z.boolean(),
  muteOnFocusLoss: z.boolean(),
  gameOptimizations: z.boolean(),
  quitAppAfter: z.boolean(),
  language: z.enum(['automatic', 'english']),
  uiDisplayMode: z.enum(['windowed', 'maximized', 'fullscreen']),
  connectionWarnings: z.boolean(),
  configurationWarnings: z.boolean(),
  richPresence: z.boolean(),
  keepAwake: z.boolean(),
  absoluteMouseMode: z.boolean(),
  captureSystemKeys: z.enum(['off', 'fullscreen', 'always']),
  touchscreenTrackpad: z.boolean(),
  swapMouseButtons: z.boolean(),
  reverseScrollDirection: z.boolean(),
  swapFaceButtons: z.boolean(),
  forceGamepad: z.boolean(),
  gamepadMouse: z.boolean(),
  backgroundGamepad: z.boolean(),
  videoDecoder: z.enum(['automatic', 'hardware', 'software']),
  videoCodec: z.enum(['automatic', 'h264', 'hevc', 'av1']),
  enableHdr: z.boolean(),
  enableYuv444: z.boolean(),
  unlockBitrate: z.boolean(),
  autoDiscoverHosts: z.boolean(),
  detectBlockedConnections: z.boolean(),
  showPerformanceStats: z.boolean(),
})

export type StreamSettings = z.infer<typeof settingsSchema>

export const defaultSettings: StreamSettings = {
  width: 1920,
  height: 1080,
  fps: 60,
  bitrateKbps: 10_000,
  displayMode: 'windowed',
  displayIndex: 0,
  enableVsync: true,
  framePacing: false,
  audioConfig: 'stereo',
  muteHostAudio: true,
  muteOnFocusLoss: false,
  gameOptimizations: true,
  quitAppAfter: false,
  language: 'automatic',
  uiDisplayMode: 'windowed',
  connectionWarnings: true,
  configurationWarnings: true,
  richPresence: false,
  keepAwake: true,
  absoluteMouseMode: false,
  captureSystemKeys: 'off',
  touchscreenTrackpad: true,
  swapMouseButtons: false,
  reverseScrollDirection: false,
  swapFaceButtons: false,
  forceGamepad: false,
  gamepadMouse: true,
  backgroundGamepad: false,
  videoDecoder: 'automatic',
  videoCodec: 'automatic',
  enableHdr: false,
  enableYuv444: false,
  unlockBitrate: false,
  autoDiscoverHosts: false,
  detectBlockedConnections: false,
  showPerformanceStats: false,
}

export const defaultSettingsByMode: Record<AppMode, StreamSettings> = {
  gaming: { ...defaultSettings },
  workstation: {
    ...defaultSettings,
    gameOptimizations: false,
    absoluteMouseMode: true,
  },
}

export function recommendedBitrate(width: number, height: number, fps: number) {
  const frameRateFactor = (fps <= 60 ? fps : Math.sqrt(fps / 60) * 60) / 30
  const table = [
    [640 * 360, 1],
    [854 * 480, 2],
    [1280 * 720, 5],
    [1920 * 1080, 10],
    [2560 * 1440, 20],
    [3840 * 2160, 40],
  ] as const
  const pixels = width * height
  let resolutionFactor = table.at(-1)?.[1] ?? 40
  for (let index = 0; index < table.length; index += 1) {
    const current = table[index]
    if (!current || pixels > current[0]) continue
    if (index === 0 || pixels === current[0]) {
      resolutionFactor = current[1]
    } else {
      const previous = table[index - 1]
      if (!previous) break
      const position = (pixels - previous[0]) / (current[0] - previous[0])
      resolutionFactor = previous[1] + position * (current[1] - previous[1])
    }
    break
  }
  return Math.round(resolutionFactor * frameRateFactor) * 1000
}
