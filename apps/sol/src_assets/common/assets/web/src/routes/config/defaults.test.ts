/**
 * @file Tests for config draft population and default stripping.
 */

import { describe, expect, it } from 'vitest'
import {
  buildOptionIndex,
  CONFIG_TABS,
  populateConfigDraft,
  stripDefaultValues,
  tabsForPlatform,
} from './defaults'

describe('tabsForPlatform', () => {
  it('excludes vt/vaapi/vulkan on windows', () => {
    const ids = tabsForPlatform(CONFIG_TABS, 'windows').map((tab) => tab.id)
    expect(ids).not.toContain('vt')
    expect(ids).not.toContain('vaapi')
    expect(ids).not.toContain('vulkan')
    expect(ids).toContain('nv')
  })

  it('excludes amd/qsv/vt on linux and freebsd', () => {
    for (const platform of ['linux', 'freebsd']) {
      const ids = tabsForPlatform(CONFIG_TABS, platform).map((tab) => tab.id)
      expect(ids).not.toContain('amd')
      expect(ids).not.toContain('qsv')
      expect(ids).not.toContain('vt')
      expect(ids).toContain('vaapi')
    }
  })

  it('excludes all hardware encoders except videotoolbox on macos', () => {
    const ids = tabsForPlatform(CONFIG_TABS, 'macos').map((tab) => tab.id)
    expect(ids).not.toContain('nv')
    expect(ids).not.toContain('amd')
    expect(ids).not.toContain('qsv')
    expect(ids).not.toContain('vaapi')
    expect(ids).not.toContain('vulkan')
    expect(ids).toContain('vt')
  })
})

describe('populateConfigDraft', () => {
  it('fills missing keys with tab defaults', () => {
    const draft = populateConfigDraft(
      { platform: 'windows', version: '1.0', sunshine_name: 'PC', username: 'account-user' },
      CONFIG_TABS,
    )
    expect(draft.sunshine_name).toBe('PC')
    expect(draft.locale).toBe('en')
    expect(draft.port).toBe(47989)
    expect(draft.sw_preset).toBe('superfast')
    expect(draft.platform).toBeUndefined()
    expect(draft.version).toBeUndefined()
    expect(draft.username).toBeUndefined()
  })

  it('parses JSON-stringified special options', () => {
    const draft = populateConfigDraft(
      { global_prep_cmd: JSON.stringify([{ do: 'a', undo: 'b' }]) },
      CONFIG_TABS,
    )
    expect(draft.global_prep_cmd).toEqual([{ do: 'a', undo: 'b' }])
  })

  it('keeps existing values untouched', () => {
    const draft = populateConfigDraft({ port: 1234 }, CONFIG_TABS)
    expect(draft.port).toBe(1234)
  })
})

describe('stripDefaultValues', () => {
  it('removes values equal to defaults and keeps others', () => {
    const draft = populateConfigDraft({ port: 1234 }, CONFIG_TABS)
    const payload = stripDefaultValues(draft, CONFIG_TABS)
    expect(payload.port).toBe(1234)
    expect(payload.locale).toBeUndefined()
    expect(payload.sw_preset).toBeUndefined()
  })

  it('keeps arrays that differ from defaults', () => {
    const draft = populateConfigDraft({}, CONFIG_TABS)
    ;(draft as Record<string, unknown>).global_prep_cmd = [{ do: 'x', undo: 'y' }]
    const payload = stripDefaultValues(draft, CONFIG_TABS)
    expect(payload.global_prep_cmd).toEqual([{ do: 'x', undo: 'y' }])
  })
})

describe('buildOptionIndex', () => {
  it('indexes every option key with its tab', () => {
    const index = buildOptionIndex(CONFIG_TABS)
    const port = index.find((entry) => entry.key === 'port')
    expect(port?.tabId).toBe('network')
    expect(port?.label).toBe('Port')
  })
})
