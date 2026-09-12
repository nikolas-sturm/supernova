import assert from 'node:assert/strict'
import { execFileSync, spawnSync } from 'node:child_process'
import { existsSync, readFileSync, realpathSync } from 'node:fs'
import test from 'node:test'
import { fileURLToPath } from 'node:url'

const root = fileURLToPath(new URL('../', import.meta.url))
const read = (file) =>
  readFileSync(new URL(`../${file}`, import.meta.url), 'utf8').replaceAll('\r\n', '\n')
const json = (file) => JSON.parse(read(file))
const git = (...args) => execFileSync('git', args, { cwd: root, encoding: 'utf8', timeout: 10000 })

test('Nx project listing exits without a persistent daemon', () => {
  // Check policy first: a regression must not launch another detached watcher.
  assert.equal(json('nx.json').useDaemonProcess, false)
  const env = { ...process.env }
  delete env.NX_DAEMON
  const result = spawnSync(
    process.execPath,
    ['node_modules/nx/dist/bin/nx.js', 'show', 'projects', '--json'],
    { cwd: root, env, encoding: 'utf8', timeout: 10000, windowsHide: true },
  )
  assert.equal(result.error, undefined)
  assert.equal(result.status, 0, result.stderr)
  const projects = JSON.parse(result.stdout)
  for (const project of ['sol', 'terra', 'design-system', 'native-tooling', 'frontend-tooling']) {
    assert.ok(projects.includes(project), `Missing Nx project: ${project}`)
  }
  assert.equal(projects.length, 5)
})

test('workspace uses one lockfile and one first-party language toolchain', () => {
  const manifest = json('package.json')
  const workflow = read('.github/workflows/workspace.yml')
  const setupNpm = `npm install --global ${manifest.packageManager}`
  assert.ok(workflow.includes(setupNpm), 'CI must install the pinned npm version')
  assert.ok(workflow.indexOf(setupNpm) < workflow.indexOf('npm ci'))
  for (const file of [
    'package.json',
    'apps/sol/project.json',
    'apps/sol/src_assets/common/assets/web/src/main.tsx',
    'apps/terra/src/App.tsx',
    'packages/design-system/src/themes.css',
    'tooling/workspace.test.mjs',
  ]) {
    assert.equal(git('check-attr', 'eol', '--', file).trim(), `${file}: eol: lf`)
  }
  assert.equal(manifest.scripts['dev:sol'], 'node tooling/dev.mjs sol')
  assert.equal(manifest.scripts['dev:terra'], 'node tooling/dev.mjs terra')
  assert.deepEqual(
    Object.keys(manifest.scripts)
      .filter((name) => name.startsWith('dev:'))
      .sort(),
    ['dev:sol', 'dev:terra'],
  )
  assert.equal(manifest.devDependencies.typescript, '7.0.2')
  assert.equal(manifest.devDependencies.nx, '23.2.0')
  assert.equal(read('.node-version').trim(), '26.8.1')
  const lock = json('package-lock.json').packages
  assert.deepEqual(
    Object.keys(lock)
      .filter((key) => /^apps\/[^/]+$/.test(key))
      .sort(),
    ['apps/sol', 'apps/terra'],
  )
  for (const app of ['sol', 'terra']) {
    const workspace = `apps/${app}`
    const name = app === 'sol' ? 'sol' : 'terra-client'
    assert.equal(json(`${workspace}/package.json`).name, name)
    assert.equal(lock[workspace].name, name)
    assert.deepEqual(lock[`node_modules/${name}`], { resolved: workspace, link: true })
    assert.equal(
      realpathSync(new URL(`../node_modules/${name}`, import.meta.url)),
      realpathSync(new URL(`../${workspace}`, import.meta.url)),
    )
    assert.ok(!existsSync(new URL(`../apps/${app}/package-lock.json`, import.meta.url)))
    assert.equal(json(`apps/${app}/package.json`).dependencies['@supernova/design-system'], '0.1.0')
    const native = app === 'terra' ? 'apps/terra/native' : 'apps/sol'
    assert.match(read(`${native}/CMakeLists.txt`), /set\(CMAKE_CXX_STANDARD 23\)/)
  }
  assert.equal(lock['node_modules/sunshine'], undefined)
  assert.equal(lock['node_modules/eclipse-client'], undefined)
})

test('native targets stay uncached with complete configuration chains', () => {
  for (const app of ['sol', 'terra']) {
    const { targets, implicitDependencies } = json(`apps/${app}/project.json`)
    assert.equal(json(`apps/${app}/project.json`).name, app)
    assert.ok(implicitDependencies.includes('native-tooling'))
    assert.ok(implicitDependencies.includes('frontend-tooling'))
    assert.ok(implicitDependencies.includes('design-system'))
    for (const operation of ['configure', 'build', 'test']) {
      const target = targets[`native:${operation}`]
      assert.equal(target.cache, false)
      assert.equal(target.defaultConfiguration, 'debug')
      assert.ok(target.configurations.debug)
      assert.ok(target.configurations.release.command.endsWith(`${operation} release`))
      assert.equal(
        target.configurations.release.command,
        `node tooling/native/build.mjs ${app} ${operation} release`,
      )
    }
    assert.deepEqual(targets['native:build'].dependsOn, ['native:configure'])
    assert.deepEqual(targets['native:test'].dependsOn, ['native:build'])
  }
})

test('Sol Git version definitions rebuild only version-reporting sources', () => {
  const version = read('apps/sol/cmake/prep/build_version.cmake')
  const target = read('apps/sol/cmake/targets/common.cmake')
  const tests = read('apps/sol/tests/CMakeLists.txt')
  assert.match(version, /set\(SOL_VERSION_DEFINITIONS/)
  assert.doesNotMatch(version, /SOL_DEFINITIONS PROJECT_VERSION=/)
  for (const source of ['confighttp.cpp', 'main.cpp', 'nvhttp.cpp']) {
    assert.match(target, new RegExp(`src/${source.replace('.', '\\.')}`))
  }
  assert.match(target, /PROPERTIES COMPILE_DEFINITIONS "\$\{SOL_VERSION_DEFINITIONS\}"/)
  assert.match(tests, /PROPERTIES COMPILE_DEFINITIONS "\$\{SOL_VERSION_DEFINITIONS\}"/)
})

test('Sol preserves Sunshine port defaults, offsets, and discovery mapping', () => {
  const upstream = '74273db90c7eb8ce6b6d07d009ffc4066f015611'
  const original = (file) => git('show', `${upstream}:src/${file}`)
  const basePattern = /(\d+),\s*\/\/ Base port number/
  const base = Number(read('apps/sol/src/config.cpp').match(basePattern)?.[1])
  assert.equal(base, 47989)
  assert.equal(base, Number(original('config.cpp').match(basePattern)?.[1]))
  for (const [file, symbol, port] of [
    ['nvhttp.h', 'PORT_HTTPS', 47984],
    ['nvhttp.h', 'PORT_HTTP', 47989],
    ['confighttp.h', 'PORT_HTTPS', 47990],
    ['rtsp.h', 'RTSP_SETUP_PORT', 48010],
    ['stream.h', 'VIDEO_STREAM_PORT', 47998],
    ['stream.h', 'CONTROL_PORT', 47999],
    ['stream.h', 'AUDIO_STREAM_PORT', 48000],
  ]) {
    const pattern = new RegExp(`constexpr auto ${symbol} = (-?\\d+);`)
    const current = read(`apps/sol/src/${file}`).match(pattern)?.[1]
    assert.notEqual(current, undefined)
    assert.equal(current, original(file).match(pattern)?.[1], `${symbol}: upstream offset changed`)
    assert.equal(base + Number(current), port)
  }
  for (const platform of ['windows', 'linux']) {
    assert.match(
      read(`apps/sol/src/platform/${platform}/publish.cpp`),
      /net::map_port\(nvhttp::PORT_HTTP\)/,
    )
  }
})

test('Terra declares the Windows 10 APIs used by its renderer', () => {
  const cmake = read('apps/terra/native/CMakeLists.txt')
  for (const macro of ['_WIN32_WINNT', 'WINVER']) {
    const value = cmake.match(new RegExp(`${macro}=(0x[0-9a-f]+)`, 'i'))?.[1]
    assert.ok(Number(value) >= 0x0a00, `${macro} must expose GetDpiForWindow`)
  }
})

test('subtree histories and original dependency pins remain intact', () => {
  // These imported assets live in a directory named build, but are not generated output.
  for (const asset of [
    'Info.plist.in',
    'dmg-finder-layout.applescript',
    'sunshine.icns',
    'sunshine-background-72dpi.jpg',
  ]) {
    const path = `apps/sol/src_assets/macos/build/${asset}`
    assert.ok(existsSync(new URL(`../${path}`, import.meta.url)), path)
    const ignored = spawnSync('git', ['check-ignore', '--no-index', '--', path], {
      cwd: root,
      encoding: 'utf8',
      timeout: 10000,
    })
    assert.equal(ignored.status, 1, `${path}: imported source must not be ignored`)
  }
  const sources = {
    sol: '74273db90c7eb8ce6b6d07d009ffc4066f015611',
    terra: '33332233b4a04d41a77d954b6479453e7eff216b',
  }
  const modules = read('.gitmodules')
  for (const [app, revision] of Object.entries(sources)) {
    git('merge-base', '--is-ancestor', revision, 'HEAD')
    const entries = git('ls-tree', '-r', revision)
      .split('\n')
      .filter((line) => line.startsWith('160000 '))
    const originalModules = git('show', `${revision}:.gitmodules`)
    for (const entry of entries) {
      const [metadata, path] = entry.split('\t')
      const pin = metadata.split(' ')[2]
      const section = modules
        .split('[submodule ')
        .find((value) => value.includes(`path = apps/${app}/${path}\n`))
      assert.ok(section, `${app}/${path}: unregistered submodule`)
      const imported = git('ls-files', '--stage', '--', `apps/${app}/${path}`)
      assert.equal(imported.split(' ')[1], pin, `${app}/${path}: changed dependency pin`)
      const worktree = fileURLToPath(new URL(`../apps/${app}/${path}/`, import.meta.url))
      if (existsSync(`${worktree}/.git`)) {
        assert.equal(
          git('-C', worktree, 'rev-parse', 'HEAD').trim(),
          pin,
          `${app}/${path}: worktree differs from dependency pin`,
        )
      }
      const originalSection = originalModules
        .split('[submodule ')
        .find((section) => section.includes(`path = ${path}\n`))
      const originalUrl = originalSection.match(/url = (.+)/)[1].trim()
      assert.equal(section.match(/url = (.+)/)[1].trim(), originalUrl)
    }
  }
})
