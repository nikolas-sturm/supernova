/**
 * @file Application bootstrap.
 *
 * Normalizes `.html` URLs produced by the static shells, initializes theming
 * and i18n, then mounts the router inside the query provider.
 */

import { QueryClientProvider } from '@tanstack/react-query'
import { RouterProvider } from '@tanstack/react-router'
import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'
import { FileBrowserModal } from './components/FileBrowser'
import { Toasts } from './components/Toasts'
import { initI18n } from './i18n'
import { queryClient } from './queries'
import { createAppRouter } from './router'
import { useThemeStore } from './store/themeStore'
import './theme/tokens.css'
import './theme/themes.css'
import './styles/global.css'

/**
 * @brief Rewrites `/page.html` URLs to `/page` so the router matches the
 * same path the backend serves for extension-less requests.
 */
function normalizePathname(): void {
  const { pathname, search } = window.location
  const match = pathname.match(/\/([A-Za-z]+)\.html$/)
  if (match) {
    window.history.replaceState(null, '', `/${match[1]?.toLowerCase()}${search}`)
  }
}

/** Full application tree shared by every page shell. */
function App() {
  return (
    <QueryClientProvider client={queryClient}>
      <RouterProvider router={createAppRouter()} />
      <Toasts />
      <FileBrowserModal />
    </QueryClientProvider>
  )
}

async function boot(): Promise<void> {
  normalizePathname()
  useThemeStore.getState().initialize()
  await initI18n()

  const rootElement = document.getElementById('root')
  if (!rootElement) {
    throw new Error('Missing application root')
  }
  createRoot(rootElement).render(
    <StrictMode>
      <App />
    </StrictMode>,
  )
}

void boot()
