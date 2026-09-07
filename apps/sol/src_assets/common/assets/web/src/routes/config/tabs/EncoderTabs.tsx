/**
 * @file Encoder settings tabs (legacy `configs/tabs/encoders/*.vue`).
 */

import { useTranslation } from 'react-i18next'
import { Collapse } from '../../../components/Collapse'
import { CheckboxField, NumberField, SelectField } from '../../../components/fields'
import type { TabBodyProps, TabProps } from '../TabContent'

const coderOptions = (t: (key: string) => string) => [
  { value: 'auto', label: t('config.ffmpeg_auto') },
  { value: 'cabac', label: t('config.coder_cabac') },
  { value: 'cavlc', label: t('config.coder_cavlc') },
]

/**
 * @brief Renders the encoder tab matching the active tab id.
 * @param props Tab props.
 * @returns The encoder tab content.
 */
export function EncoderTabs({ tabId, draft, setDraftValue, platform }: TabProps) {
  switch (tabId) {
    case 'nv':
      return <NvencTab draft={draft} setDraftValue={setDraftValue} platform={platform} />
    case 'qsv':
      return <QuickSyncTab draft={draft} setDraftValue={setDraftValue} platform={platform} />
    case 'amd':
      return <AmdTab draft={draft} setDraftValue={setDraftValue} platform={platform} />
    case 'vt':
      return <VideoToolboxTab draft={draft} setDraftValue={setDraftValue} platform={platform} />
    case 'vaapi':
      return <VaapiTab draft={draft} setDraftValue={setDraftValue} platform={platform} />
    case 'vulkan':
      return <VulkanTab draft={draft} setDraftValue={setDraftValue} platform={platform} />
    default:
      return <SoftwareTab draft={draft} setDraftValue={setDraftValue} platform={platform} />
  }
}

/**
 * @brief NVIDIA NVENC encoder tab.
 * @param props Tab props.
 * @returns The tab content.
 */
function NvencTab({ draft, setDraftValue, platform }: TabBodyProps) {
  const { t } = useTranslation()
  return (
    <div id="nvidia-nvenc-encoder" className="configPage">
      <SelectField
        id="nvenc_preset"
        label={t('config.nvenc_preset')}
        description={t('config.nvenc_preset_desc')}
        value={String(draft.nvenc_preset ?? 1)}
        onChange={(value) => setDraftValue('nvenc_preset', value)}
        options={[
          { value: '1', label: `P1 ${t('config.nvenc_preset_1')}` },
          { value: '2', label: 'P2' },
          { value: '3', label: 'P3' },
          { value: '4', label: 'P4' },
          { value: '5', label: 'P5' },
          { value: '6', label: 'P6' },
          { value: '7', label: `P7 ${t('config.nvenc_preset_7')}` },
        ]}
      />

      {platform === 'windows' && (
        <SelectField
          id="nvenc_split_encode"
          label={t('config.nvenc_split_encode')}
          description={t('config.nvenc_split_encode_desc')}
          value={String(draft.nvenc_split_encode ?? 'driver_decides')}
          onChange={(value) => setDraftValue('nvenc_split_encode', value)}
          options={[
            { value: 'disabled', label: t('_common.disabled') },
            { value: 'driver_decides', label: t('config.nvenc_split_encode_driver_decides_def') },
            { value: 'enabled', label: t('_common.enabled') },
          ]}
        />
      )}

      <SelectField
        id="nvenc_twopass"
        label={t('config.nvenc_twopass')}
        description={t('config.nvenc_twopass_desc')}
        value={String(draft.nvenc_twopass ?? 'quarter_res')}
        onChange={(value) => setDraftValue('nvenc_twopass', value)}
        options={[
          { value: 'disabled', label: t('config.nvenc_twopass_disabled') },
          { value: 'quarter_res', label: t('config.nvenc_twopass_quarter_res') },
          { value: 'full_res', label: t('config.nvenc_twopass_full_res') },
        ]}
      />

      <CheckboxField
        id="nvenc_spatial_aq"
        label={t('config.nvenc_spatial_aq')}
        description={t('config.nvenc_spatial_aq_desc')}
        value={draft.nvenc_spatial_aq ?? 'disabled'}
        onChange={(value) => setDraftValue('nvenc_spatial_aq', value)}
      />

      <NumberField
        id="nvenc_vbv_increase"
        label={t('config.nvenc_vbv_increase')}
        description={
          <>
            {t('config.nvenc_vbv_increase_desc')}
            <br />
            <br />
            <a
              href="https://en.wikipedia.org/wiki/Video_buffering_verifier"
              target="_blank"
              rel="noreferrer"
            >
              VBV/HRD
            </a>
          </>
        }
        value={String(draft.nvenc_vbv_increase ?? 0)}
        onChange={(value) => setDraftValue('nvenc_vbv_increase', value)}
        min={0}
        max={400}
        placeholder="0"
      />

      <Collapse title={t('config.misc')}>
        {platform === 'windows' && (
          <>
            <CheckboxField
              id="nvenc_realtime_hags"
              label={t('config.nvenc_realtime_hags')}
              description={
                <>
                  {t('config.nvenc_realtime_hags_desc')}{' '}
                  <a
                    href="https://devblogs.microsoft.com/directx/hardware-accelerated-gpu-scheduling/"
                    target="_blank"
                    rel="noreferrer"
                  >
                    HAGS
                  </a>
                </>
              }
              value={draft.nvenc_realtime_hags ?? 'enabled'}
              onChange={(value) => setDraftValue('nvenc_realtime_hags', value)}
            />
            <CheckboxField
              id="nvenc_latency_over_power"
              label={t('config.nvenc_latency_over_power')}
              description={t('config.nvenc_latency_over_power_desc')}
              value={draft.nvenc_latency_over_power ?? 'enabled'}
              onChange={(value) => setDraftValue('nvenc_latency_over_power', value)}
            />
            <CheckboxField
              id="nvenc_opengl_vulkan_on_dxgi"
              label={t('config.nvenc_opengl_vulkan_on_dxgi')}
              description={t('config.nvenc_opengl_vulkan_on_dxgi_desc')}
              value={draft.nvenc_opengl_vulkan_on_dxgi ?? 'enabled'}
              onChange={(value) => setDraftValue('nvenc_opengl_vulkan_on_dxgi', value)}
            />
          </>
        )}
        <CheckboxField
          id="nvenc_h264_cavlc"
          label={t('config.nvenc_h264_cavlc')}
          description={t('config.nvenc_h264_cavlc_desc')}
          value={draft.nvenc_h264_cavlc ?? 'disabled'}
          onChange={(value) => setDraftValue('nvenc_h264_cavlc', value)}
        />
      </Collapse>
    </div>
  )
}

/**
 * @brief Intel QuickSync encoder tab.
 * @param props Tab props.
 * @returns The tab content.
 */
function QuickSyncTab({ draft, setDraftValue }: TabBodyProps) {
  const { t } = useTranslation()
  return (
    <div id="intel-quicksync-encoder" className="configPage">
      <SelectField
        id="qsv_preset"
        label={t('config.qsv_preset')}
        value={String(draft.qsv_preset ?? 'medium')}
        onChange={(value) => setDraftValue('qsv_preset', value)}
        options={[
          { value: 'veryfast', label: t('config.qsv_preset_veryfast') },
          { value: 'faster', label: t('config.qsv_preset_faster') },
          { value: 'fast', label: t('config.qsv_preset_fast') },
          { value: 'medium', label: t('config.qsv_preset_medium') },
          { value: 'slow', label: t('config.qsv_preset_slow') },
          { value: 'slower', label: t('config.qsv_preset_slower') },
          { value: 'slowest', label: t('config.qsv_preset_slowest') },
        ]}
      />
      <SelectField
        id="qsv_coder"
        label={t('config.qsv_coder')}
        value={String(draft.qsv_coder ?? 'auto')}
        onChange={(value) => setDraftValue('qsv_coder', value)}
        options={coderOptions(t)}
      />
      <CheckboxField
        id="qsv_slow_hevc"
        label={t('config.qsv_slow_hevc')}
        description={t('config.qsv_slow_hevc_desc')}
        value={draft.qsv_slow_hevc ?? 'disabled'}
        onChange={(value) => setDraftValue('qsv_slow_hevc', value)}
      />
    </div>
  )
}

/**
 * @brief AMD AMF encoder tab.
 * @param props Tab props.
 * @returns The tab content.
 */
function AmdTab({ draft, setDraftValue }: TabBodyProps) {
  const { t } = useTranslation()
  return (
    <div id="amd-amf-encoder" className="configPage">
      <SelectField
        id="amd_usage"
        label={t('config.amd_usage')}
        description={t('config.amd_usage_desc')}
        value={String(draft.amd_usage ?? 'ultralowlatency')}
        onChange={(value) => setDraftValue('amd_usage', value)}
        options={[
          { value: 'transcoding', label: t('config.amd_usage_transcoding') },
          { value: 'webcam', label: t('config.amd_usage_webcam') },
          {
            value: 'lowlatency_high_quality',
            label: t('config.amd_usage_lowlatency_high_quality'),
          },
          { value: 'lowlatency', label: t('config.amd_usage_lowlatency') },
          { value: 'ultralowlatency', label: t('config.amd_usage_ultralowlatency') },
        ]}
      />

      <Collapse title={t('config.amd_rc_group')}>
        <SelectField
          id="amd_rc"
          label={t('config.amd_rc')}
          description={t('config.amd_rc_desc')}
          value={String(draft.amd_rc ?? 'vbr_latency')}
          onChange={(value) => setDraftValue('amd_rc', value)}
          options={[
            { value: 'cbr', label: t('config.amd_rc_cbr') },
            { value: 'cqp', label: t('config.amd_rc_cqp') },
            { value: 'vbr_latency', label: t('config.amd_rc_vbr_latency') },
            { value: 'vbr_peak', label: t('config.amd_rc_vbr_peak') },
          ]}
        />
        <CheckboxField
          id="amd_enforce_hrd"
          label={t('config.amd_enforce_hrd')}
          description={t('config.amd_enforce_hrd_desc')}
          value={draft.amd_enforce_hrd ?? 'disabled'}
          onChange={(value) => setDraftValue('amd_enforce_hrd', value)}
        />
        <NumberField
          id="amd_max_au_size"
          label={t('config.amd_max_au_size')}
          description={t('config.amd_max_au_size_desc')}
          value={String(draft.amd_max_au_size ?? '')}
          onChange={(value) => setDraftValue('amd_max_au_size', value)}
          min={-1}
          placeholder="-1"
        />
      </Collapse>

      <Collapse title={t('config.amd_quality_group')}>
        <SelectField
          id="amd_quality"
          label={t('config.amd_quality')}
          description={t('config.amd_quality_desc')}
          value={String(draft.amd_quality ?? 'balanced')}
          onChange={(value) => setDraftValue('amd_quality', value)}
          options={[
            { value: 'speed', label: t('config.amd_quality_speed') },
            { value: 'balanced', label: t('config.amd_quality_balanced') },
            { value: 'quality', label: t('config.amd_quality_quality') },
          ]}
        />
        <CheckboxField
          id="amd_preanalysis"
          label={t('config.amd_preanalysis')}
          description={t('config.amd_preanalysis_desc')}
          value={draft.amd_preanalysis ?? 'disabled'}
          onChange={(value) => setDraftValue('amd_preanalysis', value)}
        />
        <CheckboxField
          id="amd_vbaq"
          label={t('config.amd_vbaq')}
          description={t('config.amd_vbaq_desc')}
          value={draft.amd_vbaq ?? 'enabled'}
          onChange={(value) => setDraftValue('amd_vbaq', value)}
        />
        <SelectField
          id="amd_coder"
          label={t('config.amd_coder')}
          description={t('config.amd_coder_desc')}
          value={String(draft.amd_coder ?? 'auto')}
          onChange={(value) => setDraftValue('amd_coder', value)}
          options={coderOptions(t)}
        />
      </Collapse>
    </div>
  )
}

/**
 * @brief VideoToolbox encoder tab.
 * @param props Tab props.
 * @returns The tab content.
 */
function VideoToolboxTab({ draft, setDraftValue }: TabBodyProps) {
  const { t } = useTranslation()
  return (
    <div id="videotoolbox-encoder" className="configPage">
      <SelectField
        id="vt_coder"
        label={t('config.vt_coder')}
        value={String(draft.vt_coder ?? 'auto')}
        onChange={(value) => setDraftValue('vt_coder', value)}
        options={coderOptions(t)}
      />
      <SelectField
        id="vt_software"
        label={t('config.vt_software')}
        value={String(draft.vt_software ?? 'auto')}
        onChange={(value) => setDraftValue('vt_software', value)}
        options={[
          { value: 'auto', label: t('_common.auto') },
          { value: 'disabled', label: t('_common.disabled') },
          { value: 'allowed', label: t('config.vt_software_allowed') },
          { value: 'forced', label: t('config.vt_software_forced') },
        ]}
      />
      <CheckboxField
        id="vt_realtime"
        label={t('config.vt_realtime')}
        value={draft.vt_realtime ?? 'enabled'}
        onChange={(value) => setDraftValue('vt_realtime', value)}
      />
    </div>
  )
}

/**
 * @brief VA-API encoder tab.
 * @param props Tab props.
 * @returns The tab content.
 */
function VaapiTab({ draft, setDraftValue }: TabBodyProps) {
  const { t } = useTranslation()
  return (
    <div id="vaapi-encoder" className="configPage">
      <Collapse title={t('config.vaapi_rc_group')}>
        <SelectField
          id="vaapi_rc"
          label={t('config.vaapi_rc')}
          description={t('config.vaapi_rc_desc')}
          value={String(draft.vaapi_rc ?? 'auto')}
          onChange={(value) => setDraftValue('vaapi_rc', value)}
          options={[
            { value: 'auto', label: t('auto') },
            { value: 'avbr', label: t('config.vaapi_rc_avbr') },
            { value: 'vbr', label: t('config.vaapi_rc_vbr') },
            { value: 'cbr', label: t('config.vaapi_rc_cbr') },
            { value: 'cqp', label: t('config.vaapi_rc_cqp') },
            { value: 'icq', label: t('config.vaapi_rc_icq') },
            { value: 'qvbr', label: t('config.vaapi_rc_qvbr') },
          ]}
        />
        <CheckboxField
          id="vaapi_blbrc"
          label={t('config.vaapi_blbrc')}
          description={t('config.vaapi_blbrc_desc')}
          value={draft.vaapi_blbrc ?? 'disabled'}
          onChange={(value) => setDraftValue('vaapi_blbrc', value)}
        />
        <CheckboxField
          id="vaapi_strict_rc_buffer"
          label={t('config.vaapi_strict_rc_buffer')}
          description={t('config.vaapi_strict_rc_buffer_desc')}
          value={draft.vaapi_strict_rc_buffer ?? 'disabled'}
          onChange={(value) => setDraftValue('vaapi_strict_rc_buffer', value)}
        />
      </Collapse>

      <Collapse title={t('config.vaapi_quality_group')}>
        <SelectField
          id="vaapi_quality"
          label={t('config.vaapi_quality')}
          description={t('config.vaapi_quality_desc')}
          value={String(draft.vaapi_quality ?? 'auto')}
          onChange={(value) => setDraftValue('vaapi_quality', value)}
          options={[
            { value: 'auto', label: t('auto') },
            { value: 'speed', label: t('config.vaapi_quality_speed') },
            { value: 'balanced', label: t('config.vaapi_quality_balanced') },
            { value: 'quality', label: t('config.vaapi_quality_quality') },
          ]}
        />
      </Collapse>
    </div>
  )
}

/**
 * @brief Vulkan encoder tab.
 * @param props Tab props.
 * @returns The tab content.
 */
function VulkanTab({ draft, setDraftValue }: TabBodyProps) {
  const { t } = useTranslation()
  return (
    <div id="vulkan-encoder" className="configPage">
      <SelectField
        id="vk_tune"
        label={t('config.vk_tune')}
        description={t('config.vk_tune_desc')}
        value={String(draft.vk_tune ?? 2)}
        onChange={(value) => setDraftValue('vk_tune', value)}
        options={[
          { value: '0', label: t('_common.auto') },
          { value: '1', label: t('config.vk_tune_hq') },
          { value: '2', label: t('config.vk_tune_ll') },
          { value: '3', label: t('config.vk_tune_ull') },
        ]}
      />
      <SelectField
        id="vk_rc_mode"
        label={t('config.vk_rc_mode')}
        description={t('config.vk_rc_mode_desc')}
        value={String(draft.vk_rc_mode ?? 2)}
        onChange={(value) => setDraftValue('vk_rc_mode', value)}
        options={[
          { value: '0', label: t('_common.auto') },
          { value: '1', label: t('config.vk_rc_cqp') },
          { value: '2', label: t('config.vk_rc_cbr') },
          { value: '4', label: t('config.vk_rc_vbr') },
        ]}
      />
    </div>
  )
}

/**
 * @brief Software encoder tab.
 * @param props Tab props.
 * @returns The tab content.
 */
function SoftwareTab({ draft, setDraftValue }: TabBodyProps) {
  const { t } = useTranslation()
  return (
    <div id="software-encoder" className="configPage">
      <SelectField
        id="sw_preset"
        label={t('config.sw_preset')}
        description={t('config.sw_preset_desc')}
        value={String(draft.sw_preset ?? 'superfast')}
        onChange={(value) => setDraftValue('sw_preset', value)}
        options={[
          { value: 'ultrafast', label: t('config.sw_preset_ultrafast') },
          { value: 'superfast', label: t('config.sw_preset_superfast') },
          { value: 'veryfast', label: t('config.sw_preset_veryfast') },
          { value: 'faster', label: t('config.sw_preset_faster') },
          { value: 'fast', label: t('config.sw_preset_fast') },
          { value: 'medium', label: t('config.sw_preset_medium') },
          { value: 'slow', label: t('config.sw_preset_slow') },
          { value: 'slower', label: t('config.sw_preset_slower') },
          { value: 'veryslow', label: t('config.sw_preset_veryslow') },
        ]}
      />
      <SelectField
        id="sw_tune"
        label={t('config.sw_tune')}
        description={t('config.sw_tune_desc')}
        value={String(draft.sw_tune ?? 'zerolatency')}
        onChange={(value) => setDraftValue('sw_tune', value)}
        options={[
          { value: 'film', label: t('config.sw_tune_film') },
          { value: 'animation', label: t('config.sw_tune_animation') },
          { value: 'grain', label: t('config.sw_tune_grain') },
          { value: 'stillimage', label: t('config.sw_tune_stillimage') },
          { value: 'fastdecode', label: t('config.sw_tune_fastdecode') },
          { value: 'zerolatency', label: t('config.sw_tune_zerolatency') },
        ]}
      />
    </div>
  )
}
