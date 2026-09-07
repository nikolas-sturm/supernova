#!/usr/bin/env node
import { spawnSync } from 'node:child_process'
import { existsSync, readFileSync } from 'node:fs'
import path from 'node:path'
import { fileURLToPath, pathToFileURL } from 'node:url'

/** Restore the exact project runtime; npm installs the CLI, not the desktop executable. */
export function restore({
  root = fileURLToPath(new URL('../../', import.meta.url)),
  platform = process.platform,
  arch = process.arch,
  run = spawnSync,
} = {}) {
  const binary =
    platform === 'win32' && arch === 'x64'
      ? 'neutralino-win_x64.exe'
      : platform === 'linux' && ['x64', 'arm64'].includes(arch)
        ? `neutralino-linux_${arch}`
        : undefined
  if (!binary)
    throw new Error(`Unsupported Neutralino platform: ${platform}/${arch}. Windows and Linux only.`)
  const app = path.join(root, 'apps/terra')
  const config = JSON.parse(readFileSync(path.join(app, 'neutralino.config.json'), 'utf8'))
  for (const key of ['binaryVersion', 'clientVersion']) {
    if (!/^\d+\.\d+\.\d+$/.test(config.cli?.[key] ?? '')) {
      throw new Error(
        `Terra cli.${key} must contain an exact release version; latest/nightly fallback is not allowed.`,
      )
    }
  }
  const cli = path.join(root, 'node_modules/@neutralinojs/neu/bin/neu.js')
  if (!existsSync(cli))
    throw new Error(
      'Neutralino CLI missing. Run npm ci at the repository root with development dependencies enabled.',
    )
  console.log(
    `[setup] Restoring Neutralino ${config.cli.binaryVersion} from the pinned project configuration`,
  )
  const result = run(process.execPath, [cli, 'update'], {
    cwd: app,
    stdio: 'inherit',
    timeout: 120000,
  })
  if (result.error || result.status !== 0) {
    throw new Error(
      `Neutralino restore failed (${result.error?.message ?? result.signal ?? result.status}). Close Terra, check access to GitHub releases, then run npm run setup:terra.`,
    )
  }
  const executable = path.join(app, 'bin', binary)
  if (!existsSync(executable)) throw new Error(`Neutralino restore did not produce ${executable}`)
  console.log(`[setup] Native desktop runtime ready: ${executable}`)
}

if (process.argv[1] && import.meta.url === pathToFileURL(path.resolve(process.argv[1])).href) {
  try {
    restore()
  } catch (error) {
    console.error(`[setup] ${error.message}`)
    process.exitCode = 1
  }
}
