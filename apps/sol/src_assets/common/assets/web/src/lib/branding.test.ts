/// <reference types="node" />

/**
 * @file Regression checks for Sol branding, packaged assets, and retained contracts.
 */

import { existsSync, readdirSync, readFileSync } from 'node:fs'
import { describe, expect, it } from 'vitest'
import en from '../../public/assets/locale/en.json'
import { CONFIG_TABS, populateConfigDraft, stripDefaultValues } from '../routes/config/defaults'

const sourceUrl = import.meta.url
const webRoot = new URL('../../', sourceUrl)
const appRoot = new URL('../../../../', webRoot)

describe('Sol branding', () => {
  it('uses Sol titles and existing renamed favicons in every HTML entry point', () => {
    const pages = readdirSync(webRoot).filter((name) => name.endsWith('.html'))
    expect(pages).toHaveLength(10)
    for (const page of pages) {
      const html = readFileSync(new URL(page, webRoot), 'utf8')
      expect(html).toMatch(/<title>[^<]*Sol[^<]*<\/title>/)
      expect(html).toContain('./images/sol.ico')
    }
    expect(existsSync(new URL('public/images/sol.ico', webRoot))).toBe(true)
  })

  it('updates English display strings without renaming translation or theme keys', () => {
    expect(en.welcome.greeting).toBe('Welcome to Sol!')
    expect(en.troubleshooting.restart_sunshine).toBe('Restart Sol')
    expect(en.config.sunshine_name).toBe('Sol Name')
    expect(en.navbar.theme_sunshine).toBe('Sunshine')
    expect(en.navbar.theme_moonlight).toBe('Moonlight')
    expect(en.pin.pair_success).toBe('Success! Please check Terra to continue')
  })

  it('round-trips existing configuration and saved launch-command contracts', () => {
    const saved = {
      sunshine_name: 'Living room',
      file_state: 'sunshine_state.json',
      global_prep_cmd: [{ do: 'echo $SUNSHINE_CLIENT_WIDTH', undo: '' }],
    }
    const payload = stripDefaultValues(populateConfigDraft(saved, CONFIG_TABS), CONFIG_TABS)
    expect(payload.sunshine_name).toBe(saved.sunshine_name)
    expect(payload.file_state).toBe(saved.file_state)
    expect(payload.global_prep_cmd).toEqual(saved.global_prep_cmd)
    expect(payload).not.toHaveProperty('sol_name')
  })

  it('keeps native build and installer references aligned with renamed source assets', () => {
    for (const file of [
      'sol.svg',
      'sol.ico',
      'sol.png',
      'tools/solsvc.cpp',
      'cmake/dependencies/Boost_Sol.cmake',
      'cmake/dependencies/libevdev_Sol.cmake',
      'cmake/packaging/wix_resources/sol-installer.wxs',
      'src_assets/windows/misc/sol-setup.ps1',
      'src_assets/linux/misc/60-sol.conf',
      'src_assets/linux/misc/60-sol.rules',
      'packaging/linux/dev.lizardbyte.app.Sol.desktop',
      'packaging/linux/app-dev.lizardbyte.app.Sol.service.in',
      'packaging/linux/flatpak/dev.lizardbyte.app.Sol.yml',
    ]) {
      expect(existsSync(new URL(file, appRoot)), file).toBe(true)
    }
    for (const file of [
      'cmake/packaging/linux.cmake',
      'packaging/linux/AppImage/AppRun',
      'packaging/linux/flatpak/scripts/additional-install.sh',
    ]) {
      const source = readFileSync(new URL(file, appRoot), 'utf8')
      expect(source).toContain('60-sol.conf')
      expect(source).not.toContain('60-sunshine.conf')
    }
    const targets = readFileSync(new URL('cmake/targets/common.cmake', appRoot), 'utf8')
    expect(targets).toContain('add_executable(sol ')
    expect(targets).toContain('SOL_SOURCE_ASSETS_DIR=')
    expect(readFileSync(new URL('tools/solsvc.cpp', appRoot), 'utf8')).toContain('"SolService"')
    expect(readFileSync(new URL('tests/CMakeLists.txt', appRoot), 'utf8')).toContain('test_sol')
  })

  it('preserves protocol and state names while using renamed native identifiers', () => {
    const config = readFileSync(new URL('src/config.cpp', appRoot), 'utf8')
    expect(config).toContain('string_f(vars, "sunshine_name", nvhttp.sol_name)')
    expect(config).toContain('"sunshine_state.json"')
    expect(config).toContain('"/sunshine.conf"')
    expect(config).not.toContain('sunshine.config_file')
    const server = readFileSync(new URL('src/nvhttp.cpp', appRoot), 'utf8')
    expect(server).toContain('"SUNSHINE_SERVER_BUSY" : "SUNSHINE_SERVER_FREE"')
    expect(server).toContain('"sunshineCpuPercent"')
    expect(server).toContain('"sunshineMemoryBytes"')
    expect(server).not.toContain('config::sunshine')
  })

  it('preserves the Windows upgrade install root and installer identity', () => {
    const packaging = ['windows.cmake', 'windows_wix.cmake', 'windows_nsis.cmake']
      .map((file) => readFileSync(new URL(`cmake/packaging/${file}`, appRoot), 'utf8'))
      .join('\n')
    expect(packaging.match(/set\(CPACK_PACKAGE_INSTALL_DIRECTORY\s+[^)]+\)/gi)).toEqual([
      'set(CPACK_PACKAGE_INSTALL_DIRECTORY "Sunshine")',
    ])
    expect(packaging).toContain('CPACK_WIX_UPGRADE_GUID "512A3D1B-BE16-401B-A0D1-59BBA3942FB8"')
    const migration = readFileSync(
      new URL('src_assets/windows/misc/migration/migrate-config.bat', appRoot),
      'utf8',
    )
    expect(migration).toContain('for %%I in ("%~dp0\\..") do set "OLD_DIR=%%~fI"')
    for (const name of ['sunshine.conf', 'sunshine_state.json', 'credentials']) {
      expect(migration).toContain(`%OLD_DIR%\\${name}`)
      expect(migration).toContain(`%NEW_DIR%\\${name}`)
    }
  })

  it('uses Terra private native filenames, identifiers, includes, and CMake references', () => {
    for (const directory of ['src/', 'tests/']) {
      const root = new URL(directory, appRoot)
      const files = readdirSync(root, { recursive: true, encoding: 'utf8' })
        .map((file) => file.replaceAll('\\', '/'))
        .filter((file) => /\.(h|cpp)$/.test(file) && !/(^|\/)(macos|freebsd)\//.test(file))
      for (const file of files) {
        expect(file).not.toMatch(/eclipse/i)
        const url = new URL(file, root)
        const source = readFileSync(url, 'utf8')
        // Ignore comments and C++ literals: compatibility names belong there, never in identifiers.
        const identifiers = source.replace(
          /\/\*[\s\S]*?\*\/|\/\/[^\n]*|R"([^ ()\\\t\r\n]{0,16})\([\s\S]*?\)\1"|"(?:[^"\\]|\\.)*"|'(?:[^'\\]|\\.)*'/g,
          '',
        )
        expect(identifiers, `${directory}${file}`).not.toMatch(/eclipse/i)
        for (const match of source.matchAll(/#include\s*[<"]([^>"\n]*terra[^>"\n]*)[>"]/g)) {
          const include = match[1]
          if (!include) throw new Error('Missing include path')
          expect(
            existsSync(new URL(include, include.startsWith('src/') ? appRoot : url)),
            include,
          ).toBe(true)
        }
      }
    }
    let sources = 0
    for (const file of ['common.cmake', 'windows.cmake']) {
      const cmake = readFileSync(new URL(`cmake/compile_definitions/${file}`, appRoot), 'utf8')
      expect(cmake).not.toMatch(/eclipse/i)
      for (const match of cmake.matchAll(/\$\{CMAKE_SOURCE_DIR\}\/(src\/[^"\n]*terra[^"\n]*)/g)) {
        const path = match[1]
        if (!path) throw new Error('Missing CMake source path')
        expect(existsSync(new URL(path, appRoot)), path).toBe(true)
        sources++
      }
    }
    expect(sources).toBe(23)
  })

  it('retains Terra wire and persisted resource contracts after the private rename', () => {
    const server = readFileSync(new URL('src/nvhttp.cpp', appRoot), 'utf8')
    for (const value of [
      '^/eclipse/v1/capabilities$',
      'X-Eclipse-Claim-Token',
      'root.EclipseApiVersion',
      'root.EclipseCapabilities',
      'root.EclipseApiPort',
      'root.EclipseSessionId',
      'eclipseApiVersion',
      'eclipseAppUuid',
      'eclipseScopes',
      'eclipseInput',
      'eclipsePlatform',
      'eclipseWorkspaceId',
      'eclipseDisplayProfileId',
      'eclipseStreamProfileId',
      'eclipseLaunchProfileId',
      'eclipseSandboxProfileId',
      'eclipse_permissions.version',
      'eclipse_permissions.scopes',
      'eclipse_permissions.allowed_apps',
      'eclipse_permissions.expires_at',
      'eclipse_operations.json',
      'eclipse_sandboxes.json',
      'eclipse_profiles.json',
      'eclipse_workspaces.json',
      'eclipse_virtual_displays.json',
      'eclipse-display-topology',
    ]) {
      expect(server).toContain(`"${value}"`)
    }
    expect(server).toContain('std::char_traits<char>::length("eclipse")')
    expect(server).toContain('{"source", "eclipse"}')
    expect(server).toContain('terra_api::API_VERSION')
    expect(server).toContain('requested_terra_permissions')
    const process = readFileSync(new URL('src/process.cpp', appRoot), 'utf8')
    expect(process).toContain('source_app["x-eclipse"]')
    expect(process).toContain('normalize_terra_metadata(terra)')
  })
})
