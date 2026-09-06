import { lazy, StrictMode, Suspense } from 'react'
import { createRoot } from 'react-dom/client'
import { App } from './App'
import { useClientStore } from './store/clientStore'
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
