#!/usr/bin/env node
import { spawn, spawnSync } from 'node:child_process'
import { createSocket } from 'node:dgram'
import { existsSync } from 'node:fs'
import net from 'node:net'
import { availableParallelism } from 'node:os'
import path from 'node:path'
import { setTimeout as delay } from 'node:timers/promises'
import { fileURLToPath, pathToFileURL } from 'node:url'

const root = fileURLToPath(new URL('../', import.meta.url))

export function developmentPlan(platform = process.platform, arch = process.arch, app = 'all') {
  if (!['all', 'sol', 'terra'].includes(app))
    throw new Error('Usage: node tooling/dev.mjs [sol|terra]')
  if (
    !['win32', 'linux'].includes(platform) ||
    !['x64', 'arm64'].includes(arch) ||
    (platform === 'win32' && arch !== 'x64')
  ) {
    throw new Error(`Unsupported development platform: ${platform}/${arch}`)
  }
  const sol = path.join(root, 'apps/sol')
  const terra = path.join(root, 'apps/terra')
  const build = path.join(sol, `cmake-build-${platform}-dev-debug`)
  const suffix = platform === 'win32' ? '.exe' : ''
  const shell = path.join(
    terra,
    'bin',
    `neutralino-${platform === 'win32' ? 'win' : 'linux'}_${arch}${suffix}`,
  )
  const vite = path.join(root, 'node_modules/vite/bin/vite.js')
  const nx = path.join(root, 'node_modules/nx/dist/bin/nx.js')
  const node = (name, args, cwd = root, env) => ({
    name,
    command: process.execPath,
    args,
    cwd,
    env,
  })
  const plan = {
    sol,
    terra,
    build,
    shell,
    required: [
      [nx, 'Run npm ci from the repository root with the pinned Node/npm versions.'],
      [vite, 'Run npm ci from the repository root with the pinned Node/npm versions.'],
      [
        shell,
        'Neutralino shell version pinned in apps/terra/neutralino.config.json is missing. Run npm run setup:terra (also run automatically by root npm install/ci).',
      ],
    ],
    ports: [47984, 47989, 47990, 48010],
    udpPorts: [47998, 47999, 48000],
    prepare: [
      node(
        `${app} preparation (Nx)`,
        [
          nx,
          'run-many',
          '-t',
          'dev:prepare',
          `--projects=${app === 'all' ? 'sol,terra' : app}`,
          '--configuration=dev',
          '--parallel=2',
          '--outputStyle=stream',
        ],
        root,
        {
          NX_DAEMON: 'false',
          CMAKE_BUILD_PARALLEL_LEVEL:
            process.env.CMAKE_BUILD_PARALLEL_LEVEL ||
            String(Math.max(1, Math.floor(availableParallelism() / (app === 'all' ? 2 : 1)))),
        },
      ),
    ],
    services: [
      {
        ...node('Sol frontend', [path.join(root, 'tooling/frontend/sol-watch.mjs')], sol, {
          SOL_ASSETS_DIR: build,
          SUPERNOVA_WEB_BUILD: '0',
          SOL_BUILD_HOMEBREW: '',
          SOL_SOURCE_ASSETS_DIR: '',
        }),
        readyMessage: true,
      },
      {
        name: 'Sol backend',
        command: path.join(build, `sol${suffix}`),
        args: ['port=47989'],
        cwd: build,
        ports: [47989, 47990],
        exitTogether: app === 'sol',
        env:
          platform === 'win32'
            ? {
                PATH: `${path.join(path.dirname(process.env.SUPERNOVA_MSYS2_SHELL || 'C:\\msys64\\msys2_shell.cmd'), 'ucrt64/bin')};${process.env.PATH ?? ''}`,
              }
            : {
                XDG_CONFIG_HOME: path.join(build, 'config'),
                CONFIGURATION_DIRECTORY: path.join(build, 'config'),
              },
      },
      {
        name: 'Terra desktop',
        command: shell,
        cwd: terra,
        // Neutralino serves the prepared frontend with its authenticated native bridge.
        args: [
          '--load-dir-res',
          `--path=${terra}`,
          '--window-enable-inspector=true',
          '--window-exit-process-on-close=true',
        ],
        exitTogether: true,
      },
    ],
  }
  if (app !== 'all') {
    plan.services = plan.services.filter(({ name }) => name.toLowerCase().startsWith(app))
    plan.ports = app === 'sol' ? plan.ports : []
    plan.udpPorts = app === 'sol' ? plan.udpPorts : []
    if (app === 'sol') plan.required = plan.required.filter(([file]) => file !== shell)
  }
  return plan
}

async function assertPortFree(port, protocol = 'TCP') {
  await new Promise((resolve, reject) => {
    const server = protocol === 'UDP' ? createSocket('udp4') : net.createServer()
    server.once('error', () => {
      if (protocol === 'UDP') server.close()
      reject(
        new Error(
          `${protocol} port ${port} is occupied. Stop the existing host/dev server first; it will not be killed.`,
        ),
      )
    })
    if (protocol === 'UDP') server.bind(port, '127.0.0.1', () => server.close(resolve))
    else server.listen(port, '127.0.0.1', () => server.close(resolve))
  })
}

async function portReady(port) {
  return new Promise((resolve) => {
    const socket = net.connect({ host: '127.0.0.1', port })
    const finish = (ready) => {
      socket.destroy()
      resolve(ready)
    }
    socket.once('connect', () => finish(true))
    socket.once('error', () => finish(false))
    socket.setTimeout(200, () => finish(false))
  })
}

/** Own the complete session; never stop unrelated hosts or leave detached servers running. */
export async function runSession({
  prepare = [],
  services,
  ports = [],
  udpPorts = [],
  beforePrepare,
  timeout = 60000,
  signal,
}) {
  const children = new Set()
  const controller = new AbortController()
  let failure
  const stop = (error) => {
    failure ??= error
    controller.abort()
  }
  const interrupt = () => stop()
  process.once('SIGINT', interrupt)
  process.once('SIGTERM', interrupt)
  signal?.addEventListener('abort', interrupt, { once: true })
  if (signal?.aborted) stop()

  const launch = (spec, persistent) => {
    console.log(`[dev] Starting ${spec.name}`)
    const env = { ...process.env }
    if (spec.env?.PATH && process.platform === 'win32') {
      for (const key of Object.keys(env)) if (key.toUpperCase() === 'PATH') delete env[key]
    }
    const child = spawn(spec.command, spec.args ?? [], {
      cwd: spec.cwd ?? root,
      env: { ...env, ...spec.env },
      stdio: spec.readyMessage ? ['inherit', 'inherit', 'inherit', 'ipc'] : 'inherit',
      // Linux process groups let cleanup reach descendants without a shell or unref().
      detached: process.platform === 'linux',
    })
    let ready = !spec.readyMessage
    if (spec.readyMessage)
      child.on('message', (message) => {
        if (message === 'ready') ready = true
      })
    children.add(child)
    const exited = new Promise((resolve) => {
      child.once('error', (error) => {
        stop(new Error(`${spec.name}: ${error.message}`))
        resolve()
      })
      child.once('exit', (code, exitSignal) => {
        if (!controller.signal.aborted && (persistent || code !== 0)) {
          stop(
            spec.exitTogether && code === 0
              ? undefined
              : new Error(`${spec.name} exited (${exitSignal ?? code}).`),
          )
        }
        resolve()
      })
    })
    return { child, exited, isReady: () => ready }
  }

  try {
    for (const port of ports) await assertPortFree(port)
    for (const port of udpPorts) await assertPortFree(port, 'UDP')
    if (controller.signal.aborted) return
    await beforePrepare?.()
    for (const spec of prepare) {
      if (controller.signal.aborted) break
      const { child, exited } = launch(spec, false)
      await Promise.race([
        exited,
        new Promise((resolve) =>
          controller.signal.addEventListener('abort', resolve, { once: true }),
        ),
      ])
      if (child.exitCode === 0) children.delete(child)
    }
    for (const spec of services) {
      if (controller.signal.aborted) break
      const service = launch(spec, true)
      const deadline = Date.now() + timeout
      while (!controller.signal.aborted && !service.isReady()) {
        if (Date.now() >= deadline)
          throw new Error(`${spec.name} did not become ready within ${timeout}ms.`)
        await delay(100)
      }
      for (const port of spec.ports ?? []) {
        while (!controller.signal.aborted && !(await portReady(port))) {
          if (Date.now() >= deadline)
            throw new Error(`${spec.name} did not open port ${port} within ${timeout}ms.`)
          await delay(100)
        }
      }
    }
    if (!controller.signal.aborted) {
      const surfaces = [
        services.some(({ name }) => name === 'Sol backend') && 'Sol UI: https://localhost:47990',
        services.some(({ name }) => name === 'Terra desktop') && 'Terra desktop launched',
      ].filter(Boolean)
      console.log(`[dev] ${surfaces.join(' | ') || 'Services started'}. Ctrl+C stops the session.`)
      await new Promise((resolve) =>
        controller.signal.addEventListener('abort', resolve, { once: true }),
      )
    }
  } finally {
    controller.abort()
    console.log('[dev] Stopping session processes')
    for (const child of children) {
      if (!child.pid) continue
      if (process.platform === 'win32') {
        // Request normal window/console shutdown before escalating owned process trees.
        if (child.exitCode === null && child.signalCode === null) {
          spawnSync('taskkill.exe', ['/PID', String(child.pid), '/T'], {
            stdio: 'ignore',
            timeout: 10000,
          })
        }
      } else {
        try {
          process.kill(-child.pid, 'SIGTERM')
        } catch (error) {
          if (error.code !== 'ESRCH') failure ??= error
        }
      }
    }
    const deadline = Date.now() + 2000
    while (
      Date.now() < deadline &&
      [...children].some(
        (child) => child.pid && child.exitCode === null && child.signalCode === null,
      )
    ) {
      await delay(50)
    }
    for (const child of children) {
      if (!child.pid) continue
      if (process.platform === 'win32') {
        if (child.exitCode === null && child.signalCode === null) {
          const result = spawnSync('taskkill.exe', ['/PID', String(child.pid), '/T', '/F'], {
            stdio: 'ignore',
            timeout: 10000,
          })
          if (result.error) failure ??= result.error
        }
      } else {
        try {
          process.kill(-child.pid, 'SIGKILL')
        } catch (error) {
          if (error.code !== 'ESRCH') failure ??= error
        }
      }
    }
    process.removeListener('SIGINT', interrupt)
    process.removeListener('SIGTERM', interrupt)
    signal?.removeEventListener('abort', interrupt)
  }
  if (failure) throw failure
}

export async function main(args) {
  if (args.length > 1 || (args.length && !['sol', 'terra'].includes(args[0]))) {
    throw new Error('Usage: node tooling/dev.mjs [sol|terra]')
  }
  const app = args[0] ?? 'all'
  const plan = developmentPlan(process.platform, process.arch, app)
  const missing = plan.required.filter(([file]) => !existsSync(file))
  if (missing.length) throw new Error(missing.map(([file, hint]) => `${file}\n${hint}`).join('\n'))
  await runSession(plan)
}

if (process.argv[1] && import.meta.url === pathToFileURL(path.resolve(process.argv[1])).href) {
  main(process.argv.slice(2)).catch((error) => {
    console.error(`[dev] ${error.message}`)
    process.exitCode = 1
  })
}
