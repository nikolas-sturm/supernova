#!/usr/bin/env node
import { cpSync, existsSync, mkdirSync, readFileSync, statSync } from 'node:fs'
import path from 'node:path'
import { fileURLToPath, pathToFileURL } from 'node:url'

/** Stage only development assets; leave runtime configuration and identities untouched. */
export function stageDevAssets(sol, build, platform = process.platform) {
  if (!['win32', 'linux'].includes(platform)) throw new Error(`Unsupported platform: ${platform}`)
  const assets = path.join(build, 'assets')
  mkdirSync(assets, { recursive: true })
  for (const sourcePlatform of ['common', platform === 'win32' ? 'windows' : 'linux']) {
    cpSync(path.join(sol, 'src_assets', sourcePlatform, 'assets'), assets, {
      recursive: true,
      filter: (source, destination) => {
        if (
          source === path.join(sol, 'src_assets/common/assets/web') ||
          (platform === 'win32' && source === path.join(sol, 'src_assets/windows/assets/shaders'))
        )
          return false
        return (
          !statSync(source).isFile() ||
          !existsSync(destination) ||
          !readFileSync(source).equals(readFileSync(destination))
        )
      },
    })
  }
}

if (process.argv[1] && import.meta.url === pathToFileURL(path.resolve(process.argv[1])).href) {
  const sol = fileURLToPath(new URL('../apps/sol', import.meta.url))
  stageDevAssets(sol, path.join(sol, `cmake-build-${process.platform}-dev-debug`))
}
