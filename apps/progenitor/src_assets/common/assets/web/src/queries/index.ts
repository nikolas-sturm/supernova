/**
 * @file TanStack Query client and shared query/mutation factories.
 */

import { mutationOptions, QueryClient, queryOptions } from '@tanstack/react-query'
import * as api from '../api/endpoints'

/** Shared query client instance. */
export const queryClient = new QueryClient()

/** Query keys used across the app. */
export const queryKeys = {
  config: ['config'] as const,
  apps: ['apps'] as const,
  clients: ['clients'] as const,
  pairings: ['pairings'] as const,
  logs: ['logs'] as const,
  virtualInputStatus: ['virtual-input-status'] as const,
  virtualInputLicense: ['virtual-input-license'] as const,
  release: (repo: string) => ['release', repo] as const,
  featured: ['featured'] as const,
}

/**
 * @brief Query options for the Sunshine configuration.
 */
export function configQuery() {
  return queryOptions({
    queryKey: queryKeys.config,
    queryFn: api.getConfig,
    staleTime: 60_000,
  })
}

/**
 * @brief Query options for the application list.
 */
export function appsQuery() {
  return queryOptions({
    queryKey: queryKeys.apps,
    queryFn: api.getApps,
  })
}

/**
 * @brief Query options for the paired clients list.
 */
export function clientsQuery() {
  return queryOptions({
    queryKey: queryKeys.clients,
    queryFn: api.getClients,
  })
}

/**
 * @brief Query options for pending pairing requests.
 */
export function pairingsQuery() {
  return queryOptions({
    queryKey: queryKeys.pairings,
    queryFn: api.getPendingPairings,
    refetchInterval: 2_000,
  })
}

/**
 * @brief Query options for the raw log text.
 */
export function logsQuery() {
  return queryOptions({
    queryKey: queryKeys.logs,
    queryFn: api.getLogs,
    refetchInterval: 5_000,
  })
}

/**
 * @brief Query options for virtual input driver status (Windows only).
 */
export function virtualInputStatusQuery() {
  return queryOptions({
    queryKey: queryKeys.virtualInputStatus,
    queryFn: api.getVirtualInputStatus,
  })
}

/**
 * @brief Query options for the virtual input license status.
 */
export function virtualInputLicenseQuery() {
  return queryOptions({
    queryKey: queryKeys.virtualInputLicense,
    queryFn: api.getVirtualInputLicense,
  })
}

/**
 * @brief Query options for the latest GitHub release of a repository.
 * @param repository Repository in owner/name form.
 */
export function releaseQuery(repository: string) {
  return queryOptions({
    queryKey: queryKeys.release(repository),
    queryFn: () => api.getGithubRelease(repository),
    staleTime: 5 * 60_000,
    retry: 1,
  })
}

/**
 * @brief Query options for the featured app directory.
 */
export function featuredQuery() {
  return queryOptions({
    queryKey: queryKeys.featured,
    queryFn: api.getFeaturedDirectory,
    staleTime: 10 * 60_000,
  })
}

/**
 * @brief Mutation options for saving the configuration.
 */
export function saveConfigMutation() {
  return mutationOptions({ mutationFn: api.saveConfig })
}

/**
 * @brief Mutation options for restarting Sunshine.
 */
export function restartMutation() {
  return mutationOptions({ mutationFn: api.restart })
}
