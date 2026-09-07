import { beforeEach, describe, expect, it } from 'vitest'
import { defaultSettings, defaultSettingsByMode } from '../settings'
import { useClientStore } from './clientStore'

describe('clientStore mode profiles', () => {
  beforeEach(() => {
    localStorage.clear()
    useClientStore.setState({
      appMode: 'gaming',
      settingsByMode: {
        gaming: { ...defaultSettingsByMode.gaming },
        workstation: { ...defaultSettingsByMode.workstation },
      },
    })
  })

  it('starts pairing with Sol instructions and preserves the PIN', () => {
    useClientStore.getState().startPairing('host-1', '0427')
    expect(useClientStore.getState().pairing).toEqual({
      hostId: 'host-1',
      pin: '0427',
      state: 'pairing',
      message: "Enter this PIN in Sol's web interface.",
    })
    useClientStore.getState().clearPairing()
  })

  it('updates and resets only the active mode profile', () => {
    useClientStore.getState().updateSettings({ fps: 120 })
    useClientStore.getState().setAppMode('workstation')
    useClientStore.getState().updateSettings({ fps: 75, bitrateKbps: 25_000 })

    expect(useClientStore.getState().settingsByMode.gaming.fps).toBe(120)
    expect(useClientStore.getState().settingsByMode.workstation).toMatchObject({
      fps: 75,
      bitrateKbps: 25_000,
    })

    useClientStore.getState().resetSettings()

    expect(useClientStore.getState().settingsByMode.gaming.fps).toBe(120)
    expect(useClientStore.getState().settingsByMode.workstation).toEqual(
      defaultSettingsByMode.workstation,
    )
  })

  it('migrates the previous settings object into the Gaming profile', async () => {
    localStorage.setItem(
      'eclipse-client-settings',
      JSON.stringify({
        version: 0,
        state: {
          settings: {
            ...defaultSettings,
            width: 3440,
            height: 1440,
            fps: 90,
          },
        },
      }),
    )

    await useClientStore.persist.rehydrate()

    expect(useClientStore.getState().appMode).toBe('gaming')
    expect(useClientStore.getState().settingsByMode.gaming).toMatchObject({
      width: 3440,
      height: 1440,
      fps: 90,
    })
    expect(useClientStore.getState().settingsByMode.workstation).toEqual(
      defaultSettingsByMode.workstation,
    )
  })
})
