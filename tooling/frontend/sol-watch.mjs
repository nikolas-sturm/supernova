#!/usr/bin/env node
import { build } from 'vite'
import { nativeWatchIgnored } from './config.ts'

// The first watch build is the initial build. The launcher waits before starting Sol.
const watcher = await build({ build: { watch: { exclude: nativeWatchIgnored } } })
let ready = false
let failed = false
watcher.on('event', async (event) => {
  if (event.code === 'ERROR') {
    console.error(event.error)
    if (!ready) {
      failed = true
      await watcher.close()
      process.exit(1)
    }
  }
  if (event.code === 'END' && !ready && !failed) {
    ready = true
    process.send?.('ready')
  }
})
