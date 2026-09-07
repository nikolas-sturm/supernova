#!/usr/bin/env node
import { spawn, spawnSync } from 'node:child_process'
import { createSocket } from 'node:dgram'
import { cpSync, existsSync, mkdirSync } from 'node:fs'
import net from 'node:net'
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
  const native = path.join(root, 'tooling/native/build.mjs')
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
      [vite, 'Run npm ci from the repository root with the pinned Node/npm versions.'],
      [
        shell,
        'Restore the Neutralino shell version pinned in apps/terra/neutralino.config.json (6.9.0); no automatic download is performed.',
      ],
    ],
    ports: [47984, 47989, 47990, 48010],
    udpPorts: [47998, 47999, 48000],
    prepare: [
      ...['sol', 'terra'].flatMap((app) =>
        ['configure', 'build'].map((operation) =>
          node(`${app} ${operation}`, [
            native,
            app,
            operation,
            'debug',
            ...(app === 'sol' ? ['--dev'] : []),
          ]),
        ),
      ),
      node('Sol web assets', [vite, 'build'], sol, { SOL_ASSETS_DIR: build }),
      node('Terra web assets', [vite, 'build'], terra),
    ],
    services: [
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
        ...node('Sol frontend', [vite, 'build', '--watch'], sol, { SOL_ASSETS_DIR: build }),
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
    plan.prepare = plan.prepare.filter(({ name }) => name.toLowerCase().startsWith(app))
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
      stdio: 'inherit',
      // Linux process groups let cleanup reach descendants without a shell or unref().
      detached: process.platform === 'linux',
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
    return { child, exited }
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
      launch(spec, true)
      const deadline = Date.now() + timeout
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
  await runSession({
    ...plan,
    beforePrepare() {
      if (app === 'terra') return
      mkdirSync(path.join(plan.build, 'assets'), { recursive: true })
      for (const platform of ['common', process.platform === 'win32' ? 'windows' : 'linux']) {
        cpSync(
          path.join(plan.sol, 'src_assets', platform, 'assets'),
          path.join(plan.build, 'assets'),
          {
            recursive: true,
            filter: (source) =>
              source !== path.join(plan.sol, 'src_assets/common/assets/web') &&
              !(
                process.platform === 'win32' &&
                source === path.join(plan.sol, 'src_assets/windows/assets/shaders')
              ),
          },
        )
      }
    },
  })
}

if (process.argv[1] && import.meta.url === pathToFileURL(path.resolve(process.argv[1])).href) {
  main(process.argv.slice(2)).catch((error) => {
    console.error(`[dev] ${error.message}`)
    process.exitCode = 1
  })
}
