import assert from 'node:assert/strict'
import { execFileSync } from 'node:child_process'
import { existsSync, readFileSync } from 'node:fs'
import test from 'node:test'
import { fileURLToPath } from 'node:url'

const root = fileURLToPath(new URL('../', import.meta.url))
const read = (file) =>
  readFileSync(new URL(`../${file}`, import.meta.url), 'utf8').replaceAll('\r\n', '\n')
const json = (file) => JSON.parse(read(file))
const git = (...args) => execFileSync('git', args, { cwd: root, encoding: 'utf8', timeout: 10000 })

test('workspace uses one lockfile and one first-party language toolchain', () => {
  const manifest = json('package.json')
  assert.equal(manifest.devDependencies.typescript, '7.0.2')
  assert.equal(manifest.devDependencies.nx, '23.2.0')
  assert.equal(read('.node-version').trim(), '26.8.1')
  for (const app of ['progenitor', 'terra']) {
    assert.ok(!existsSync(new URL(`../apps/${app}/package-lock.json`, import.meta.url)))
    assert.equal(json(`apps/${app}/package.json`).dependencies['@supernova/design-system'], '0.1.0')
    const native = app === 'terra' ? 'apps/terra/native' : 'apps/progenitor'
    assert.match(read(`${native}/CMakeLists.txt`), /set\(CMAKE_CXX_STANDARD 23\)/)
  }
})

test('native targets stay uncached with complete configuration chains', () => {
  for (const app of ['progenitor', 'terra']) {
    const { targets, implicitDependencies } = json(`apps/${app}/project.json`)
    assert.ok(implicitDependencies.includes('native-tooling'))
    assert.ok(implicitDependencies.includes('design-system'))
    for (const operation of ['configure', 'build', 'test']) {
      const target = targets[`native:${operation}`]
      assert.equal(target.cache, false)
      assert.equal(target.defaultConfiguration, 'debug')
      assert.ok(target.configurations.debug)
      assert.ok(target.configurations.release.command.endsWith(`${operation} release`))
    }
    assert.deepEqual(targets['native:build'].dependsOn, ['native:configure'])
    assert.deepEqual(targets['native:test'].dependsOn, ['native:build'])
  }
})

test('subtree histories and original dependency pins remain intact', () => {
  const sources = {
    progenitor: '74273db90c7eb8ce6b6d07d009ffc4066f015611',
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
      const imported = git('ls-files', '--stage', '--', `apps/${app}/${path}`)
      assert.equal(imported.split(' ')[1], pin, `${app}/${path}: changed dependency pin`)
      assert.ok(
        modules.includes(`path = apps/${app}/${path}\n`),
        `${app}/${path}: unregistered submodule`,
      )
      const originalSection = originalModules
        .split('[submodule ')
        .find((section) => section.includes(`path = ${path}\n`))
      const originalUrl = originalSection.match(/url = (.+)/)[1].trim()
      const section = modules
        .split('[submodule ')
        .find((value) => value.includes(`path = apps/${app}/${path}\n`))
      assert.equal(section.match(/url = (.+)/)[1].trim(), originalUrl)
    }
  }
})
