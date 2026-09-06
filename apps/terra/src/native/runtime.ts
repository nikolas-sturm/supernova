import { init } from '@neutralinojs/lib'

let initialized = false

export function hasNeutralinoRuntime() {
  return typeof window !== 'undefined' && 'NL_OS' in window
}

export function initializeNeutralinoRuntime() {
  if (!hasNeutralinoRuntime() || initialized) return
  init()
  initialized = true
}
