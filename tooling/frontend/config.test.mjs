import assert from 'node:assert/strict'
import { mkdirSync, mkdtempSync, readFileSync, renameSync, rmSync, writeFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import path from 'node:path'
import test from 'node:test'
import { setTimeout as delay } from 'node:timers/promises'
import { fileURLToPath } from 'node:url'
import { createServer, loadConfigFromFile } from 'vite'
import { runSession } from '../dev.mjs'
import { designSystemSource, nativeWatchIgnored } from './config.ts'

test('cacheable Sol web configuration ignores external paths and disables bundle upload', async () => {
  const values = {
    SUPERNOVA_WEB_BUILD: '1',
    SOL_ASSETS_DIR: '/missing-output',
    SOL_SOURCE_ASSETS_DIR: '/missing-input',
    SOL_BUILD_HOMEBREW: '1',
  }
  const previous = Object.fromEntries(Object.keys(values).map((key) => [key, process.env[key]]))
  try {
    Object.assign(process.env, values)
    const result = await loadConfigFromFile(
      { command: 'build', mode: 'production' },
      fileURLToPath(new URL('../../apps/sol/vite.config.ts', import.meta.url)),
    )
    assert.ok(result.config.build.outDir.endsWith(path.join('build', 'assets', 'web')))
    assert.ok(result.config.root.endsWith(path.join('src_assets', 'common', 'assets', 'web')))
    assert.equal(result.config.build.emptyOutDir, true)
    assert.equal(result.config.plugins.at(-1), false)
  } finally {
    for (const [key, value] of Object.entries(previous)) {
      if (value === undefined) delete process.env[key]
      else process.env[key] = value
    }
  }
})

test('real frontend watch build reports ready only after emitting initial assets', {
  timeout: 20000,
}, async () => {
  const temporary = mkdtempSync(path.join(tmpdir(), 'supernova-build-watch-'))
  try {
    writeFileSync(path.join(temporary, 'index.html'), '<html><body>fixture</body></html>')
    await runSession({
      timeout: 10000,
      services: [
        {
          name: 'watch fixture',
          command: process.execPath,
          args: [fileURLToPath(new URL('./sol-watch.mjs', import.meta.url))],
          cwd: temporary,
          readyMessage: true,
        },
        {
          name: 'verify output',
          command: process.execPath,
          args: [
            '-e',
            `process.exit(require('node:fs').readFileSync(${JSON.stringify(path.join(temporary, 'dist/index.html'))}, 'utf8').includes('fixture') ? 0 : 1)`,
          ],
          exitTogether: true,
        },
      ],
    })
  } finally {
    rmSync(temporary, { recursive: true, force: true })
  }
})

test('initial frontend compilation errors abort startup instead of accepting stale assets', {
  timeout: 20000,
}, async () => {
  const temporary = mkdtempSync(path.join(tmpdir(), 'supernova-watch-error-'))
  try {
    writeFileSync(
      path.join(temporary, 'index.html'),
      '<script type="module" src="./missing.js"></script>',
    )
    mkdirSync(path.join(temporary, 'dist'))
    writeFileSync(path.join(temporary, 'dist/index.html'), 'stale output')
    await assert.rejects(
      runSession({
        timeout: 10000,
        services: [
          {
            name: 'broken watcher',
            command: process.execPath,
            args: [fileURLToPath(new URL('./sol-watch.mjs', import.meta.url))],
            cwd: temporary,
            readyMessage: true,
          },
          { name: 'must not start', command: process.execPath, args: ['-e', 'process.exit(99)'] },
        ],
      }),
      /broken watcher exited \(1\)/,
    )
  } finally {
    rmSync(temporary, { recursive: true, force: true })
  }
})

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
