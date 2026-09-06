/**
 * @file Vitest DOM test setup.
 */

import '@testing-library/jest-dom/vitest'

// jsdom does not implement localStorage; provide a minimal in-memory stub.
if (typeof window.localStorage === 'undefined') {
  const store = new Map<string, string>()
  const storage: Storage = {
    get length() {
      return store.size
    },
    clear: () => {
      store.clear()
    },
    getItem: (key) => (store.has(key) ? (store.get(key) ?? null) : null),
    key: (index) => [...store.keys()][index] ?? null,
    removeItem: (key) => {
      store.delete(key)
    },
    setItem: (key, value) => {
      store.set(key, value)
    },
  }
  Object.defineProperty(window, 'localStorage', { value: storage, configurable: true })
}
