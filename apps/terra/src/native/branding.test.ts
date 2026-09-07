import { describe, expect, it } from 'vitest'
import cmake from '../../native/CMakeLists.txt?raw'
import controlPlane from '../../native/src/control_plane.cpp?raw'
import gameStream from '../../native/src/gamestream_client.cpp?raw'
import core from '../../native/src/main.cpp?raw'
import streamSession from '../../native/src/stream_session.cpp?raw'
import config from '../../neutralino.config.json'
import appStyles from '../App.module.css?raw'
import bridge from './coreBridge.ts?raw'

describe('Terra rename contracts', () => {
  it('names Sol in first-party host diagnostics without renaming upstream transport', () => {
    for (const source of [core, controlPlane, gameStream, streamSession]) {
      expect(source).not.toContain('Sunshine')
    }
    expect(gameStream).toContain('Enter Sol HTTP address, not an HTTPS URL.')
    expect(streamSession).toContain('verify Sol firewall')
    expect(streamSession).toContain('Starting Moonlight transport.')
  })
  it('excludes shared Terra buttons from app-local button resets', () => {
    expect(appStyles.match(/button:not\(\[data-terra-button\]\)/g)).toHaveLength(2)
    expect(appStyles).not.toContain('data-eclipse-button')
    expect(core).toContain("Enter the displayed PIN in Sol's web interface.")
  })

  it('keeps shell, native target, and bridge names synchronized', () => {
    expect(config.applicationName).toBe('Terra')
    expect(config.modes.window.title).toBe('Terra')
    expect(config.cli.binaryName).toBe('terra')
    expect(config.extensions[0]?.id).toBe('dev.terra.core')
    expect(bridge).toContain("const extensionId = 'dev.terra.core'")
    expect(cmake).toContain('project(terra-client LANGUAGES C CXX)')
    expect(cmake).toContain(`\${TERRA_ROOT}/extensions/terra-core/bin`)
    for (const command of [
      config.extensions[0]?.commandLinux,
      config.extensions[0]?.commandWindows,
    ]) {
      expect(command?.replaceAll('\\', '/')).toContain('/extensions/terra-core/bin/terra-core')
    }
    const events = [...bridge.matchAll(/const \w+Event = '(terra\.[^']+)'/g)]
    expect(events).toHaveLength(9)
    for (const [, event] of events) expect(core).toContain(`"${event}"`)
  })

  it('retains persisted shell and identity locations rather than silently resetting users', () => {
    expect(config.applicationId).toBe('dev.eclipse.client')
    expect(config.extensions[0]?.commandLinux).toContain(`\${NL_OSDATAPATH}/Eclipse`)
    expect(config.extensions[0]?.commandWindows).toContain(`\${NL_OSDATAPATH}\\Eclipse`)
    expect(core).toContain('".eclipse-data"')
  })
})
