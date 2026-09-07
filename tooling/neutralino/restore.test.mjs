import assert from 'node:assert/strict'
import {
  mkdirSync,
  mkdtempSync,
  readFileSync,
  realpathSync,
  rmSync,
  symlinkSync,
  writeFileSync,
} from 'node:fs'
import { tmpdir } from 'node:os'
import path from 'node:path'
import test from 'node:test'
import { restore } from './restore.mjs'

test('root install and explicit setup restore the same pinned runtime', () => {
  const root = JSON.parse(readFileSync(new URL('../../package.json', import.meta.url), 'utf8'))
  const terra = JSON.parse(
    readFileSync(new URL('../../apps/terra/package.json', import.meta.url), 'utf8'),
  )
  assert.equal(root.scripts.postinstall, 'node tooling/neutralino/restore.mjs')
  assert.equal(root.scripts['setup:terra'], root.scripts.postinstall)
  assert.equal(terra.scripts['neutralino:update'], 'node ../../tooling/neutralino/restore.mjs')
})

test('resolves the CLI through Terra workspace links with an isolated package store', () => {
  const root = mkdtempSync(path.join(tmpdir(), 'supernova-isolated-'))
  const app = path.join(root, 'apps/terra')
  const installed = path.join(root, 'node_modules/.store/neu/node_modules/@neutralinojs/neu')
  const link = path.join(app, 'node_modules/@neutralinojs/neu')
  try {
    mkdirSync(path.join(app, 'bin'), { recursive: true })
    mkdirSync(path.dirname(link), { recursive: true })
    mkdirSync(path.join(installed, 'bin'), { recursive: true })
    writeFileSync(
      path.join(app, 'package.json'),
      JSON.stringify({ name: 'terra-client', devDependencies: { '@neutralinojs/neu': '11.7.2' } }),
    )
    writeFileSync(
      path.join(app, 'neutralino.config.json'),
      JSON.stringify({ cli: { binaryVersion: '6.9.0', clientVersion: '6.9.0' } }),
    )
    writeFileSync(
      path.join(installed, 'package.json'),
      JSON.stringify({ name: '@neutralinojs/neu', version: '11.7.2' }),
    )
    writeFileSync(path.join(installed, 'bin/neu.js'), '// fixture only')
    symlinkSync(installed, link, process.platform === 'win32' ? 'junction' : 'dir')
    let called = false
    restore({
      root,
      platform: 'linux',
      arch: 'x64',
      run(command, args, options) {
        called = true
        assert.equal(command, process.execPath)
        assert.deepEqual(args, [realpathSync(path.join(installed, 'bin/neu.js')), 'update'])
        assert.equal(options.cwd, app)
        writeFileSync(path.join(app, 'bin/neutralino-linux_x64'), 'fixture')
        return { status: 0 }
      },
    })
    assert.equal(called, true)
  } finally {
    rmSync(root, { recursive: true, force: true })
  }
})

test('runtime restoration validates pins, invokes local CLI without latest, and propagates failures', () => {
  const root = mkdtempSync(path.join(tmpdir(), 'supernova-runtime-'))
  const app = path.join(root, 'apps/terra')
  const cli = path.join(root, 'node_modules/@neutralinojs/neu/bin/neu.js')
  const config = path.join(app, 'neutralino.config.json')
  mkdirSync(path.join(app, 'bin'), { recursive: true })
  mkdirSync(path.dirname(cli), { recursive: true })
  writeFileSync(cli, '// fixture only')
  const writeConfig = (binaryVersion) =>
    writeFileSync(config, JSON.stringify({ cli: { binaryVersion, clientVersion: '6.9.0' } }))
  try {
    writeConfig('6.9.0')
    for (const [platform, arch, binary] of [
      ['win32', 'x64', 'neutralino-win_x64.exe'],
      ['linux', 'x64', 'neutralino-linux_x64'],
      ['linux', 'arm64', 'neutralino-linux_arm64'],
    ]) {
      restore({
        root,
        platform,
        arch,
        run(command, args, options) {
          assert.equal(command, process.execPath)
          assert.deepEqual(args, [cli, 'update'])
          assert.equal(options.cwd, app)
          assert.equal(options.timeout, 120000)
          writeFileSync(path.join(app, 'bin', binary), 'fixture')
          return { status: 0 }
        },
      })
    }
    assert.throws(() => restore({ root, platform: 'darwin', arch: 'arm64' }), /Unsupported/)
    for (const version of ['latest', 'nightly', '', undefined]) {
      writeConfig(version)
      assert.throws(
        () =>
          restore({
            root,
            platform: 'win32',
            arch: 'x64',
            run() {
              assert.fail('Unpinned runtime must not be downloaded')
            },
          }),
        /exact release version/,
      )
    }
    writeConfig('6.9.0')
    assert.throws(
      () => restore({ root, platform: 'win32', arch: 'x64', run: () => ({ status: 1 }) }),
      /restore failed/,
    )
    rmSync(path.join(app, 'bin/neutralino-win_x64.exe'))
    assert.throws(
      () => restore({ root, platform: 'win32', arch: 'x64', run: () => ({ status: 0 }) }),
      /did not produce/,
    )
  } finally {
    rmSync(root, { recursive: true, force: true })
  }
})
