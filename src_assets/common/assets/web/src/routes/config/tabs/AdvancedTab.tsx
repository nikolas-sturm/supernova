/**
 * @file Advanced settings tab (legacy `configs/tabs/Advanced.vue`).
 */

import { useTranslation } from 'react-i18next'
import { NumberField, SelectField, TextField } from '../../../components/fields'
import type { TabBodyProps } from '../TabContent'

/**
 * @brief Advanced tab: FEC, QP, threads, codec advertisement, capture and
 * encoder overrides.
 * @param props Tab props.
 * @returns The tab content.
 */
export function AdvancedTab({ draft, setDraftValue, platform }: TabBodyProps) {
  const { t } = useTranslation()

  const captureOptions: { value: string; label: string }[] = [
    { value: '', label: t('_common.autodetect') },
  ]
  if (platform === 'windows') {
    captureOptions.push(
      { value: 'ddx', label: 'Desktop Duplication API' },
      { value: 'wgc', label: `Windows.Graphics.Capture ${t('_common.beta')}` },
    )
  } else if (platform === 'linux') {
    captureOptions.push(
      { value: 'nvfbc', label: 'NvFBC' },
      { value: 'wlr', label: 'wlroots' },
      { value: 'kms', label: 'KMS' },
      { value: 'x11', label: 'X11' },
      { value: 'kwin', label: 'KWin Screencast' },
      { value: 'portal', label: 'XDG Portal' },
    )
  } else if (platform === 'freebsd') {
    captureOptions.push(
      { value: 'wlr', label: 'wlroots' },
      { value: 'x11', label: 'X11' },
      { value: 'portal', label: 'XDG Portal' },
    )
  }

  const encoderOptions: { value: string; label: string }[] = [
    { value: '', label: t('_common.autodetect') },
  ]
  if (platform === 'windows') {
    encoderOptions.push(
      { value: 'nvenc', label: 'NVIDIA NVENC' },
      { value: 'quicksync', label: 'Intel QuickSync' },
      { value: 'amdvce', label: 'AMD AMF/VCE' },
    )
  } else if (platform === 'linux') {
    encoderOptions.push(
      { value: 'nvenc', label: 'NVIDIA NVENC' },
      { value: 'vaapi', label: 'VA-API' },
      { value: 'vulkan', label: 'Vulkan' },
    )
  } else if (platform === 'freebsd') {
    encoderOptions.push({ value: 'vulkan', label: 'Vulkan' }, { value: 'vaapi', label: 'VA-API' })
  } else if (platform === 'macos') {
    encoderOptions.push({ value: 'videotoolbox', label: 'VideoToolbox' })
  }
  encoderOptions.push({ value: 'software', label: t('config.encoder_software') })

  return (
    <div className="configPage">
      <TextField
        id="fec_percentage"
        label={t('config.fec_percentage')}
        description={t('config.fec_percentage_desc')}
        value={String(draft.fec_percentage ?? 20)}
        onChange={(value) => setDraftValue('fec_percentage', value)}
        placeholder="20"
      />

      <NumberField
        id="qp"
        label={t('config.qp')}
        description={t('config.qp_desc')}
        value={String(draft.qp ?? 28)}
        onChange={(value) => setDraftValue('qp', value)}
        placeholder="28"
      />

      <NumberField
        id="min_threads"
        label={t('config.min_threads')}
        description={t('config.min_threads_desc')}
        value={String(draft.min_threads ?? 2)}
        onChange={(value) => setDraftValue('min_threads', value)}
        min={1}
        placeholder="2"
      />

      <SelectField
        id="hevc_mode"
        label={t('config.hevc_mode')}
        description={t('config.hevc_mode_desc')}
        value={String(draft.hevc_mode ?? 0)}
        onChange={(value) => setDraftValue('hevc_mode', value)}
        options={[
          { value: '0', label: t('config.hevc_mode_0') },
          { value: '1', label: t('config.hevc_mode_1') },
          { value: '2', label: t('config.hevc_mode_2') },
          { value: '3', label: t('config.hevc_mode_3') },
        ]}
      />

      <SelectField
        id="av1_mode"
        label={t('config.av1_mode')}
        description={t('config.av1_mode_desc')}
        value={String(draft.av1_mode ?? 0)}
        onChange={(value) => setDraftValue('av1_mode', value)}
        options={[
          { value: '0', label: t('config.av1_mode_0') },
          { value: '1', label: t('config.av1_mode_1') },
          { value: '2', label: t('config.av1_mode_2') },
          { value: '3', label: t('config.av1_mode_3') },
        ]}
      />

      {platform !== 'macos' && (
        <SelectField
          id="capture"
          label={t('config.capture')}
          description={t('config.capture_desc')}
          value={String(draft.capture ?? '')}
          onChange={(value) => setDraftValue('capture', value)}
          options={captureOptions}
        />
      )}

      <SelectField
        id="encoder"
        label={t('config.encoder')}
        description={t('config.encoder_desc')}
        value={String(draft.encoder ?? '')}
        onChange={(value) => setDraftValue('encoder', value)}
        options={encoderOptions}
      />
    </div>
  )
}
