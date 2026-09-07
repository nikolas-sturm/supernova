/**
 * @file Audio/Video settings tab (legacy `configs/tabs/AudioVideo.vue` and
 * its `audiovideo/` subcomponents).
 */

import { Trash2 } from 'lucide-react'
import { useTranslation } from 'react-i18next'
import { Collapse } from '../../../components/Collapse'
import { CheckboxField, NumberField, SelectField, TextField } from '../../../components/fields'
import { PlatformSwitch, usePlatformTranslation } from '../../../components/Platform'
import { Button } from '../../../components/ui'
import type { TabBodyProps } from '../TabContent'
import styles from './AudioVideoTab.module.css'

/** Remapping entry shape for dd_mode_remapping. */
interface RemappingEntry {
  requested_resolution?: string
  requested_fps?: string
  final_resolution?: string
  final_refresh_rate?: string
}

/** The three remapping buckets. */
type RemappingMap = Record<'mixed' | 'resolution_only' | 'refresh_rate_only', RemappingEntry[]>

/**
 * @brief Audio/Video tab: sinks, adapter/output selection, display device
 * options, mode remapping, and stream quality limits.
 * @param props Tab props.
 * @returns The tab content.
 */
export function AudioVideoTab({ draft, setDraftValue, platform }: TabBodyProps) {
  const { t } = useTranslation()
  const tp = usePlatformTranslation()

  const ddConfiguration = String(draft.dd_configuration_option ?? 'disabled')
  const ddResolutionOption = String(draft.dd_resolution_option ?? 'auto')
  const ddRefreshRateOption = String(draft.dd_refresh_rate_option ?? 'auto')

  const remapping = (draft.dd_mode_remapping ?? {
    mixed: [],
    resolution_only: [],
    refresh_rate_only: [],
  }) as RemappingMap

  const canBeRemapped =
    (ddResolutionOption === 'auto' || ddRefreshRateOption === 'auto') &&
    ddConfiguration !== 'disabled'
  const remappingType =
    ddResolutionOption !== 'auto'
      ? 'refresh_rate_only'
      : ddRefreshRateOption !== 'auto'
        ? 'resolution_only'
        : 'mixed'
  const remappingEntries = remapping[remappingType] ?? []

  /**
   * @brief Appends an empty remapping row matching the active bucket.
   */
  const addRemappingEntry = () => {
    const row: RemappingEntry = {}
    if (remappingType !== 'resolution_only') {
      row.requested_fps = ''
      row.final_refresh_rate = ''
    }
    if (remappingType !== 'refresh_rate_only') {
      row.requested_resolution = ''
      row.final_resolution = ''
    }
    setDraftValue('dd_mode_remapping', {
      ...remapping,
      [remappingType]: [...remappingEntries, row],
    })
  }

  const outputNamePlaceholder =
    platform === 'windows'
      ? '{de9bb7e2-186e-505b-9e93-f48793333810}'
      : platform === 'linux' || platform === 'freebsd'
        ? 'DP-0'
        : '0'

  return (
    <div id="audio-video" className="configPage">
      <TextField
        id="audio_sink"
        label={t('config.audio_sink')}
        description={
          <>
            {tp('config.audio_sink_desc')}
            <br />
            <PlatformSwitch
              windows={<pre>tools\audio-info.exe</pre>}
              linux={
                <>
                  <pre>pacmd list-sinks | grep "name:"</pre>
                  <pre>pactl info | grep Source</pre>
                </>
              }
              freebsd={
                <>
                  <pre>pacmd list-sinks | grep "name:"</pre>
                  <pre>pactl info | grep Source</pre>
                </>
              }
              macos={
                <>
                  <a
                    href="https://github.com/mattingalls/Soundflower"
                    target="_blank"
                    rel="noreferrer"
                  >
                    Soundflower
                  </a>
                  <br />
                  <a
                    href="https://github.com/ExistentialAudio/BlackHole"
                    target="_blank"
                    rel="noreferrer"
                  >
                    BlackHole
                  </a>
                  .
                </>
              }
            />
          </>
        }
        value={String(draft.audio_sink ?? '')}
        onChange={(value) => setDraftValue('audio_sink', value)}
        placeholder={tp(
          'config.audio_sink_placeholder',
          'alsa_output.pci-0000_09_00.3.analog-stereo',
        )}
      />

      <PlatformSwitch
        windows={
          <>
            <TextField
              id="virtual_sink"
              label={t('config.virtual_sink')}
              description={t('config.virtual_sink_desc')}
              value={String(draft.virtual_sink ?? '')}
              onChange={(value) => setDraftValue('virtual_sink', value)}
              placeholder={t('config.virtual_sink_placeholder')}
            />
            <CheckboxField
              id="install_steam_audio_drivers"
              label={t('config.install_steam_audio_drivers')}
              description={t('config.install_steam_audio_drivers_desc')}
              value={draft.install_steam_audio_drivers ?? 'enabled'}
              onChange={(value) => setDraftValue('install_steam_audio_drivers', value)}
            />
          </>
        }
      />

      <CheckboxField
        id="stream_audio"
        label={t('config.stream_audio')}
        description={t('config.stream_audio_desc')}
        value={draft.stream_audio ?? 'enabled'}
        onChange={(value) => setDraftValue('stream_audio', value)}
      />

      {platform !== 'macos' && (
        <div className={styles.withBrowse}>
          <TextField
            id="adapter_name"
            label={t('config.adapter_name')}
            description={
              <PlatformSwitch
                windows={
                  <>
                    {t('config.adapter_name_desc_windows')}
                    <br />
                    <pre>tools\dxgi-info.exe</pre>
                  </>
                }
                linux={
                  <>
                    {t('config.adapter_name_desc_linux_1')}
                    <br />
                    <pre>ls /dev/dri/renderD* # {t('config.adapter_name_desc_linux_2')}</pre>
                    <pre>
                      {
                        'vainfo --display drm --device /dev/dri/renderD129 | \\\n  grep -E "((VAProfileH264High|VAProfileHEVCMain|VAProfileHEVCMain10).*VAEntrypointEncSlice)|Driver version"'
                      }
                    </pre>
                    {t('config.adapter_name_desc_linux_3')}
                    <br />
                    <i>VAProfileH264High : VAEntrypointEncSlice</i>
                  </>
                }
                freebsd={
                  <>
                    {t('config.adapter_name_desc_linux_1')}
                    <br />
                    <pre>ls /dev/dri/renderD* # {t('config.adapter_name_desc_linux_2')}</pre>
                    <pre>
                      {
                        'vainfo --display drm --device /dev/dri/renderD129 | \\\n  grep -E "((VAProfileH264High|VAProfileHEVCMain|VAProfileHEVCMain10).*VAEntrypointEncSlice)|Driver version"'
                      }
                    </pre>
                    {t('config.adapter_name_desc_linux_3')}
                    <br />
                    <i>VAProfileH264High : VAEntrypointEncSlice</i>
                  </>
                }
              />
            }
            value={String(draft.adapter_name ?? '')}
            onChange={(value) => setDraftValue('adapter_name', value)}
            placeholder={tp('config.adapter_name_placeholder', '/dev/dri/renderD128')}
          />
        </div>
      )}

      <TextField
        id="output_name"
        label={t('config.output_name')}
        description={
          <>
            {tp('config.output_name_desc')}
            <br />
            <PlatformSwitch
              windows={
                <pre>
                  {
                    '  {\n    "device_id": "{de9bb7e2-186e-505b-9e93-f48793333810}"\n    "display_name": "\\\\\\\\.\\\\DISPLAY1"\n    "friendly_name": "ROG PG279Q"\n    ...\n  }'
                  }
                </pre>
              }
              linux={
                <pre>
                  {
                    'Info: Detecting displays\nInfo: Detected display: HDMI-A-1 connected: true\nInfo: Detected display: DP-1 connected: true\nInfo: Detected display: DP-2 connected: false'
                  }
                </pre>
              }
              freebsd={
                <pre>
                  {
                    'Info: Detecting displays\nInfo: Detected display: HDMI-A-1 connected: true\nInfo: Detected display: DP-1 connected: true\nInfo: Detected display: DP-2 connected: false'
                  }
                </pre>
              }
              macos={
                <pre>
                  {
                    'Info: Detecting displays\nInfo: Detected display: Monitor-0 (id: 3) connected: true\nInfo: Detected display: Monitor-1 (id: 2) connected: true'
                  }
                </pre>
              }
            />
          </>
        }
        value={String(draft.output_name ?? '')}
        onChange={(value) => setDraftValue('output_name', value)}
        placeholder={outputNamePlaceholder}
      />

      {platform === 'windows' && (
        <Collapse title={t('config.dd_options_header')}>
          <SelectField
            id="dd_configuration_option"
            label={t('config.dd_configuration_option')}
            value={ddConfiguration}
            onChange={(value) => setDraftValue('dd_configuration_option', value)}
            options={[
              { value: 'disabled', label: t('_common.disabled_def') },
              { value: 'verify_only', label: t('config.dd_config_verify_only') },
              { value: 'ensure_active', label: t('config.dd_config_ensure_active') },
              { value: 'ensure_primary', label: t('config.dd_config_ensure_primary') },
              { value: 'ensure_only_display', label: t('config.dd_config_ensure_only_display') },
            ]}
          />

          {ddConfiguration !== 'disabled' && (
            <>
              <SelectField
                id="dd_resolution_option"
                label={t('config.dd_resolution_option')}
                description={
                  ddResolutionOption !== 'disabled'
                    ? t('config.dd_resolution_option_ogs_desc')
                    : undefined
                }
                value={ddResolutionOption}
                onChange={(value) => setDraftValue('dd_resolution_option', value)}
                options={[
                  { value: 'disabled', label: t('config.dd_resolution_option_disabled') },
                  { value: 'auto', label: t('config.dd_resolution_option_auto') },
                  { value: 'manual', label: t('config.dd_resolution_option_manual') },
                ]}
              />
              {ddResolutionOption === 'manual' && (
                <TextField
                  id="dd_manual_resolution"
                  label={t('config.dd_manual_resolution')}
                  value={String(draft.dd_manual_resolution ?? '')}
                  onChange={(value) => setDraftValue('dd_manual_resolution', value)}
                  placeholder="2560x1440"
                  mono
                />
              )}

              <SelectField
                id="dd_refresh_rate_option"
                label={t('config.dd_refresh_rate_option')}
                value={ddRefreshRateOption}
                onChange={(value) => setDraftValue('dd_refresh_rate_option', value)}
                options={[
                  { value: 'disabled', label: t('config.dd_refresh_rate_option_disabled') },
                  { value: 'auto', label: t('config.dd_refresh_rate_option_auto') },
                  { value: 'manual', label: t('config.dd_refresh_rate_option_manual') },
                ]}
              />
              {ddRefreshRateOption === 'manual' && (
                <TextField
                  id="dd_manual_refresh_rate"
                  label={t('config.dd_manual_refresh_rate')}
                  value={String(draft.dd_manual_refresh_rate ?? '')}
                  onChange={(value) => setDraftValue('dd_manual_refresh_rate', value)}
                  placeholder="59.9558"
                  mono
                />
              )}

              <SelectField
                id="dd_hdr_option"
                label={t('config.dd_hdr_option')}
                value={String(draft.dd_hdr_option ?? 'auto')}
                onChange={(value) => setDraftValue('dd_hdr_option', value)}
                options={[
                  { value: 'disabled', label: t('config.dd_hdr_option_disabled') },
                  { value: 'auto', label: t('config.dd_hdr_option_auto') },
                ]}
              />

              <NumberField
                id="dd_wa_hdr_toggle_delay"
                label={t('config.dd_wa_hdr_toggle_delay')}
                description={
                  <>
                    {t('config.dd_wa_hdr_toggle_delay_desc_1')}
                    <br />
                    {t('config.dd_wa_hdr_toggle_delay_desc_2')}
                    <br />
                    {t('config.dd_wa_hdr_toggle_delay_desc_3')}
                  </>
                }
                value={String(draft.dd_wa_hdr_toggle_delay ?? 0)}
                onChange={(value) => setDraftValue('dd_wa_hdr_toggle_delay', value)}
                min={0}
                max={3000}
                placeholder="0"
              />

              <NumberField
                id="dd_config_revert_delay"
                label={t('config.dd_config_revert_delay')}
                description={t('config.dd_config_revert_delay_desc')}
                value={String(draft.dd_config_revert_delay ?? 3000)}
                onChange={(value) => setDraftValue('dd_config_revert_delay', value)}
                min={0}
                placeholder="3000"
              />

              <CheckboxField
                id="dd_config_revert_on_disconnect"
                label={t('config.dd_config_revert_on_disconnect')}
                description={t('config.dd_config_revert_on_disconnect_desc')}
                value={draft.dd_config_revert_on_disconnect ?? 'disabled'}
                onChange={(value) => setDraftValue('dd_config_revert_on_disconnect', value)}
              />

              {canBeRemapped && (
                <div className="configSection">
                  <div className="configSectionLabel">{t('config.dd_mode_remapping')}</div>
                  <div className="configSectionDesc">
                    {t('config.dd_mode_remapping_desc_1')}
                    <br />
                    {t('config.dd_mode_remapping_desc_2')}
                    <br />
                    {t('config.dd_mode_remapping_desc_3')}
                    <br />
                    {t(
                      remappingType === 'mixed'
                        ? 'config.dd_mode_remapping_desc_4_final_values_mixed'
                        : 'config.dd_mode_remapping_desc_4_final_values_non_mixed',
                    )}
                    <br />
                    {remappingType === 'mixed' &&
                      t('config.dd_mode_remapping_desc_5_sops_mixed_only')}
                    {remappingType === 'resolution_only' &&
                      t('config.dd_mode_remapping_desc_5_sops_resolution_only')}
                  </div>

                  {remappingEntries.length > 0 && (
                    <table className="table">
                      <thead>
                        <tr>
                          {remappingType !== 'refresh_rate_only' && (
                            <th>{t('config.dd_mode_remapping_requested_resolution')}</th>
                          )}
                          {remappingType !== 'resolution_only' && (
                            <th>{t('config.dd_mode_remapping_requested_fps')}</th>
                          )}
                          {remappingType !== 'refresh_rate_only' && (
                            <th>{t('config.dd_mode_remapping_final_resolution')}</th>
                          )}
                          {remappingType !== 'resolution_only' && (
                            <th>{t('config.dd_mode_remapping_final_refresh_rate')}</th>
                          )}
                          <th />
                        </tr>
                      </thead>
                      <tbody>
                        {remappingEntries.map((entry, index) => (
                          // biome-ignore lint/suspicious/noArrayIndexKey: rows are positional editing entries
                          <tr key={index}>
                            {remappingType !== 'refresh_rate_only' && (
                              <td>
                                <input
                                  type="text"
                                  className="mono inputCell"
                                  placeholder="1920x1080"
                                  value={entry.requested_resolution ?? ''}
                                  onChange={(event) => {
                                    const next = [...remappingEntries]
                                    next[index] = {
                                      ...entry,
                                      requested_resolution: event.target.value,
                                    }
                                    setDraftValue('dd_mode_remapping', {
                                      ...remapping,
                                      [remappingType]: next,
                                    })
                                  }}
                                />
                              </td>
                            )}
                            {remappingType !== 'resolution_only' && (
                              <td>
                                <input
                                  type="text"
                                  className="mono inputCell"
                                  placeholder="60"
                                  value={entry.requested_fps ?? ''}
                                  onChange={(event) => {
                                    const next = [...remappingEntries]
                                    next[index] = { ...entry, requested_fps: event.target.value }
                                    setDraftValue('dd_mode_remapping', {
                                      ...remapping,
                                      [remappingType]: next,
                                    })
                                  }}
                                />
                              </td>
                            )}
                            {remappingType !== 'refresh_rate_only' && (
                              <td>
                                <input
                                  type="text"
                                  className="mono inputCell"
                                  placeholder="2560x1440"
                                  value={entry.final_resolution ?? ''}
                                  onChange={(event) => {
                                    const next = [...remappingEntries]
                                    next[index] = { ...entry, final_resolution: event.target.value }
                                    setDraftValue('dd_mode_remapping', {
                                      ...remapping,
                                      [remappingType]: next,
                                    })
                                  }}
                                />
                              </td>
                            )}
                            {remappingType !== 'resolution_only' && (
                              <td>
                                <input
                                  type="text"
                                  className="mono inputCell"
                                  placeholder="119.95"
                                  value={entry.final_refresh_rate ?? ''}
                                  onChange={(event) => {
                                    const next = [...remappingEntries]
                                    next[index] = {
                                      ...entry,
                                      final_refresh_rate: event.target.value,
                                    }
                                    setDraftValue('dd_mode_remapping', {
                                      ...remapping,
                                      [remappingType]: next,
                                    })
                                  }}
                                />
                              </td>
                            )}
                            <td>
                              <Button
                                small
                                variant="danger"
                                aria-label={t('_common.close')}
                                onClick={() =>
                                  setDraftValue('dd_mode_remapping', {
                                    ...remapping,
                                    [remappingType]: remappingEntries.filter((_, i) => i !== index),
                                  })
                                }
                              >
                                <Trash2 size={14} aria-hidden />
                              </Button>
                            </td>
                          </tr>
                        ))}
                      </tbody>
                    </table>
                  )}

                  <Button variant="success" onClick={addRemappingEntry}>
                    + {t('config.dd_mode_remapping_add')}
                  </Button>
                </div>
              )}
            </>
          )}
        </Collapse>
      )}

      <NumberField
        id="max_bitrate"
        label={t('config.max_bitrate')}
        description={t('config.max_bitrate_desc')}
        value={String(draft.max_bitrate ?? 0)}
        onChange={(value) => setDraftValue('max_bitrate', value)}
        placeholder="0"
      />

      <NumberField
        id="minimum_fps_target"
        label={t('config.minimum_fps_target')}
        description={t('config.minimum_fps_target_desc')}
        value={String(draft.minimum_fps_target ?? 0)}
        onChange={(value) => setDraftValue('minimum_fps_target', value)}
        min={0}
        max={1000}
        placeholder="0"
      />
    </div>
  )
}
