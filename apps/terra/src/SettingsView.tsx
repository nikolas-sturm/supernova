import { RotateCcw, SlidersHorizontal } from 'lucide-react'
import { useState } from 'react'
import styles from './App.module.css'
import { fpsOptions, recommendedBitrate, resolutionOptions } from './settings'
import { useClientStore } from './store/clientStore'

interface ToggleProps {
  checked: boolean
  label: string
  note?: string
  disabled?: boolean
  onChange: (checked: boolean) => void
}

function Toggle({ checked, label, note, disabled = false, onChange }: ToggleProps) {
  return (
    <label className={`${styles.settingToggle} ${disabled ? styles.settingDisabled : ''}`}>
      <input
        type="checkbox"
        checked={checked}
        disabled={disabled}
        onChange={(event) => onChange(event.currentTarget.checked)}
      />
      <span className={styles.toggleControl} aria-hidden="true" />
      <span>
        <strong>{label}</strong>
        {note && <small>{note}</small>}
      </span>
    </label>
  )
}

interface SelectOption {
  label: string
  value: string | number
  disabled?: boolean
}

interface SelectProps {
  label: string
  value: string | number
  options: SelectOption[]
  disabled?: boolean
  note?: string
  onChange: (value: string) => void
}

function SelectSetting({ label, value, options, disabled = false, note, onChange }: SelectProps) {
  return (
    <label className={`${styles.selectSetting} ${disabled ? styles.settingDisabled : ''}`}>
      <span>{label}</span>
      <select
        aria-label={label}
        value={value}
        disabled={disabled}
        onChange={(event) => onChange(event.currentTarget.value)}
      >
        {options.map((option) => (
          <option key={option.value} value={option.value} disabled={option.disabled}>
            {option.label}
          </option>
        ))}
      </select>
      {note && <small>{note}</small>}
    </label>
  )
}

function NumberSetting({
  label,
  value,
  min,
  max,
  onCommit,
}: {
  label: string
  value: number
  min: number
  max: number
  onCommit: (value: number) => void
}) {
  function commit(input: HTMLInputElement) {
    const next = Number(input.value)
    if (!Number.isInteger(next) || next < min || next > max) {
      input.value = String(value)
      return
    }
    onCommit(next)
  }

  return (
    <label className={styles.numberSetting}>
      <span>{label}</span>
      <input
        key={value}
        type="number"
        aria-label={label}
        defaultValue={value}
        min={min}
        max={max}
        step="1"
        onBlur={(event) => commit(event.currentTarget)}
        onKeyDown={(event) => {
          if (event.key === 'Enter') event.currentTarget.blur()
        }}
      />
    </label>
  )
}

function SettingGroup({
  title,
  eyebrow,
  children,
}: {
  title: string
  eyebrow: string
  children: React.ReactNode
}) {
  return (
    <section className={styles.settingsGroup}>
      <div className={styles.settingsGroupHeading}>
        <div>
          <p>{eyebrow}</p>
          <h2>{title}</h2>
        </div>
      </div>
      <div className={styles.settingsCard}>{children}</div>
    </section>
  )
}

export function SettingsView({ section = 'profile' }: { section?: 'profile' | 'gamepad' }) {
  const appMode = useClientStore((state) => state.appMode)
  const settings = useClientStore((state) => state.settingsByMode[state.appMode])
  const update = useClientStore((state) => state.updateSettings)
  const reset = useClientStore((state) => state.resetSettings)
  const presetResolution = resolutionOptions.find(
    ({ width, height }) => width === settings.width && height === settings.height,
  )
  const presetFps = fpsOptions.includes(settings.fps as (typeof fpsOptions)[number])
  const [customResolution, setCustomResolution] = useState(!presetResolution)
  const [customFps, setCustomFps] = useState(!presetFps)
  const resolutionValue = customResolution ? 'custom' : `${settings.width}x${settings.height}`
  const fpsValue = customFps ? 'custom' : settings.fps
  const bitrateMaximum = settings.unlockBitrate ? 500_000 : 150_000
  const recommended = Math.min(
    bitrateMaximum,
    Math.max(500, recommendedBitrate(settings.width, settings.height, settings.fps)),
  )

  function recommendedModeBitrate(width: number, height: number, fps: number) {
    return Math.min(bitrateMaximum, Math.max(500, recommendedBitrate(width, height, fps)))
  }

  function selectResolution(value: string) {
    if (value === 'custom') {
      setCustomResolution(true)
      return
    }
    const selected = resolutionOptions.find(({ width, height }) => `${width}x${height}` === value)
    if (!selected) return
    setCustomResolution(false)
    update({
      width: selected.width,
      height: selected.height,
      bitrateKbps: recommendedModeBitrate(selected.width, selected.height, settings.fps),
    })
  }

  function selectFps(value: string) {
    if (value === 'custom') {
      setCustomFps(true)
      return
    }
    setCustomFps(false)
    const fps = Number(value)
    update({ fps, bitrateKbps: recommendedModeBitrate(settings.width, settings.height, fps) })
  }

  function updateCustomMode(values: { width?: number; height?: number; fps?: number }) {
    const width = values.width ?? settings.width
    const height = values.height ?? settings.height
    const fps = values.fps ?? settings.fps
    update({ ...values, bitrateKbps: recommendedModeBitrate(width, height, fps) })
  }

  function resetProfile() {
    setCustomResolution(false)
    setCustomFps(false)
    reset()
  }

  if (section === 'gamepad') {
    return (
      <div className={styles.settingsPage}>
        <section className={styles.settingsIntro}>
          <div className={styles.settingsIntroIcon}>
            <SlidersHorizontal size={24} />
          </div>
          <div>
            <p className={styles.panelLabel}>GAMING PROFILE / INPUT BRIDGE</p>
            <h2>Controller behavior, isolated from workstation mode.</h2>
            <p>Gamepad changes persist in Gaming and apply when the next stream starts.</p>
          </div>
          <button type="button" onClick={resetProfile}>
            <RotateCcw size={14} /> Reset Gaming defaults
          </button>
        </section>

        <div className={styles.settingsSingleColumn}>
          <SettingGroup title="Gamepad & haptics" eyebrow="CONTROLLER SETTINGS">
            <div className={styles.toggleStack}>
              <Toggle
                label="Swap A/B and X/Y gamepad buttons"
                checked={settings.swapFaceButtons}
                onChange={(swapFaceButtons) => update({ swapFaceButtons })}
              />
              <Toggle
                label="Force gamepad #1 always connected"
                checked={settings.forceGamepad}
                onChange={(forceGamepad) => update({ forceGamepad })}
              />
              <Toggle
                label="Enable mouse control by holding Start"
                note="Planned: controller mouse emulation"
                checked={settings.gamepadMouse}
                disabled
                onChange={(gamepadMouse) => update({ gamepadMouse })}
              />
              <Toggle
                label="Process gamepad input while Terra is in background"
                checked={settings.backgroundGamepad}
                onChange={(backgroundGamepad) => update({ backgroundGamepad })}
              />
            </div>
          </SettingGroup>
        </div>
      </div>
    )
  }

  return (
    <div className={styles.settingsPage}>
      <section className={styles.settingsIntro}>
        <div className={styles.settingsIntroIcon}>
          <SlidersHorizontal size={24} />
        </div>
        <div>
          <p className={styles.panelLabel}>STREAM PROFILE / LOCAL CLIENT</p>
          <h2>
            {appMode === 'gaming'
              ? 'Gaming profile, applied at next launch.'
              : 'Workstation profile, tuned for precise desktop control.'}
          </h2>
          <p>
            This profile persists independently. Switching modes never overwrites its stream or
            input configuration.
          </p>
        </div>
        <button type="button" onClick={resetProfile}>
          <RotateCcw size={14} /> Reset {appMode === 'gaming' ? 'Gaming' : 'Workstation'} defaults
        </button>
      </section>

      <div className={styles.settingsColumns}>
        <div>
          <SettingGroup title="Stream" eyebrow="BASIC SETTINGS">
            <div className={styles.settingPair}>
              <SelectSetting
                label="Resolution"
                value={resolutionValue}
                options={[
                  ...resolutionOptions.map((option) => ({
                    label: option.label,
                    value: `${option.width}x${option.height}`,
                  })),
                  { label: 'Custom', value: 'custom' },
                ]}
                onChange={selectResolution}
              />
              <SelectSetting
                label="Frame rate"
                value={fpsValue}
                options={[
                  ...fpsOptions.map((fps) => ({ label: `${fps} FPS`, value: fps })),
                  { label: 'Custom', value: 'custom' },
                ]}
                onChange={selectFps}
              />
            </div>
            {customResolution && (
              <div className={styles.settingPair}>
                <NumberSetting
                  label="Custom width"
                  value={settings.width}
                  min={640}
                  max={7680}
                  onCommit={(width) => updateCustomMode({ width })}
                />
                <NumberSetting
                  label="Custom height"
                  value={settings.height}
                  min={360}
                  max={4320}
                  onCommit={(height) => updateCustomMode({ height })}
                />
              </div>
            )}
            {customFps && (
              <NumberSetting
                label="Custom frame rate"
                value={settings.fps}
                min={1}
                max={240}
                onCommit={(fps) => updateCustomMode({ fps })}
              />
            )}
            <label className={styles.bitrateSetting}>
              <span>
                <strong>Video bitrate</strong>
                <output>{(settings.bitrateKbps / 1000).toFixed(1)} Mbps</output>
              </span>
              <input
                type="range"
                min="500"
                max={bitrateMaximum}
                step="500"
                value={settings.bitrateKbps}
                onChange={(event) => update({ bitrateKbps: Number(event.currentTarget.value) })}
              />
              <button type="button" onClick={() => update({ bitrateKbps: recommended })}>
                Use recommended ({recommended / 1000} Mbps)
              </button>
            </label>
            <SelectSetting
              label="Display mode"
              value={settings.displayMode}
              options={[
                { label: 'Fullscreen', value: 'fullscreen' },
                { label: 'Borderless windowed', value: 'borderless' },
                { label: 'Windowed', value: 'windowed' },
              ]}
              onChange={(displayMode) =>
                update({ displayMode: displayMode as typeof settings.displayMode })
              }
            />
            <SelectSetting
              label="Target display"
              value={settings.displayIndex}
              options={[
                { label: 'Primary display', value: 0 },
                { label: 'Display 2', value: 1 },
                { label: 'Display 3', value: 2 },
                { label: 'Display 4', value: 3 },
              ]}
              note="Unavailable displays fall back to primary."
              onChange={(displayIndex) => update({ displayIndex: Number(displayIndex) })}
            />
            <div className={styles.toggleStack}>
              <Toggle
                label="V-Sync"
                checked={settings.enableVsync}
                onChange={(enableVsync) => update({ enableVsync })}
              />
              <Toggle
                label="Frame pacing"
                note="Planned: native presentation scheduler"
                checked={settings.framePacing}
                disabled
                onChange={(framePacing) => update({ framePacing })}
              />
            </div>
          </SettingGroup>

          <SettingGroup title="Audio" eyebrow="AUDIO SETTINGS">
            <SelectSetting
              label="Audio configuration"
              value={settings.audioConfig}
              options={[
                { label: 'Stereo', value: 'stereo' },
                { label: '5.1 surround sound', value: '5.1' },
                { label: '7.1 surround sound', value: '7.1' },
              ]}
              onChange={(audioConfig) =>
                update({ audioConfig: audioConfig as typeof settings.audioConfig })
              }
            />
            <div className={styles.toggleStack}>
              <Toggle
                label="Mute host PC speakers while streaming"
                checked={settings.muteHostAudio}
                onChange={(muteHostAudio) => update({ muteHostAudio })}
              />
              <Toggle
                label="Mute audio stream when Terra is not active"
                note="Planned: focus-aware WASAPI control"
                checked={settings.muteOnFocusLoss}
                disabled
                onChange={(muteOnFocusLoss) => update({ muteOnFocusLoss })}
              />
            </div>
          </SettingGroup>

          {appMode === 'gaming' ? (
            <SettingGroup title="Host behavior" eyebrow="HOST SETTINGS">
              <div className={styles.toggleStack}>
                <Toggle
                  label="Optimize game settings for streaming"
                  checked={settings.gameOptimizations}
                  onChange={(gameOptimizations) => update({ gameOptimizations })}
                />
                <Toggle
                  label="Quit app on host after ending stream"
                  note="Otherwise ending a stream leaves the host application available to resume"
                  checked={settings.quitAppAfter}
                  onChange={(quitAppAfter) => update({ quitAppAfter })}
                />
              </div>
            </SettingGroup>
          ) : (
            <SettingGroup title="Session behavior" eyebrow="WORKSTATION SETTINGS">
              <div className={styles.toggleStack}>
                <Toggle
                  label="Leave desktop available after disconnecting"
                  note="Keeps the host application running for fast reconnection"
                  checked={!settings.quitAppAfter}
                  onChange={(leaveRunning) => update({ quitAppAfter: !leaveRunning })}
                />
                <Toggle
                  label="Keep local display awake while connected"
                  checked={settings.keepAwake}
                  onChange={(keepAwake) => update({ keepAwake })}
                />
              </div>
            </SettingGroup>
          )}

          <SettingGroup title="Application" eyebrow="UI SETTINGS">
            <div className={styles.settingPair}>
              <SelectSetting
                label="Language"
                value={settings.language}
                options={[
                  { label: 'Automatic', value: 'automatic' },
                  { label: 'English', value: 'english' },
                ]}
                note="Additional translations planned"
                onChange={(language) => update({ language: language as typeof settings.language })}
              />
              <SelectSetting
                label="GUI display mode"
                value={settings.uiDisplayMode}
                options={[
                  { label: 'Windowed', value: 'windowed' },
                  { label: 'Maximized', value: 'maximized' },
                  { label: 'Fullscreen', value: 'fullscreen' },
                ]}
                onChange={(uiDisplayMode) =>
                  update({ uiDisplayMode: uiDisplayMode as typeof settings.uiDisplayMode })
                }
              />
            </div>
            <div className={styles.toggleStack}>
              <Toggle
                label="Show connection quality warnings"
                checked={settings.connectionWarnings}
                onChange={(connectionWarnings) => update({ connectionWarnings })}
              />
              <Toggle
                label="Show configuration warnings"
                checked={settings.configurationWarnings}
                onChange={(configurationWarnings) => update({ configurationWarnings })}
              />
              <Toggle
                label="Discord Rich Presence integration"
                note="Planned: Discord IPC"
                checked={settings.richPresence}
                disabled
                onChange={(richPresence) => update({ richPresence })}
              />
              {appMode === 'gaming' && (
                <Toggle
                  label="Keep display awake while streaming"
                  checked={settings.keepAwake}
                  onChange={(keepAwake) => update({ keepAwake })}
                />
              )}
            </div>
          </SettingGroup>
        </div>

        <div>
          <SettingGroup title="Mouse and keyboard" eyebrow="INPUT SETTINGS">
            <SelectSetting
              label="Capture system keyboard shortcuts"
              value={settings.captureSystemKeys}
              options={[
                { label: 'Off', value: 'off' },
                { label: 'Fullscreen only', value: 'fullscreen' },
                { label: 'Always while input is captured', value: 'always' },
              ]}
              note="Ctrl+Alt+Del and compositor-reserved shortcuts cannot be captured"
              onChange={(captureSystemKeys) =>
                update({
                  captureSystemKeys: captureSystemKeys as typeof settings.captureSystemKeys,
                })
              }
            />
            <div className={styles.toggleStack}>
              <Toggle
                label="Optimize mouse for remote desktop instead of games"
                note="Uses absolute host pointer positioning without capture"
                checked={settings.absoluteMouseMode}
                onChange={(absoluteMouseMode) => update({ absoluteMouseMode })}
              />
              <Toggle
                label="Use touchscreen as a virtual trackpad"
                note="Turn off to send direct Sol touch and pen input"
                checked={settings.touchscreenTrackpad}
                onChange={(touchscreenTrackpad) => update({ touchscreenTrackpad })}
              />
              <Toggle
                label="Swap left and right mouse buttons"
                checked={settings.swapMouseButtons}
                onChange={(swapMouseButtons) => update({ swapMouseButtons })}
              />
              <Toggle
                label="Reverse mouse scrolling direction"
                checked={settings.reverseScrollDirection}
                onChange={(reverseScrollDirection) => update({ reverseScrollDirection })}
              />
            </div>
          </SettingGroup>

          <SettingGroup title="Video pipeline" eyebrow="ADVANCED SETTINGS">
            <div className={styles.settingPair}>
              <SelectSetting
                label="Video decoder"
                value={settings.videoDecoder}
                options={[
                  { label: 'Automatic', value: 'automatic' },
                  { label: 'Hardware', value: 'hardware' },
                  { label: 'Software (planned)', value: 'software', disabled: true },
                ]}
                onChange={(videoDecoder) =>
                  update({ videoDecoder: videoDecoder as typeof settings.videoDecoder })
                }
              />
              <SelectSetting
                label="Video codec"
                value={settings.videoCodec}
                options={[
                  { label: 'Automatic', value: 'automatic' },
                  { label: 'H.264', value: 'h264' },
                  { label: 'HEVC', value: 'hevc' },
                  { label: 'AV1', value: 'av1' },
                ]}
                onChange={(videoCodec) =>
                  update({ videoCodec: videoCodec as typeof settings.videoCodec })
                }
              />
            </div>
            <div className={styles.toggleStack}>
              <Toggle
                label="Enable HDR"
                note="Requires Windows HDR or Linux Vulkan HDR10, Main10 host encoding, and compatible GPU"
                checked={settings.enableHdr}
                onChange={(enableHdr) => update({ enableHdr })}
              />
              <Toggle
                label="Enable YUV 4:4:4"
                note="Requires Linux Vulkan presentation and host encoder support"
                checked={settings.enableYuv444}
                onChange={(enableYuv444) => update({ enableYuv444 })}
              />
              <Toggle
                label="Unlock bitrate limit"
                note="Raises slider limit from 150 to 500 Mbps"
                checked={settings.unlockBitrate}
                onChange={(unlockBitrate) =>
                  update({
                    unlockBitrate,
                    bitrateKbps: unlockBitrate
                      ? settings.bitrateKbps
                      : Math.min(settings.bitrateKbps, 150_000),
                  })
                }
              />
              <Toggle
                label="Automatically find PCs on local network"
                note="Discovers compatible hosts through local DNS-SD"
                checked={settings.autoDiscoverHosts}
                onChange={(autoDiscoverHosts) => update({ autoDiscoverHosts })}
              />
              <Toggle
                label="Automatically detect blocked connections"
                note="Tests ports related to failed connection stages"
                checked={settings.detectBlockedConnections}
                onChange={(detectBlockedConnections) => update({ detectBlockedConnections })}
              />
              <Toggle
                label="Show performance stats while streaming"
                note="Shows native video, network, latency, and loss metrics"
                checked={settings.showPerformanceStats}
                onChange={(showPerformanceStats) => update({ showPerformanceStats })}
              />
            </div>
          </SettingGroup>
        </div>
      </div>
    </div>
  )
}
