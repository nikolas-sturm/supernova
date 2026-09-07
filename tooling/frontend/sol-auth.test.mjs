import assert from 'node:assert/strict'
import { createServer } from 'node:http'
import test from 'node:test'
import { solApiProxy, solHtmlRewrite } from '../../apps/sol/vite.auth.ts'

test('only same-origin password setup is translated through the dev proxy', () => {
  const proxy = solApiProxy('48990')
  assert.equal(proxy.target, 'https://localhost:48990')
  let onRequest
  proxy.configure({
    on: (event, handler) => {
      assert.equal(event, 'proxyReq')
      onRequest = handler
    },
  })
  for (const [origin, url, method, translated] of [
    ['http://127.0.0.1:5173', '/api/password', 'POST', true],
    ['http://localhost:5173', '/api/password', 'POST', true],
    ['http://evil.example', '/api/password', 'POST', false],
    ['http://127.0.0.1:5173.evil.example', '/api/password', 'POST', false],
    ['http://127.0.0.1:5174', '/api/password', 'POST', false],
    [undefined, '/api/password', 'POST', false],
    ['http://127.0.0.1:5173', '/api/config', 'POST', false],
    ['http://127.0.0.1:5173', '/api/password', 'GET', false],
  ]) {
    const headers = new Map()
    onRequest(
      { setHeader: (key, value) => headers.set(key, value) },
      { method, url, headers: { origin } },
    )
    assert.deepEqual([...headers], translated ? [['Origin', 'https://localhost:48990']] : [])
  }
})

test('Sol dev page navigation preserves setup, login, credentials, and logout', {
  timeout: 10000,
}, async () => {
  let configured = false
  let unavailable = false
  let gate
  const authorization = `Basic ${Buffer.from('fixture-user:fixture-password').toString('base64')}`
  const challenge = 'Basic realm="Sol Gamestream Host", charset="UTF-8"'
  const server = createServer((req, res) => {
    if (req.url === '/api/config') {
      if (unavailable) res.writeHead(502)
      else if (!configured) res.writeHead(307, { Location: '/welcome' })
      else if (req.headers.authorization !== authorization)
        res.writeHead(401, { 'WWW-Authenticate': challenge })
      else res.writeHead(200, { 'Content-Type': 'application/json' })
      res.end('{}')
      return
    }
    gate(req, res, () => res.end(`served ${req.url}`))
  })
  solHtmlRewrite().configureServer({
    httpServer: server,
    middlewares: {
      use: (middleware) => {
        gate = middleware
      },
    },
  })
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve))
  const get = (path, headers) =>
    fetch(`http://127.0.0.1:${server.address().port}${path}`, { redirect: 'manual', headers })
  try {
    for (const path of ['/', '/index.html', '/config', '/apps.html']) {
      const response = await get(path)
      assert.equal(response.status, 307)
      assert.equal(response.headers.get('location'), '/welcome')
    }
    assert.equal(await (await get('/welcome')).text(), 'served /welcome.html')
    configured = true
    const afterSetup = await get('/welcome')
    assert.equal(afterSetup.status, 307)
    assert.equal(afterSetup.headers.get('location'), '/')
    const unauthenticated = await get('/')
    assert.equal(unauthenticated.status, 401)
    assert.equal(unauthenticated.headers.get('www-authenticate'), challenge)
    assert.equal((await get('/', { Authorization: 'Basic invalid' })).status, 401)
    const authenticated = await get('/config?tab=network', { Authorization: authorization })
    assert.equal(authenticated.status, 200)
    assert.equal(await authenticated.text(), 'served /config.html?tab=network')
    assert.equal(authenticated.headers.get('cache-control'), 'no-store')
    assert.equal(await (await get('/logout')).text(), 'served /logout.html')
    assert.equal((await get('/api/config')).status, 401)
    unavailable = true
    assert.equal((await get('/')).status, 502)
  } finally {
    server.closeAllConnections()
    await new Promise((resolve) => server.close(resolve))
  }
})
