/**
 * @file Typed endpoint functions for every Sunshine web API route.
 *
 * The route surface is fixed by the C++ backend (`confighttp.cpp`) and must
 * not change: functions here mirror those routes one-to-one.
 */

import { ApiError, apiFetch, apiJson, apiSend } from './client'
import {
  type AppRecord,
  appsResponseSchema,
  browseResponseSchema,
  clientsResponseSchema,
  configSchema,
  coverUploadResponseSchema,
  githubReleaseSchema,
  localeResponseSchema,
  pairingsResponseSchema,
  type SunshineConfig,
  statusResponseSchema,
  virtualInputLicenseSchema,
  virtualInputStatusSchema,
} from './schemas'

/**
 * @brief Fetches the configured locale.
 * @returns Locale response (works unauthenticated).
 */
export async function getLocale() {
  return localeResponseSchema.parse(await apiJson('./api/configLocale'))
}

/**
 * @brief Fetches the CSRF token for mutating requests.
 * @returns The raw token string.
 */
export async function getCsrfToken(): Promise<string> {
  const body = await apiJson<{ csrf_token: string }>('./api/csrf-token')
  return body.csrf_token
}

/**
 * @brief Fetches the full Sunshine configuration.
 * @returns Parsed configuration object.
 */
export async function getConfig(): Promise<SunshineConfig> {
  return configSchema.parse(await apiJson('./api/config'))
}

/**
 * @brief Saves configuration values.
 * @param config Only the values that differ from defaults.
 * @returns Status response.
 */
export async function saveConfig(config: Record<string, unknown>) {
  return statusResponseSchema.parse(await apiSend('./api/config', 'POST', config))
}

/**
 * @brief Requests a Sunshine restart.
 * @returns Raw response body.
 */
export async function restart() {
  return apiSend('./api/restart', 'POST')
}

/**
 * @brief Fetches the application list.
 * @returns Array of application records.
 */
export async function getApps(): Promise<AppRecord[]> {
  const body = appsResponseSchema.parse(await apiJson('./api/apps'))
  return body.apps
}

/**
 * @brief Saves (creates or updates) an application.
 * @param app The full application record.
 * @returns Raw response body.
 */
export async function saveApp(app: Record<string, unknown>) {
  return apiSend('./api/apps', 'POST', app)
}

/**
 * @brief Deletes an application by index.
 * @param index The application index.
 * @returns Raw response body.
 */
export async function deleteApp(index: number) {
  return apiSend(`./api/apps/${index}`, 'DELETE')
}

/**
 * @brief Force-closes the currently running application.
 * @returns Status response.
 */
export async function closeApp() {
  return statusResponseSchema.parse(await apiSend('./api/apps/close', 'POST'))
}

/**
 * @brief Uploads cover art for the given IGDB key.
 * @param key The IGDB game key (`igdb_<id>`).
 * @param url The full-resolution image URL.
 * @returns The persisted local cover path.
 */
export async function uploadCover(key: string, url: string): Promise<string> {
  const body = coverUploadResponseSchema.parse(
    await apiSend('./api/covers/upload', 'POST', { key, url }),
  )
  return body.path
}

/**
 * @brief Lists directory entries for the file browser.
 * @param type Entry filter type.
 * @param path Directory to list; omit for the root listing.
 * @returns Parsed browse response.
 */
export async function browseDirectory(type: string, path?: string) {
  const params = new URLSearchParams({ type })
  if (path) {
    params.set('path', path)
  }
  const body = await apiJson(`./api/browse?${params.toString()}`)
  const parsed = browseResponseSchema.parse(body)
  if (parsed.error) {
    throw new Error(parsed.error)
  }
  return parsed
}

/**
 * @brief Lists paired clients.
 * @returns Array of clients sorted by name, empty names last.
 */
export async function getClients() {
  const body = clientsResponseSchema.parse(await apiJson('./api/clients/list'))
  const clients = body.named_certs ?? []
  return [...clients].sort((a, b) => {
    if (a.name === '') {
      return 1
    }
    if (b.name === '') {
      return -1
    }
    return a.name.toLowerCase() > b.name.toLowerCase() ? 1 : -1
  })
}

/**
 * @brief Unpairs a single client.
 * @param uuid The client uuid.
 */
export async function unpairClient(uuid: string) {
  return apiSend('./api/clients/unpair', 'POST', { uuid })
}

/**
 * @brief Unpairs all clients.
 */
export async function unpairAll() {
  return statusResponseSchema.parse(await apiSend('./api/clients/unpair-all', 'POST'))
}

/**
 * @brief Updates a client (enable/disable).
 * @param uuid The client uuid.
 * @param enabled The new enabled state.
 */
export async function updateClient(uuid: string, enabled: boolean) {
  return apiSend('./api/clients/update', 'POST', { uuid, enabled })
}

/**
 * @brief Lists pending pairing requests.
 * @returns Array of pending pairings.
 */
export async function getPendingPairings() {
  const body = pairingsResponseSchema.parse(await apiJson('./api/pin'))
  return body.pairings
}

/**
 * @brief Submits a PIN for a pairing request.
 * @param pairingId The pending pairing id.
 * @param pin The 4-digit PIN.
 * @param name The device name to register.
 * @returns Status response.
 */
export async function savePin(pairingId: string, pin: string, name: string) {
  return statusResponseSchema.parse(
    await apiSend('./api/pin', 'POST', { pairing_id: pairingId, pin, name }),
  )
}

/**
 * @brief Cancels a pending pairing request.
 * @param pairingId The pending pairing id.
 */
export async function cancelPairing(pairingId: string) {
  return statusResponseSchema.parse(await apiSend('./api/pin', 'DELETE', { pairing_id: pairingId }))
}

/**
 * @brief Saves a new username/password.
 * @param payload Username and password fields.
 * @returns Status response.
 */
export async function savePassword(payload: {
  currentUsername?: string
  currentPassword?: string
  newUsername: string
  newPassword: string
  confirmNewPassword?: string
}) {
  return apiJson<{ status: boolean; error?: string }>('./api/password', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(payload),
  })
}

/**
 * @brief Fetches the raw log text.
 * @returns The log file contents.
 */
export async function getLogs(): Promise<string> {
  const response = await apiFetch('./api/logs')
  if (!response.ok) {
    throw new ApiError(response.status, `Request failed with status ${response.status}`)
  }
  return response.text()
}

/**
 * @brief Resets persisted display device settings (Windows only).
 * @returns Status response.
 */
export async function resetDisplayDevicePersistence() {
  return statusResponseSchema.parse(await apiSend('/api/reset-display-device-persistence', 'POST'))
}

/**
 * @brief Fetches virtual input driver installation status.
 * @returns Parsed status of virtualhid and vigembus.
 */
export async function getVirtualInputStatus() {
  return virtualInputStatusSchema.parse(await apiJson('./api/virtual-input/status'))
}

/**
 * @brief Fetches the virtual input driver license status.
 * @returns Parsed license status.
 */
export async function getVirtualInputLicense() {
  return virtualInputLicenseSchema.parse(await apiJson('./api/virtual-input/license'))
}

/**
 * @brief Performs a license operation (validate/activate/deactivate).
 * @param action The operation name.
 * @param licenseKey The license key (activate only).
 * @returns Parsed license status.
 */
export async function updateVirtualInputLicense(action: string, licenseKey?: string) {
  const body: Record<string, string> = { action }
  if (licenseKey !== undefined) {
    body.license_key = licenseKey
  }
  const response = await apiSend<Record<string, unknown>>(
    './api/virtual-input/license',
    'POST',
    body,
  )
  return virtualInputLicenseSchema.parse(response)
}

/**
 * @brief Fetches the latest release of a GitHub repository.
 * @param repository Repository in owner/name form.
 * @returns Parsed GitHub release.
 */
export async function getGithubRelease(repository: string) {
  return githubReleaseSchema.parse(
    await apiJson(`https://api.github.com/repos/${repository}/releases/latest`, {
      headers: { Accept: 'application/vnd.github+json' },
    }),
  )
}

/**
 * @brief Fetches the featured app directory.
 * @returns The raw directory JSON (apps + categories).
 */
export async function getFeaturedDirectory(): Promise<FeaturedDirectory> {
  const response = await fetch('https://app.lizardbyte.dev/app-directory/sunshine.json')
  if (!response.ok) {
    throw new Error('Failed to load featured apps')
  }
  return (await response.json()) as FeaturedDirectory
}

/** Shape of the featured app directory JSON. */
export interface FeaturedDirectory {
  apps: FeaturedApp[]
  categories: FeaturedCategory[]
}

/** One featured application entry. */
export interface FeaturedApp {
  id: string
  name: string
  tagline?: string
  description?: string
  icon?: string
  official?: boolean
  category?: string
  platforms?: string[]
  screenshots?: string[]
  downloads?: { label: string; url: string; img?: string }[]
  links?: { website?: string; documentation?: string; download?: string; github?: string }
  github?: { stars?: number; forks?: number; openIssues?: number; lastUpdated?: string }
}

/** One featured category entry. */
export interface FeaturedCategory {
  id: string
  originalId: string
}
