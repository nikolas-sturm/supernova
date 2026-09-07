import assert from 'node:assert/strict'
import { createSocket } from 'node:dgram'
import { existsSync, readFileSync } from 'node:fs'
import net from 'node:net'
import test from 'node:test'
import { developmentPlan, main, runSession } from './dev.mjs'
import { commandsFor, parseArgs } from './native/build.mjs'

const node = (name, code, extra = {}) => ({
  name,
  command: process.execPath,
  args: ['-e', code],
  ...extra,
})
const listen = (server) =>
  new Promise((resolve) => server.listen(0, '127.0.0.1', () => resolve(server.address().port)))
const close = (server) => new Promise((resolve) => server.close(resolve))

test('occupied UDP streaming ports are rejected without stopping their owner', async () => {
  const socket = createSocket('udp4')
  await new Promise((resolve) => socket.bind(0, '127.0.0.1', resolve))
  const port = socket.address().port
  try {
    await assert.rejects(runSession({ udpPorts: [port], services: [] }), /UDP port .* occupied/)
    assert.equal(socket.address().port, port)
  } finally {
    await new Promise((resolve) => socket.close(resolve))
  }
})

test('per-app commands build and run only their own native application', () => {
  const sol = developmentPlan('win32', 'x64', 'sol')
  const terra = developmentPlan('win32', 'x64', 'terra')
  assert.deepEqual(
    sol.services.map(({ name }) => name),
    ['Sol backend', 'Sol frontend'],
  )
  assert.deepEqual(
    terra.services.map(({ name }) => name),
    ['Terra desktop'],
  )
  assert.ok(sol.prepare.every(({ name }) => name.toLowerCase().startsWith('sol')))
  assert.ok(terra.prepare.every(({ name }) => name.toLowerCase().startsWith('terra')))
  assert.ok(!sol.required.some(([file]) => file === sol.shell))
  assert.ok(terra.required.some(([file]) => file === terra.shell))
  assert.deepEqual(terra.ports, [])
  assert.deepEqual(terra.udpPorts, [])
  assert.equal(sol.services[0].exitTogether, true)
  assert.ok(!terra.services[0].args.some((arg) => arg.startsWith('--url=')))
  assert.throws(() => developmentPlan('win32', 'x64', 'unknown'), /Usage/)
})

test('development plan builds serially and launches native desktop after both frontends', () => {
  for (const platform of ['win32', 'linux']) {
    const plan = developmentPlan(platform, 'x64')
    assert.deepEqual(
      plan.prepare.map(({ name }) => name),
      [
        'sol configure',
        'sol build',
        'terra configure',
        'terra build',
        'Sol web assets',
        'Terra web assets',
      ],
    )
    assert.deepEqual(
      plan.services.map(({ name }) => name),
      ['Sol backend', 'Sol frontend', 'Terra desktop'],
    )
    assert.ok(plan.build.endsWith(`cmake-build-${platform}-dev-debug`))
    assert.ok(plan.services[2].args.includes('--window-exit-process-on-close=true'))
    assert.ok(!plan.services[2].args.some((arg) => arg.startsWith('--url=')))
    assert.ok(!plan.services[2].args.includes('--export-auth-info'))
    assert.deepEqual(plan.services[0].args, ['port=47989'])
    assert.deepEqual(plan.services[0].ports, [47989, 47990])
    assert.deepEqual(plan.services[1].args.slice(1), ['build', '--watch'])
    assert.equal(plan.services[1].env.SOL_ASSETS_DIR, plan.build)
    assert.ok(!plan.ports.includes(5173))
    assert.deepEqual(plan.ports, [47984, 47989, 47990, 48010])
    assert.deepEqual(plan.udpPorts, [47998, 47999, 48000])
    if (platform === 'win32') assert.match(plan.services[0].env.PATH, /ucrt64/)
    else assert.ok(plan.services[0].env.XDG_CONFIG_HOME.startsWith(plan.build))
  }
  assert.throws(() => developmentPlan('darwin', 'arm64'), /Unsupported/)
  assert.throws(() => developmentPlan('win32', 'arm64'), /Unsupported/)
})

test('Sol dev builds use isolated trees and runtime assets without changing normal builds', () => {
  const options = parseArgs(['sol', 'configure', 'debug', '--dev'])
  assert.equal(options.dev, true)
  for (const platform of ['win32', 'linux']) {
    const command = commandsFor(options, platform)
    assert.ok(command[0].some((arg) => arg.includes(`cmake-build-${platform}-dev-debug`)))
    assert.ok(
      command[0].some((arg) => arg.startsWith('-DSOL_ASSETS_DIR_DEF=') && arg.endsWith('/assets')),
    )
    assert.ok(
      !commandsFor({ ...options, dev: false }, platform)[0].some((arg) =>
        arg.startsWith('-DSOL_ASSETS_DIR_DEF='),
      ),
    )
    assert.ok(
      commandsFor({ ...options, operation: 'test' }, platform)[0][0].includes(
        `cmake-build-${platform}-dev-debug`,
      ),
    )
  }
  assert.throws(() => parseArgs(['terra', 'build', 'debug', '--dev']), /Usage/)
  assert.throws(() => parseArgs(['sol', 'build', 'release', '--dev']), /Usage/)
})

test('root command and shell config support direct desktop launch', () => {
  const json = (path) => JSON.parse(readFileSync(new URL(`../${path}`, import.meta.url), 'utf8'))
  assert.equal(json('package.json').scripts.dev, 'node tooling/dev.mjs')
  assert.equal(json('apps/terra/neutralino.config.json').modes.window.injectGlobals, true)
})

test('missing prerequisites and invalid arguments fail before any process starts', async () => {
  await assert.rejects(main(['--unknown']), /Usage/)
  if (!existsSync(developmentPlan().shell))
    await assert.rejects(main([]), /Neutralino shell version pinned/)
})

test('occupied ports are rejected without terminating their owner', async () => {
  const server = net.createServer()
  const port = await listen(server)
  try {
    await assert.rejects(runSession({ ports: [port], services: [] }), /occupied/)
    assert.equal(server.listening, true)
  } finally {
    await close(server)
  }
})

test('preparation failures stop startup and propagate nonzero status', {
  timeout: 10000,
}, async () => {
  await assert.rejects(
    runSession({
      prepare: [node('failed build', 'process.exit(7)')],
      services: [node('must not start', 'process.exit(99)')],
    }),
    /failed build exited \(7\)/,
  )
})

test('readiness timeout cleans up the process', { timeout: 10000 }, async () => {
  const server = net.createServer()
  const port = await listen(server)
  await close(server)
  await assert.rejects(
    runSession({
      services: [node('unready backend', 'setInterval(() => {}, 1000)', { ports: [port] })],
      timeout: 200,
    }),
    /did not open port/,
  )
})

test('closing desktop stops backend and releases its listening port', {
  timeout: 15000,
}, async () => {
  const reservation = net.createServer()
  const port = await listen(reservation)
  await close(reservation)
  await runSession({
    services: [
      node(
        'backend fixture',
        `require('node:child_process').spawn(process.execPath, ['-e', ${JSON.stringify(`require('node:net').createServer().listen(${port}, '127.0.0.1')`)}], { stdio: 'inherit' })`,
        {
          ports: [port],
        },
      ),
      node('desktop fixture', 'setTimeout(() => process.exit(0), 100)', { exitTogether: true }),
    ],
  })
  const probe = net.createServer()
  await new Promise((resolve, reject) => {
    probe.once('error', reject)
    probe.listen(port, '127.0.0.1', resolve)
  })
  await close(probe)
})

test('external cancellation stops owned processes', { timeout: 10000 }, async () => {
  const controller = new AbortController()
  const timer = setTimeout(() => controller.abort(), 300)
  try {
    await runSession({
      services: [node('running fixture', 'setInterval(() => {}, 1000)')],
      signal: controller.signal,
    })
  } finally {
    clearTimeout(timer)
  }
})
