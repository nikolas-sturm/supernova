/**
 * @file Config tab registry, default population, and default-stripping logic.
 * Ported from the legacy `config.html` script section.
 */

/** A config tab definition. */
export interface ConfigTab {
  /** Tab id used in URLs and lookup. */
  id: string
  /** Display name. */
  name: string
  /** Default values for the options on this tab. */
  options: Record<string, unknown>
}

/** Encoder tab ids, displayed in a separate group. */
export const ENCODER_TAB_IDS = new Set(['nv', 'amd', 'qsv', 'vaapi', 'vt', 'vulkan', 'sw'])

/** Full tab registry with defaults for every option exposed in the UI. */
export const CONFIG_TABS: ConfigTab[] = [
  {
    id: 'general',
    name: 'General',
    options: {
      locale: 'en',
      sunshine_name: '',
      min_log_level: 2,
      global_prep_cmd: [],
      notify_pre_releases: 'disabled',
      system_tray: 'enabled',
    },
  },
  {
    id: 'input',
    name: 'Input',
    options: {
      controller: 'enabled',
      gamepad: 'auto',
      ds4_back_as_touchpad_click: 'enabled',
      motion_as_ds4: 'enabled',
      touchpad_as_ds4: 'enabled',
      virtualhid_randomize_mac: 'enabled',
      back_button_timeout: -1,
      keyboard: 'enabled',
      key_repeat_delay: 500,
      key_repeat_frequency: 24.9,
      always_send_scancodes: 'enabled',
      key_rightalt_to_key_win: 'disabled',
      mouse: 'enabled',
      high_resolution_scrolling: 'enabled',
      native_pen_touch: 'enabled',
    },
  },
  {
    id: 'av',
    name: 'Audio/Video',
    options: {
      audio_sink: '',
      virtual_sink: '',
      stream_audio: 'enabled',
      install_steam_audio_drivers: 'enabled',
      adapter_name: '',
      output_name: '',
      dd_configuration_option: 'disabled',
      dd_resolution_option: 'auto',
      dd_manual_resolution: '',
      dd_refresh_rate_option: 'auto',
      dd_manual_refresh_rate: '',
      dd_hdr_option: 'auto',
      dd_wa_hdr_toggle_delay: 0,
      dd_config_revert_delay: 3000,
      dd_config_revert_on_disconnect: 'disabled',
      dd_mode_remapping: { mixed: [], resolution_only: [], refresh_rate_only: [] },
      max_bitrate: 0,
      minimum_fps_target: 0,
    },
  },
  {
    id: 'network',
    name: 'Network',
    options: {
      upnp: 'disabled',
      address_family: 'ipv4',
      bind_address: '',
      port: 47989,
      origin_web_ui_allowed: 'lan',
      csrf_allowed_origins: '',
      external_ip: '',
      lan_encryption_mode: 0,
      wan_encryption_mode: 1,
      ping_timeout: 10000,
      packetsize: 0,
    },
  },
  {
    id: 'files',
    name: 'Config Files',
    options: {
      file_apps: '',
      credentials_file: '',
      log_path: '',
      pkey: '',
      cert: '',
      file_state: '',
    },
  },
  {
    id: 'advanced',
    name: 'Advanced',
    options: {
      fec_percentage: 20,
      qp: 28,
      min_threads: 2,
      hevc_mode: 0,
      av1_mode: 0,
      capture: '',
      encoder: '',
    },
  },
  {
    id: 'nv',
    name: 'NVIDIA NVENC Encoder',
    options: {
      nvenc_preset: 1,
      nvenc_twopass: 'quarter_res',
      nvenc_spatial_aq: 'disabled',
      nvenc_vbv_increase: 0,
      nvenc_realtime_hags: 'enabled',
      nvenc_split_encode: 'driver_decides',
      nvenc_latency_over_power: 'enabled',
      nvenc_opengl_vulkan_on_dxgi: 'enabled',
      nvenc_h264_cavlc: 'disabled',
    },
  },
  {
    id: 'qsv',
    name: 'Intel QuickSync Encoder',
    options: {
      qsv_preset: 'medium',
      qsv_coder: 'auto',
      qsv_slow_hevc: 'disabled',
    },
  },
  {
    id: 'amd',
    name: 'AMD AMF Encoder',
    options: {
      amd_usage: 'ultralowlatency',
      amd_rc: 'vbr_latency',
      amd_enforce_hrd: 'disabled',
      amd_max_au_size: '',
      amd_quality: 'balanced',
      amd_preanalysis: 'disabled',
      amd_vbaq: 'enabled',
      amd_coder: 'auto',
    },
  },
  {
    id: 'vt',
    name: 'VideoToolbox Encoder',
    options: {
      vt_coder: 'auto',
      vt_software: 'auto',
      vt_realtime: 'enabled',
    },
  },
  {
    id: 'vaapi',
    name: 'VA-API Encoder',
    options: {
      vaapi_blbrc: 'disabled',
      vaapi_quality: 'auto',
      vaapi_rc: 'auto',
      vaapi_strict_rc_buffer: 'disabled',
    },
  },
  {
    id: 'vulkan',
    name: 'Vulkan Encoder',
    options: {
      vk_tune: 2,
      vk_rc_mode: 2,
    },
  },
  {
    id: 'sw',
    name: 'Software Encoder',
    options: {
      sw_preset: 'superfast',
      sw_tune: 'zerolatency',
    },
  },
]

/**
 * @brief Filters tabs by platform, matching the legacy visibility rules.
 * @param tabs The full tab list.
 * @param platform The host platform.
 * @returns The visible tabs.
 */
export function tabsForPlatform(tabs: ConfigTab[], platform: string): ConfigTab[] {
  const exclude = new Set<string>()
  if (platform === 'windows') {
    exclude.add('vt')
    exclude.add('vaapi')
    exclude.add('vulkan')
  } else if (platform === 'freebsd' || platform === 'linux') {
    exclude.add('amd')
    exclude.add('qsv')
    exclude.add('vt')
  } else if (platform === 'macos') {
    exclude.add('amd')
    exclude.add('nv')
    exclude.add('qsv')
    exclude.add('vaapi')
    exclude.add('vulkan')
  }
  return tabs.filter((tab) => !exclude.has(tab.id))
}

/**
 * @brief Keys whose values arrive JSON-stringified from the backend.
 */
const SPECIAL_OPTIONS = ['dd_mode_remapping', 'global_prep_cmd']

/**
 * @brief Produces the editable config draft: parses special JSON-string
 * values and fills missing keys with tab defaults.
 * @param config The raw config from the API.
 * @param tabs The visible tabs (defaults are taken from these).
 * @returns The populated draft.
 */
export function populateConfigDraft(
  config: Record<string, unknown>,
  tabs: ConfigTab[],
): Record<string, unknown> {
  const draft: Record<string, unknown> = { ...config }
  delete draft.platform
  delete draft.status
  delete draft.version

  for (const optionKey of SPECIAL_OPTIONS) {
    const value = draft[optionKey]
    if (typeof value === 'string') {
      try {
        draft[optionKey] = JSON.parse(value)
      } catch {
        // Leave the raw string when parsing fails.
      }
    }
  }

  for (const tab of tabs) {
    for (const [optionKey, defaultValue] of Object.entries(tab.options)) {
      if (draft[optionKey] === undefined) {
        draft[optionKey] = structuredClone(defaultValue)
      }
    }
  }
  return draft
}

/**
 * @brief Removes values equal to their tab defaults so only deltas are saved.
 * @param draft The populated draft.
 * @param tabs The visible tabs.
 * @returns A payload containing only non-default values.
 */
export function stripDefaultValues(
  draft: Record<string, unknown>,
  tabs: ConfigTab[],
): Record<string, unknown> {
  const payload: Record<string, unknown> = structuredClone(draft)
  for (const tab of tabs) {
    for (const [optionKey, defaultValue] of Object.entries(tab.options)) {
      if (JSON.stringify(payload[optionKey]) === JSON.stringify(defaultValue)) {
        delete payload[optionKey]
      }
    }
  }
  return payload
}

/**
 * @brief Builds the search index of option keys across tabs.
 * @param tabs The visible tabs.
 * @returns Entries with key, generated label, and tab metadata.
 */
export function buildOptionIndex(
  tabs: ConfigTab[],
): { key: string; label: string; tabId: string; tabName: string }[] {
  const entries: { key: string; label: string; tabId: string; tabName: string }[] = []
  for (const tab of tabs) {
    for (const key of Object.keys(tab.options)) {
      const label = key.replaceAll('_', ' ').replaceAll(/\b\w/g, (letter) => letter.toUpperCase())
      entries.push({ key, label, tabId: tab.id, tabName: tab.name })
    }
  }
  return entries
}
