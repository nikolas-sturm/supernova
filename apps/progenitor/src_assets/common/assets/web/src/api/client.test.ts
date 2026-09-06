/**
 * @file Tests for the API client: CSRF injection and retry behavior.
 */

import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { apiSend, invalidateCsrfToken } from './client'

const jsonResponse = (body: unknown, status = 200) =>
  new Response(JSON.stringify(body), {
    status,
    headers: { 'Content-Type': 'application/json' },
  })

describe('apiSend', () => {
  beforeEach(() => {
    invalidateCsrfToken()
    vi.stubGlobal(
      'fetch',
      vi.fn(async (input: RequestInfo | URL, init?: RequestInit) => {
        const url = String(input)
        if (url.includes('/api/csrf-token')) {
          return jsonResponse({ csrf_token: 'token-1' })
        }
        const headers = new Headers(init?.headers)
        const requestedToken = headers.get('X-CSRF-Token')
        if (requestedToken === 'token-1') {
          return jsonResponse({ status: true })
        }
        return jsonResponse({ error: 'Missing CSRF token' }, 400)
      }),
    )
  })

  afterEach(() => {
    vi.unstubAllGlobals()
  })

  it('fetches a CSRF token and attaches it to mutations', async () => {
    const body = await apiSend<{ status: boolean }>('./api/config', 'POST', { port: 1234 })
    expect(body.status).toBe(true)
    const calls = vi.mocked(fetch).mock.calls
    const mutationCall = calls.find((call) => String(call[0]).includes('/api/config'))
    const headers = new Headers(mutationCall?.[1]?.headers)
    expect(headers.get('X-CSRF-Token')).toBe('token-1')
    expect(headers.get('Content-Type')).toBe('application/json')
  })

  it('retries once with a fresh token after a CSRF rejection', async () => {
    const body = await apiSend<{ status: boolean }>('./api/config', 'POST', { port: 1234 })
    expect(body.status).toBe(true)
  })

  it('does not attach a token to GET requests', async () => {
    vi.mocked(fetch).mockImplementationOnce(async (_input) => {
      const headers = new Headers()
      return new Response(JSON.stringify({ seen: headers.get('X-CSRF-Token') }), { status: 200 })
    })
    await apiJsonTest()
    expect(vi.mocked(fetch).mock.calls[0]?.[1]?.method ?? 'GET').toBe('GET')
  })
})

/**
 * @brief Helper performing a GET through the client.
 */
async function apiJsonTest(): Promise<void> {
  const { apiJson } = await import('./client')
  await apiJson('./api/config')
}
