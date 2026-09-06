import { storage } from '@neutralinojs/lib'
import type { StateStorage } from 'zustand/middleware'
import { hasNeutralinoRuntime, initializeNeutralinoRuntime } from '../native/runtime'

interface NeutralinoError {
  code?: string
}

function isMissingStorageKey(error: unknown) {
  const code = (error as NeutralinoError)?.code
  return code === 'NE_ST_NOSTKEX' || code === 'NE_ST_STKEYRE'
}

export const settingsStorage: StateStorage<Promise<void> | void> = {
  async getItem(name) {
    if (!hasNeutralinoRuntime()) return localStorage.getItem(name)

    initializeNeutralinoRuntime()
    try {
      return await storage.getData(name)
    } catch (error) {
      if (!isMissingStorageKey(error)) throw error

      const legacySettings = localStorage.getItem(name)
      if (legacySettings !== null) await storage.setData(name, legacySettings)
      return legacySettings
    }
  },
  setItem(name, value) {
    if (!hasNeutralinoRuntime()) {
      localStorage.setItem(name, value)
      return
    }

    initializeNeutralinoRuntime()
    return storage.setData(name, value)
  },
  async removeItem(name) {
    if (!hasNeutralinoRuntime()) {
      localStorage.removeItem(name)
      return
    }

    initializeNeutralinoRuntime()
    await storage.removeData(name).catch((error: unknown) => {
      if (!isMissingStorageKey(error)) throw error
    })
    localStorage.removeItem(name)
  },
}
