import { beforeEach, describe, expect, it, vi } from 'vitest'

const neutralino = vi.hoisted(() => {
  const data = new Map<string, string>()
  return {
    data,
    getData: vi.fn((key: string) =>
      data.has(key)
        ? Promise.resolve(data.get(key) as string)
        : Promise.reject({ code: 'NE_ST_NOSTKEX' }),
    ),
    setData: vi.fn((key: string, value: string) => {
      data.set(key, value)
      return Promise.resolve()
    }),
    removeData: vi.fn((key: string) => {
      if (!data.delete(key)) return Promise.reject({ code: 'NE_ST_STKEYRE' })
      return Promise.resolve()
    }),
    init: vi.fn(),
  }
})

vi.mock('@neutralinojs/lib', () => ({
  init: neutralino.init,
  storage: {
    getData: neutralino.getData,
    setData: neutralino.setData,
    removeData: neutralino.removeData,
  },
}))

import { settingsStorage } from './settingsStorage'

describe('settingsStorage', () => {
  beforeEach(() => {
    localStorage.clear()
    neutralino.data.clear()
    neutralino.getData.mockClear()
    neutralino.setData.mockClear()
    neutralino.removeData.mockClear()
    neutralino.init.mockClear()
    Object.defineProperty(window, 'NL_OS', { configurable: true, value: 'Linux' })
  })

  it('persists settings through Neutralino storage', async () => {
    await settingsStorage.setItem('eclipse-client-settings', '{"fps":120}')

    expect(await settingsStorage.getItem('eclipse-client-settings')).toBe('{"fps":120}')
    expect(localStorage.getItem('eclipse-client-settings')).toBeNull()
  })

  it('migrates settings previously stored in localStorage', async () => {
    localStorage.setItem('eclipse-client-settings', '{"fps":90}')

    expect(await settingsStorage.getItem('eclipse-client-settings')).toBe('{"fps":90}')
    expect(neutralino.data.get('eclipse-client-settings')).toBe('{"fps":90}')
  })
})
