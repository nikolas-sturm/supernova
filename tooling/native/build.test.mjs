import assert from 'node:assert/strict'
import { spawnSync } from 'node:child_process'
import { existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import path from 'node:path'
import test from 'node:test'
import { fileURLToPath } from 'node:url'
import { commandsFor, parseArgs, shellQuote } from './build.mjs'

test('accepts every documented app, operation and configuration', () => {
  for (const app of ['progenitor', 'terra']) {
    for (const operation of ['configure', 'build', 'test']) {
      for (const config of ['debug', 'release']) {
        assert.deepEqual(parseArgs([app, operation, config]), { app, operation, config })
      }
    }
  }
})

test('rejects missing, extra, unknown and shell-injection arguments', () => {
  for (const args of [
    [],
    ['terra'],
    ['terra', 'build'],
    ['other', 'build', 'debug'],
    ['terra', 'clean', 'debug'],
    ['terra', 'build', 'Debug'],
    ['terra', 'build', 'debug', '--unknown'],
    ['terra; echo bad', 'build', 'debug'],
  ]) {
    assert.throws(() => parseArgs(args), /Usage:/)
  }
})

test('CLI validation fails before native tools run', () => {
  const result = spawnSync(
    process.execPath,
    [fileURLToPath(new URL('./build.mjs', import.meta.url)), 'terra', 'build', 'bad'],
    { encoding: 'utf8', env: { ...process.env, PATH: '' } },
  )
  assert.equal(result.status, 1)
  assert.match(result.stderr, /Usage:/)
})

test('configure preserves upstream roots and isolates configurations', () => {
  for (const app of ['terra', 'progenitor']) {
    const debug = commandsFor({ app, operation: 'configure', config: 'debug' }, 'linux')[0]
    const release = commandsFor({ app, operation: 'configure', config: 'release' }, 'linux')[0]
    assert.ok(debug[2].endsWith(path.join('apps', app, ...(app === 'terra' ? ['native'] : []))))
    assert.ok(debug[4].endsWith(path.join('apps', app, 'cmake-build-linux-debug')))
    assert.ok(release[4].endsWith(path.join('apps', app, 'cmake-build-linux-release')))
    const windows = commandsFor({ app, operation: 'configure', config: 'debug' }, 'win32')[0]
    assert.ok(windows[4].endsWith(path.join('apps', app, 'cmake-build-win32-debug')))
    assert.notEqual(windows[4], debug[4])
    assert.ok(debug.includes('-DCMAKE_BUILD_TYPE=Debug'))
    assert.ok(release.includes('-DCMAKE_BUILD_TYPE=Release'))
    if (app === 'progenitor') assert.ok(debug.includes('-DSUNSHINE_BUILD_WEB_UI=OFF'))
  }
})

test('Windows explicitly selects GCC and Terra stages only after build', () => {
  const configure = commandsFor(
    { app: 'terra', operation: 'configure', config: 'debug' },
    'win32',
  )[0]
  assert.ok(configure.includes('-DCMAKE_CXX_COMPILER=g++'))
  const commands = commandsFor({ app: 'terra', operation: 'build', config: 'debug' })
  assert.equal(commands.length, 2)
  assert.equal(commands[0].at(-1), '--parallel')
  assert.deepEqual(commands[1].slice(-2), ['--target', 'stage-extension'])
  assert.equal(commandsFor({ app: 'progenitor', operation: 'build', config: 'debug' }).length, 1)
})

test('test uses Progenitor executable and Terra CTest with no-tests failure', () => {
  const options = { app: 'progenitor', operation: 'test', config: 'debug' }
  assert.ok(commandsFor(options, 'win32')[0][0].endsWith(path.join('tests', 'test_sunshine.exe')))
  assert.ok(commandsFor(options, 'linux')[0][0].endsWith(path.join('tests', 'test_sunshine')))
  assert.ok(commandsFor({ ...options, app: 'terra' })[0].includes('--no-tests=error'))
})

test('shell quoting preserves spaces, quotes and metacharacters as data', () => {
  assert.equal(shellQuote('D:/Two  Words/$folder'), "'D:/Two  Words/$folder'")
  assert.equal(shellQuote("it's"), "'it'\\''s'")
})

const cmake =
  process.platform === 'win32'
    ? path.join(
        path.dirname(process.env.SUPERNOVA_MSYS2_SHELL || 'C:\\msys64\\msys2_shell.cmd'),
        'ucrt64',
        'bin',
        'cmake.exe',
      )
    : 'cmake'
const hasCmake = spawnSync(cmake, ['--version']).status === 0

test('Terra staging replaces generated runtime files and preserves unrelated files', {
  skip: !hasCmake,
}, () => {
  const temporary = mkdtempSync(path.join(tmpdir(), 'supernova-native-'))
  try {
    const source = path.join(temporary, "Debug output's files")
    const destination = path.join(temporary, 'extension', 'bin')
    mkdirSync(source)
    mkdirSync(destination, { recursive: true })
    writeFileSync(path.join(source, 'eclipse-core.exe'), 'debug')
    writeFileSync(path.join(source, 'current.dll'), 'current')
    writeFileSync(path.join(destination, 'obsolete.dll'), 'obsolete')
    writeFileSync(path.join(destination, 'gamecontrollerdb.txt'), 'obsolete')
    writeFileSync(path.join(destination, 'eclipse-core.exe'), 'release')
    writeFileSync(path.join(destination, 'keep.txt'), 'keep')
    const result = spawnSync(
      cmake,
      [
        `-DECLIPSE_STAGE_SOURCE=${source}`,
        `-DECLIPSE_STAGE_DESTINATION=${destination}`,
        '-P',
        fileURLToPath(
          new URL('../../apps/terra/native/cmake/StageExtension.cmake', import.meta.url),
        ),
      ],
      { encoding: 'utf8' },
    )
    assert.equal(result.status, 0, result.stderr)
    assert.equal(readFileSync(path.join(destination, 'eclipse-core.exe'), 'utf8'), 'debug')
    assert.equal(readFileSync(path.join(destination, 'keep.txt'), 'utf8'), 'keep')
    assert.ok(existsSync(path.join(destination, 'current.dll')))
    assert.ok(!existsSync(path.join(destination, 'obsolete.dll')))
    assert.ok(!existsSync(path.join(destination, 'gamecontrollerdb.txt')))
    rmSync(path.join(source, 'eclipse-core.exe'))
    const missing = spawnSync(
      cmake,
      [
        `-DECLIPSE_STAGE_SOURCE=${source}`,
        `-DECLIPSE_STAGE_DESTINATION=${destination}`,
        '-P',
        fileURLToPath(
          new URL('../../apps/terra/native/cmake/StageExtension.cmake', import.meta.url),
        ),
      ],
      { encoding: 'utf8' },
    )
    assert.notEqual(missing.status, 0)
    assert.match(missing.stderr, /Extension output missing/)
    assert.equal(readFileSync(path.join(destination, 'eclipse-core.exe'), 'utf8'), 'debug')
  } finally {
    rmSync(temporary, { recursive: true, force: true })
  }
})

test('Windows fails clearly for an explicitly configured missing MSYS2 shell', {
  skip: process.platform !== 'win32',
}, () => {
  const result = spawnSync(
    process.execPath,
    [fileURLToPath(new URL('./build.mjs', import.meta.url)), 'terra', 'configure', 'debug'],
    {
      encoding: 'utf8',
      env: {
        ...process.env,
        SUPERNOVA_MSYS2_SHELL: 'Z:\\missing-supernova-msys2\\msys2_shell.cmd',
      },
    },
  )
  assert.equal(result.status, 1)
  assert.match(result.stderr, /MSYS2 shell missing or invalid/)
})
