/**
 * @file Low-level API fetch wrapper with CSRF handling.
 *
 * Cross-origin mutating requests require an `X-CSRF-Token` header
 * (see the backend `validate_csrf_token`). The token is fetched lazily
 * from `/api/csrf-token` and cached; CSRF rejections are retried once
 * with a fresh token. Same-origin requests are unaffected, so attaching
 * the header unconditionally is harmless in production and required
 * behind the dev-server proxy.
 */

import { notify } from '../store/toastStore'

/**
 * @brief Error messages that indicate a CSRF validation failure.
 */
const CSRF_ERRORS = new Set(['Missing CSRF token', 'Invalid CSRF token', 'CSRF token expired'])

let csrfToken: string | null = null
let csrfPromise: Promise<string | null> | null = null

/**
 * @brief Fetches (once) and caches the CSRF token for this session.
 * @returns The token, or null when it cannot be obtained (e.g. unauthenticated).
 */
export async function ensureCsrfToken(): Promise<string | null> {
  if (csrfToken) {
    return csrfToken
  }
  if (!csrfPromise) {
    csrfPromise = (async () => {
      try {
        const response = await fetch('./api/csrf-token')
        if (!response.ok) {
          return null
        }
        const body = (await response.json()) as { csrf_token?: string }
        csrfToken = body.csrf_token ?? null
        return csrfToken
      } catch {
        return null
      } finally {
        csrfPromise = null
      }
    })()
  }
  return csrfPromise
}

/**
 * @brief Clears the cached CSRF token (after a rejection).
 */
export function invalidateCsrfToken(): void {
  csrfToken = null
}

/** Error thrown by `apiJson` for non-2xx responses. */
export class ApiError extends Error {
  /** HTTP status code. */
  readonly status: number
  /** Parsed JSON body when available. */
  readonly body: unknown

  /**
   * @param status HTTP status code.
   * @param message Error message.
   * @param body Parsed JSON body when available.
   */
  constructor(status: number, message: string, body?: unknown) {
    super(message)
    this.name = 'ApiError'
    this.status = status
    this.body = body
  }
}

/**
 * @brief Wrapper around `fetch` with CSRF injection and single retry on CSRF rejection.
 * @param url The URL to fetch (relative paths resolve against the backend origin).
 * @param options Standard fetch options.
 * @returns The fetch Response.
 */
export async function apiFetch(url: string, options: RequestInit = {}): Promise<Response> {
  const method = (options.method ?? 'GET').toUpperCase()
  const headers = new Headers(options.headers)

  if (method !== 'GET' && method !== 'HEAD') {
    const token = await ensureCsrfToken()
    if (token) {
      headers.set('X-CSRF-Token', token)
    }
  }

  const response = await fetch(url, { ...options, headers })

  if (response.status === 400) {
    let body: { error?: string } | null = null
    try {
      body = (await response.clone().json()) as { error?: string }
    } catch {
      // Body is not JSON; nothing to inspect.
    }
    if (body && body.error !== undefined && CSRF_ERRORS.has(body.error)) {
      notify.error('_common.csrf_error_desc', '_common.csrf_error')
      invalidateCsrfToken()
      // Retry once with a fresh token.
      headers.delete('X-CSRF-Token')
      const token = await ensureCsrfToken()
      if (token) {
        headers.set('X-CSRF-Token', token)
      }
      return fetch(url, { ...options, headers })
    }
  }

  return response
}

/**
 * @brief Performs a request and returns the parsed JSON body.
 * @param url The URL to fetch.
 * @param options Standard fetch options.
 * @returns Parsed response body.
 * @throws ApiError when the response is not ok.
 */
export async function apiJson<T = unknown>(url: string, options: RequestInit = {}): Promise<T> {
  const response = await apiFetch(url, options)
  if (!response.ok) {
    let body: unknown
    try {
      body = await response.clone().json()
    } catch {
      // Body is not JSON.
    }
    const message =
      body !== null &&
      typeof body === 'object' &&
      'error' in body &&
      typeof (body as { error: unknown }).error === 'string'
        ? (body as { error: string }).error
        : `Request failed with status ${response.status}`
    throw new ApiError(response.status, message, body)
  }
  return (await response.json()) as T
}

/**
 * @brief Convenience helper for JSON POST/PUT/PATCH/DELETE requests.
 * @param url The URL to fetch.
 * @param method The HTTP method.
 * @param body Payload serialized as JSON; omit for empty bodies.
 * @returns Parsed response body.
 * @throws ApiError when the response is not ok.
 */
export async function apiSend<T = unknown>(
  url: string,
  method: string,
  body?: unknown,
): Promise<T> {
  const options: RequestInit = { method }
  if (body !== undefined) {
    options.headers = { 'Content-Type': 'application/json' }
    options.body = JSON.stringify(body)
  }
  return apiJson<T>(url, options)
}
