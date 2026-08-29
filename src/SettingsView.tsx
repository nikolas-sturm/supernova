import { RotateCcw, SlidersHorizontal } from 'lucide-react'
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

export function SettingsView() {
  const settings = useClientStore((state) => state.settings)
  const update = useClientStore((state) => state.updateSettings)
  const reset = useClientStore((state) => state.resetSettings)
  const resolutionValue = `${settings.width}x${settings.height}`
  const recommended = recommendedBitrate(settings.width, settings.height, settings.fps)
  const bitrateMaximum = settings.unlockBitrate ? 500_000 : 150_000

  function selectResolution(value: string) {
    const selected = resolutionOptions.find(({ width, height }) => `${width}x${height}` === value)
    if (!selected) return
    update({
      width: selected.width,
      height: selected.height,
      bitrateKbps: recommendedBitrate(selected.width, selected.height, settings.fps),
    })
  }

  function selectFps(value: string) {
    const fps = Number(value)
    update({ fps, bitrateKbps: recommendedBitrate(settings.width, settings.height, fps) })
  }

  return (
    <div className={styles.settingsPage}>
      <section className={styles.settingsIntro}>
        <div className={styles.settingsIntroIcon}>
          <SlidersHorizontal size={24} />
        </div>
        <div>
          <p className={styles.panelLabel}>STREAM PROFILE / LOCAL CLIENT</p>
          <h2>One profile, applied at next launch.</h2>
          <p>
            Settings persist on this device. Live controls map to native transport; planned controls
            stay visible for Moonlight parity without pretending support.
          </p>
        </div>
        <button type="button" onClick={reset}>
          <RotateCcw size={14} /> Reset defaults
        </button>
      </section>

      <div className={styles.settingsColumns}>
        <div>
          <SettingGroup title="Stream" eyebrow="BASIC SETTINGS">
            <div className={styles.settingPair}>
              <SelectSetting
                label="Resolution"
                value={resolutionValue}
                options={resolutionOptions.map((option) => ({
                  label: option.label,
                  value: `${option.width}x${option.height}`,
                }))}
                onChange={selectResolution}
              />
              <SelectSetting
                label="Frame rate"
                value={settings.fps}
                options={fpsOptions.map((fps) => ({ label: `${fps} FPS`, value: fps }))}
                onChange={selectFps}
              />
            </div>
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
                { label: '5.1 surround sound (planned)', value: '5.1', disabled: true },
                { label: '7.1 surround sound (planned)', value: '7.1', disabled: true },
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
                label="Mute audio stream when Eclipse is not active"
                note="Planned: focus-aware WASAPI control"
                checked={settings.muteOnFocusLoss}
                disabled
                onChange={(muteOnFocusLoss) => update({ muteOnFocusLoss })}
              />
            </div>
          </SettingGroup>

          <SettingGroup title="Host behavior" eyebrow="HOST SETTINGS">
            <div className={styles.toggleStack}>
              <Toggle
                label="Optimize game settings for streaming"
                checked={settings.gameOptimizations}
                onChange={(gameOptimizations) => update({ gameOptimizations })}
              />
              <Toggle
                label="Quit app on host after ending stream"
                note="Planned: disconnect without cancelling host app"
                checked={settings.quitAppAfter}
                disabled
                onChange={(quitAppAfter) => update({ quitAppAfter })}
              />
            </div>
          </SettingGroup>

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
              <Toggle
                label="Keep display awake while streaming"
                checked={settings.keepAwake}
                onChange={(keepAwake) => update({ keepAwake })}
              />
            </div>
          </SettingGroup>
        </div>

        <div>
          <SettingGroup title="Mouse and keyboard" eyebrow="INPUT SETTINGS">
            <div className={styles.toggleStack}>
              <Toggle
                label="Optimize mouse for remote desktop instead of games"
                note="Uses absolute host pointer positioning without capture"
                checked={settings.absoluteMouseMode}
                onChange={(absoluteMouseMode) => update({ absoluteMouseMode })}
              />
              <Toggle
                label="Capture system keyboard shortcuts"
                note="Planned: system-wide keyboard hook"
                checked={settings.captureSystemKeys !== 'off'}
                disabled
                onChange={() => undefined}
              />
              <Toggle
                label="Use touchscreen as a virtual trackpad"
                note="Planned: native pointer and touch input"
                checked={settings.touchscreenTrackpad}
                disabled
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

          <SettingGroup title="Controller" eyebrow="GAMEPAD SETTINGS">
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
                label="Process gamepad input while Eclipse is in background"
                checked={settings.backgroundGamepad}
                onChange={(backgroundGamepad) => update({ backgroundGamepad })}
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
                  { label: 'HEVC (planned)', value: 'hevc', disabled: true },
                  { label: 'AV1 (planned)', value: 'av1', disabled: true },
                ]}
                onChange={(videoCodec) =>
                  update({ videoCodec: videoCodec as typeof settings.videoCodec })
                }
              />
            </div>
            <div className={styles.toggleStack}>
              <Toggle
                label="Enable HDR"
                note="Planned: 10-bit decode and HDR presentation"
                checked={settings.enableHdr}
                disabled
                onChange={(enableHdr) => update({ enableHdr })}
              />
              <Toggle
                label="Enable YUV 4:4:4"
                note="Planned: 4:4:4 decoder path"
                checked={settings.enableYuv444}
                disabled
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
                note="Planned: mDNS discovery"
                checked={settings.autoDiscoverHosts}
                disabled
                onChange={(autoDiscoverHosts) => update({ autoDiscoverHosts })}
              />
              <Toggle
                label="Automatically detect blocked connections"
                note="Planned: connectivity diagnostics"
                checked={settings.detectBlockedConnections}
                disabled
                onChange={(detectBlockedConnections) => update({ detectBlockedConnections })}
              />
              <Toggle
                label="Show performance stats while streaming"
                note="Planned: native stream overlay"
                checked={settings.showPerformanceStats}
                disabled
                onChange={(showPerformanceStats) => update({ showPerformanceStats })}
              />
            </div>
          </SettingGroup>
        </div>
      </div>
    </div>
  )
}
