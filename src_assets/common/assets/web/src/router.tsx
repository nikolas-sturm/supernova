/**
 * @file TanStack Router route tree.
 *
 * The Sunshine backend serves one HTML shell per route (`/apps` →
 * `apps.html`); the shells all load this same SPA and the router keys off
 * `location.pathname`. Heavy routes are code-split with `React.lazy`.
 */

import { createRootRoute, createRoute, createRouter, Outlet } from '@tanstack/react-router'
import { lazy, Suspense } from 'react'
import { Spinner } from './components/ui'

/** Fallback shown while lazy routes load. */
const Pending = () => <Spinner />

/** Root layout: global chrome plus the routed outlet. */
function RootLayout() {
  return (
    <Suspense fallback={<Pending />}>
      <Outlet />
    </Suspense>
  )
}

const rootRoute = createRootRoute({ component: RootLayout })

/** Loads a lazily-split route component. */
function lazyComponent<T extends { default: () => React.JSX.Element }>(loader: () => Promise<T>) {
  const Component = lazy(async () => {
    const module = await loader()
    return { default: module.default }
  })
  return function LazyRoute() {
    return (
      <Suspense fallback={<Pending />}>
        <Component />
      </Suspense>
    )
  }
}

const indexRoute = createRoute({
  getParentRoute: () => rootRoute,
  path: '/',
  component: lazyComponent(() => import('./routes/DashboardRoute')),
})

const appsRoute = createRoute({
  getParentRoute: () => rootRoute,
  path: '/apps',
  component: lazyComponent(() => import('./routes/AppsRoute')),
})

const configRoute = createRoute({
  getParentRoute: () => rootRoute,
  path: '/config',
  component: lazyComponent(() => import('./routes/ConfigRoute')),
})

const clientsRoute = createRoute({
  getParentRoute: () => rootRoute,
  path: '/clients',
  component: lazyComponent(() => import('./routes/ClientsRoute')),
})

const featuredRoute = createRoute({
  getParentRoute: () => rootRoute,
  path: '/featured',
  component: lazyComponent(() => import('./routes/FeaturedRoute')),
})

const logoutRoute = createRoute({
  getParentRoute: () => rootRoute,
  path: '/logout',
  component: lazyComponent(() => import('./routes/LogoutRoute')),
})

const passwordRoute = createRoute({
  getParentRoute: () => rootRoute,
  path: '/password',
  component: lazyComponent(() => import('./routes/PasswordRoute')),
})

const pinRoute = createRoute({
  getParentRoute: () => rootRoute,
  path: '/pin',
  component: lazyComponent(() => import('./routes/PinRoute')),
})

const troubleshootingRoute = createRoute({
  getParentRoute: () => rootRoute,
  path: '/troubleshooting',
  component: lazyComponent(() => import('./routes/TroubleshootingRoute')),
})

const welcomeRoute = createRoute({
  getParentRoute: () => rootRoute,
  path: '/welcome',
  component: lazyComponent(() => import('./routes/WelcomeRoute')),
})

/** The complete route tree served by the backend's fixed page routes. */
export const routeTree = rootRoute.addChildren([
  indexRoute,
  appsRoute,
  configRoute,
  clientsRoute,
  featuredRoute,
  logoutRoute,
  passwordRoute,
  pinRoute,
  troubleshootingRoute,
  welcomeRoute,
])

/**
 * @brief Creates the application router.
 * @returns The router instance.
 */
export function createAppRouter() {
  return createRouter({
    routeTree,
    defaultPendingComponent: Pending,
  })
}
