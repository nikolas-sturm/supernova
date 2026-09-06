import fs from 'node:fs'
import { resolve } from 'node:path'
import process from 'node:process'
import { codecovVitePlugin } from '@codecov/vite-plugin'
import react from '@vitejs/plugin-react'
import { defineConfig, type Plugin } from 'vite'

/**
 * Source and destination paths for the web UI.
 * CMake overrides these via environment variables so the built assets
 * land inside the CMake binary directory.
 */
let assetsSrcPath = 'src_assets/common/assets/web'
let assetsDstPath = 'build/assets/web'

if (process.env.SUNSHINE_BUILD_HOMEBREW) {
  console.log('Building for homebrew, using default paths')
} else {
  // If the paths supplied in the environment variables contain any symbolic links
  // at any point in the series of directories, the entire build will fail with
  // a cryptic error message. Resolve potential symlinks using `fs.realpathSync`.
  if (process.env.SUNSHINE_SOURCE_ASSETS_DIR) {
    const path = resolve(
      fs.realpathSync(process.env.SUNSHINE_SOURCE_ASSETS_DIR),
      'common/assets/web',
    )
    console.log(`Using srcdir from Cmake: ${path}`)
    assetsSrcPath = path
  }
  if (process.env.SUNSHINE_ASSETS_DIR) {
    const path = resolve(fs.realpathSync(process.env.SUNSHINE_ASSETS_DIR), 'assets/web')
    console.log(`Using destdir from Cmake: ${path}`)
    assetsDstPath = path
  }
}

/**
 * Page names served by the Sunshine backend. The backend maps routes like
 * `/apps` to `apps.html`; in the dev server we rewrite such requests so
 * client-side routing works with a plain page reload.
 */
const pageNames = new Set([
  'apps',
  'clients',
  'config',
  'featured',
  'index',
  'logout',
  'password',
  'pin',
  'troubleshooting',
  'welcome',
])

/**
 * Dev-server middleware that rewrites extension-less page requests
 * (`/apps`) to their HTML shells (`/apps.html`) so a reload during
 * development keeps working with TanStack Router's pathname-based routes.
 * @returns The Vite plugin.
 */
function sunshineHtmlRewrite(): Plugin {
  return {
    name: 'sunshine-html-rewrite',
    apply: 'serve',
    configureServer(server) {
      server.middlewares.use((req, _res, next) => {
        if (req.url) {
          const match = req.url.match(/^\/([A-Za-z]+)(\?.*)?$/)
          if (match && pageNames.has(match[1].toLowerCase())) {
            req.url = `/${match[1].toLowerCase()}.html${match[2] ?? ''}`
          }
        }
        next()
      })
    },
  }
}

// https://vitejs.dev/config/
export default defineConfig({
  resolve: {
    alias: {
      '@': resolve(assetsSrcPath, 'src'),
    },
  },
  base: './',
  plugins: [
    react({
      compiler: { target: '19' },
    }),
    sunshineHtmlRewrite(),
    // The Codecov vite plugin should be after all other plugins
    codecovVitePlugin({
      enableBundleAnalysis: true,
      bundleName: 'sunshine',
      uploadToken: process.env.CODECOV_TOKEN,
      gitService: 'github',
      dryRun: process.env.GITHUB_REPOSITORY !== 'LizardByte/Sunshine',
      telemetry: process.env.GITHUB_REPOSITORY === 'LizardByte/Sunshine',
    }),
  ],
  root: resolve(assetsSrcPath),
  server: {
    port: 5173,
    proxy: {
      '/api': {
        target: 'https://localhost:47990',
        changeOrigin: true,
        secure: false,
        ws: false,
      },
    },
  },
  build: {
    outDir: resolve(assetsDstPath),
    target: 'es2022',
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
