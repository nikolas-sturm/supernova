import assert from 'node:assert/strict'
import { mkdirSync, mkdtempSync, readFileSync, renameSync, rmSync, writeFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import path from 'node:path'
import test from 'node:test'
import { setTimeout as delay } from 'node:timers/promises'
import { createServer } from 'vite'
import { designSystemSource, nativeWatchIgnored } from './config.ts'

test('both apps resolve the design system without a workspace package link', () => {
  assert.equal(readFileSync(path.join(designSystemSource, 'styles.css'), 'utf8').length > 0, true)
  for (const app of ['sol', 'terra']) {
    const config = readFileSync(
      new URL(`../../apps/${app}/vite.config.ts`, import.meta.url),
      'utf8',
    )
    assert.match(config, /['"]@supernova\/design-system['"]:\s*designSystemSource/)
  }
})

test('both app configurations install the native-directory exclusion', () => {
  for (const app of ['sol', 'terra']) {
    const config = readFileSync(
      new URL(`../../apps/${app}/vite.config.ts`, import.meta.url),
      'utf8',
    )
    assert.match(config, /watch:\s*\{\s*ignored:\s*nativeWatchIgnored\s*\}/)
  }
})

test('watch exclusions cover native trees but retain frontend and shared source', () => {
  for (const app of ['sol', 'terra']) {
    for (const native of [
      'cmake-build-win32-debug',
      'cmake-build-linux-release',
      'third-party',
      'refs',
      'extensions',
    ]) {
      const directory = `D:/Projects/supernova/apps/${app}/${native}`
      for (const location of [directory, `${directory}/_deps/ex-json/json/include`]) {
        assert.ok(nativeWatchIgnored.test(location), location)
        assert.ok(nativeWatchIgnored.test(location.replaceAll('/', '\\')), location)
      }
    }
  }
  for (const location of [
    'D:/Projects/supernova/apps/terra/src/App.tsx',
    'D:/Projects/supernova/apps/terra/src/refs/state.ts',
    'D:/Projects/supernova/apps/sol/src_assets/common/assets/web/src/main.tsx',
    'D:/Projects/supernova/packages/design-system/src/themes.css',
  ]) {
    assert.equal(nativeWatchIgnored.test(location), false, location)
  }
})

test('Vite watches frontend source without locking CMake extraction directories', {
  timeout: 10000,
}, async () => {
  const temporary = mkdtempSync(path.join(tmpdir(), 'supernova-watch-'))
  const root = path.join(temporary, 'apps', 'terra')
  const source = path.join(root, 'src')
  const dependency = path.join(root, 'cmake-build-win32-debug', '_deps', 'ex-json', 'json')
  mkdirSync(source, { recursive: true })
  mkdirSync(path.join(dependency, 'include'), { recursive: true })
  writeFileSync(path.join(source, 'main.ts'), 'export const value = 1\n')
  writeFileSync(path.join(dependency, 'include', 'json.hpp'), '// fixture\n')
  let server
  try {
    // Middleware mode and ws:false create only an in-process watcher, no listener.
    server = await createServer({
      root,
      configFile: false,
      publicDir: false,
      logLevel: 'silent',
      optimizeDeps: { noDiscovery: true, include: [] },
      server: { middlewareMode: true, ws: false, watch: { ignored: nativeWatchIgnored } },
    })
    const normalize = (value) => value.replaceAll('\\', '/')
    for (let attempt = 0; attempt < 100; attempt++) {
      if (
        Object.keys(server.watcher.getWatched()).some(
          (directory) => normalize(directory) === normalize(source),
        )
      )
        break
      await delay(10)
    }
    const watched = Object.keys(server.watcher.getWatched()).map(normalize)
    assert.ok(watched.includes(normalize(source)), 'Frontend source must remain watched')
    assert.ok(!watched.some((directory) => directory.includes('/cmake-build-')))
    const destination = path.join(root, 'cmake-build-win32-debug', '_deps', 'json-src')
    assert.doesNotThrow(() => renameSync(dependency, destination))
  } finally {
    await server?.close()
    rmSync(temporary, { recursive: true, force: true })
  }
})
