import fs from 'node:fs'
import { resolve } from 'node:path'
import process from 'node:process'
import { codecovVitePlugin } from '@codecov/vite-plugin'
import { defineConfig } from 'vite'
import {
  browserTarget,
  designSystemSource,
  nativeWatchIgnored,
  reactCompiler,
} from '../../tooling/frontend/config.ts'
import { solApiProxy, solHtmlRewrite } from './vite.auth.ts'

/**
 * Source and destination paths for the web UI.
 * CMake overrides these via environment variables so the built assets
 * land inside the CMake binary directory.
 */
let assetsSrcPath = 'src_assets/common/assets/web'
let assetsDstPath = 'build/assets/web'

if (process.env.SOL_BUILD_HOMEBREW) {
  console.log('Building for homebrew, using default paths')
} else {
  // If the paths supplied in the environment variables contain any symbolic links
  // at any point in the series of directories, the entire build will fail with
  // a cryptic error message. Resolve potential symlinks using `fs.realpathSync`.
  if (process.env.SOL_SOURCE_ASSETS_DIR) {
    const path = resolve(fs.realpathSync(process.env.SOL_SOURCE_ASSETS_DIR), 'common/assets/web')
    console.log(`Using srcdir from Cmake: ${path}`)
    assetsSrcPath = path
  }
  if (process.env.SOL_ASSETS_DIR) {
    const path = resolve(fs.realpathSync(process.env.SOL_ASSETS_DIR), 'assets/web')
    console.log(`Using destdir from Cmake: ${path}`)
    assetsDstPath = path
  }
}

// https://vitejs.dev/config/
export default defineConfig({
  resolve: {
    alias: {
      '@': resolve(assetsSrcPath, 'src'),
      '@supernova/design-system': designSystemSource,
    },
  },
  base: './',
  plugins: [
    reactCompiler(),
    solHtmlRewrite(),
    // The Codecov vite plugin should be after all other plugins
    codecovVitePlugin({
      enableBundleAnalysis: true,
      bundleName: 'sol',
      ...(process.env.CODECOV_TOKEN ? { uploadToken: process.env.CODECOV_TOKEN } : {}),
      gitService: 'github',
      dryRun: process.env.GITHUB_REPOSITORY !== 'LizardByte/Sunshine',
      telemetry: process.env.GITHUB_REPOSITORY === 'LizardByte/Sunshine',
    }),
  ],
  root: resolve(assetsSrcPath),
  server: {
    watch: { ignored: nativeWatchIgnored },
    host: '127.0.0.1',
    port: 5173,
    strictPort: true,
    proxy: {
      '/api': solApiProxy(process.env.SUPERNOVA_SOL_ADMIN_PORT || '47990'),
    },
  },
  build: {
    outDir: resolve(assetsDstPath),
    target: browserTarget,
    rollupOptions: {
      input: {
        apps: resolve(assetsSrcPath, 'apps.html'),
        clients: resolve(assetsSrcPath, 'clients.html'),
        config: resolve(assetsSrcPath, 'config.html'),
        featured: resolve(assetsSrcPath, 'featured.html'),
        index: resolve(assetsSrcPath, 'index.html'),
        logout: resolve(assetsSrcPath, 'logout.html'),
        password: resolve(assetsSrcPath, 'password.html'),
        pin: resolve(assetsSrcPath, 'pin.html'),
        troubleshooting: resolve(assetsSrcPath, 'troubleshooting.html'),
        welcome: resolve(assetsSrcPath, 'welcome.html'),
      },
    },
  },
})
