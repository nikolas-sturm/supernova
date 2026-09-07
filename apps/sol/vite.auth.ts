import { request } from 'node:http'
import type { Plugin, ProxyOptions } from 'vite'

/**
 * @brief Keep the existing API proxy policy and map same-origin account setup to the backend origin.
 * @param adminPort The native admin port selected by the development launcher.
 * @returns Proxy settings; untrusted origins are never rewritten.
 */
export function solApiProxy(adminPort: string): ProxyOptions {
  const target = `https://localhost:${adminPort}`
  return {
    target,
    changeOrigin: true,
    secure: false,
    ws: false,
    configure(proxy) {
      proxy.on('proxyReq', (outgoing, incoming) => {
        // Initial setup cannot fetch an authenticated CSRF token yet. Translate
        // only the verified frontend origin, never an arbitrary Origin/Referer.
        if (
          incoming.method === 'POST' &&
          incoming.url === '/api/password' &&
          (incoming.headers.origin === 'http://127.0.0.1:5173' ||
            incoming.headers.origin === 'http://localhost:5173')
        ) {
          outgoing.setHeader('Origin', target)
        }
      })
    },
  }
}

const pages = new Set([
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
 * @brief Preserve backend setup redirects and Basic-auth challenges on Vite page navigation.
 * @returns The development-only authentication and HTML route rewrite plugin.
 */
export function solHtmlRewrite(): Plugin {
  return {
    name: 'sol-html-rewrite',
    apply: 'serve',
    configureServer(server) {
      server.middlewares.use((req, res, next) => {
        const url = new URL(req.url ?? '/', 'http://localhost')
        const page = (
          url.pathname === '/' ? 'index' : url.pathname.slice(1).replace(/\.html$/, '')
        ).toLowerCase()
        if (req.method !== 'GET' || !pages.has(page)) return next()
        const servePage = () => {
          req.url = `/${page}.html${url.search}`
          next()
        }
        if (page === 'logout') return servePage()
        const address = server.httpServer?.address()
        if (!address || typeof address === 'string') {
          res.statusCode = 503
          res.end('Sol development server is not listening.')
          return
        }
        // Reuse the existing API proxy's TLS policy and target selection.
        // Unlike fetch(), this client preserves the backend's setup redirect.
        const check = request(
          {
            hostname: '127.0.0.1',
            port: address.port,
            path: '/api/config',
            headers: req.headers.authorization ? { authorization: req.headers.authorization } : {},
          },
          (response) => {
            response.resume()
            if (res.destroyed) return
            const setupRequired =
              response.statusCode === 307 && response.headers.location === '/welcome'
            res.setHeader('Cache-Control', 'no-store')
            if (setupRequired && page === 'welcome') return servePage()
            if (response.statusCode === 200 && page !== 'welcome') return servePage()
            if (
              setupRequired ||
              (page === 'welcome' && [200, 401].includes(response.statusCode ?? 0))
            ) {
              res.writeHead(307, { Location: setupRequired ? '/welcome' : '/' })
              res.end()
              return
            }
            res.statusCode = response.statusCode ?? 502
            if (response.headers['www-authenticate'])
              res.setHeader('WWW-Authenticate', response.headers['www-authenticate'])
            res.end(
              res.statusCode === 401
                ? 'Sign in with your Sol host credentials.'
                : 'Sol authentication check failed. Is the backend running?',
            )
          },
        )
        check.on('error', () => {
          if (res.destroyed || res.writableEnded) return
          res.statusCode = 502
          res.end(
            'Sol backend is unavailable. Start npm run dev or check the configured admin port.',
          )
        })
        check.setTimeout(5000, () => check.destroy(new Error('Sol authentication check timed out')))
        res.once('close', () => check.destroy())
        check.end()
      })
    },
  }
}
