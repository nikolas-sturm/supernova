import { useThemeStore } from '@supernova/design-system/theme'
import { lazy, StrictMode, Suspense } from 'react'
import { createRoot } from 'react-dom/client'
import { App } from './App'
import { useClientStore } from './store/clientStore'
import '@supernova/design-system/styles.css'
import './styles/global.css'

const StreamOverlay = lazy(() =>
  import('./overlay/StreamOverlay').then((module) => ({ default: module.StreamOverlay })),
)

const root = document.getElementById('root')

if (!root) {
  throw new Error('Missing application root')
}

const overlayWindow = /^\/stream-overlay\/[^/]+$/.test(window.location.pathname)
if (overlayWindow) document.documentElement.classList.add('stream-overlay-window')
// The overlay is intentionally a dark, translucent capture surface in every UI theme.
if (!overlayWindow) useThemeStore.getState().initialize()

async function renderApp() {
  if (!overlayWindow) await useClientStore.persist.rehydrate()

  createRoot(root as HTMLElement).render(
    <StrictMode>
      {overlayWindow ? (
        <Suspense fallback={null}>
          <StreamOverlay />
        </Suspense>
      ) : (
        <App />
      )}
    </StrictMode>,
  )
}

void renderApp()
