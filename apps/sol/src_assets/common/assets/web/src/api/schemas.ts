/**
 * @file Zod schemas for Sol API responses.
 *
 * Validation is intentionally loose: the config surface is huge and partly
 * platform-specific, so unknown fields are preserved (loose objects) while
 * the fields the UI relies on are typed.
 */

import { z } from 'zod'

/** Response of `GET /api/configLocale`. */
export const localeResponseSchema = z.object({
  status: z.boolean().optional(),
  locale: z.string().optional(),
})

/** Response of `GET /api/config`. */
export const configSchema = z.looseObject({
  platform: z.string().catch(''),
  status: z.union([z.string(), z.boolean()]).optional(),
  version: z.string().optional(),
})

/** TypeScript type of `GET /api/config`. */
export type SolConfig = z.infer<typeof configSchema>

/** A single application entry of `GET /api/apps`. */
export const appSchema = z.looseObject({
  name: z.string().optional(),
  output: z.string().optional(),
  cmd: z.string().optional(),
  index: z.number().optional(),
  'exclude-global-prep-cmd': z.union([z.boolean(), z.string()]).optional(),
  elevated: z.union([z.boolean(), z.string()]).optional(),
  'auto-detach': z.union([z.boolean(), z.string()]).optional(),
  'wait-all': z.union([z.boolean(), z.string()]).optional(),
  'exit-timeout': z.union([z.number(), z.string()]).optional(),
  'prep-cmd': z
    .array(
      z.looseObject({
        do: z.string().optional(),
        undo: z.string().optional(),
        elevated: z.union([z.boolean(), z.string()]).optional(),
      }),
    )
    .optional(),
  detached: z.array(z.string()).optional(),
  'image-path': z.string().optional(),
  'working-dir': z.string().optional(),
})

/** Application entry type. */
export type AppRecord = z.infer<typeof appSchema>

/** Response of `GET /api/apps`. */
export const appsResponseSchema = z.object({
  apps: z.array(appSchema),
})

/** A paired client from `GET /api/clients/list`. */
export const clientSchema = z.object({
  uuid: z.string(),
  name: z.string(),
  enabled: z.boolean(),
})

/** Response of `GET /api/clients/list`. */
export const clientsResponseSchema = z.object({
  status: z.boolean().optional(),
  named_certs: z.array(clientSchema).optional(),
})

/** Paired client type. */
export type Client = z.infer<typeof clientSchema>

/** A pending pairing request from `GET /api/pin`. */
export const pairingSchema = z.looseObject({
  id: z.string(),
  name: z.string().optional(),
  address: z.string().optional(),
  platform: z.string().optional(),
  requested_scopes: z.array(z.string()).default([]),
  requested_inputs: z.array(z.string()).default([]),
})

/** Pending pairing request type. */
export type Pairing = z.infer<typeof pairingSchema>

/** Response of `GET /api/pin`. */
export const pairingsResponseSchema = z.object({
  pairings: z.array(pairingSchema).default([]),
})

/** Installed status of one virtual input driver. */
export const driverStatusSchema = z.looseObject({
  installed: z.boolean().default(false),
  version: z.string().default(''),
  version_compatible: z.boolean().default(false),
  minimum_version: z.string().default(''),
  supported_versions: z.string().default(''),
})

/** Installed driver status type. */
export type DriverStatus = z.infer<typeof driverStatusSchema>

/** Response of `GET /api/virtual-input/status`. */
export const virtualInputStatusSchema = z.looseObject({
  virtualhid: driverStatusSchema.default({
    installed: false,
    version: '',
    version_compatible: false,
    minimum_version: '',
    supported_versions: '',
  }),
  vigembus: driverStatusSchema.default({
    installed: false,
    version: '',
    version_compatible: false,
    minimum_version: '',
    supported_versions: '',
  }),
})

/** Response of `GET /api/virtual-input/license`. */
export const virtualInputLicenseSchema = z.looseObject({
  operation_ok: z.boolean().default(false),
  service_available: z.boolean().default(false),
  state: z.string().default('unavailable'),
  licensed: z.boolean().default(false),
  active_devices: z.number().default(0),
  activation_limit: z.number().default(0),
  activation_usage: z.number().default(0),
  plan_name: z.string().default(''),
  customer_email: z.string().default(''),
  message: z.string().default(''),
  purchase_url: z.string().default(''),
  manage_account_url: z.string().default(''),
  error: z.string().optional(),
})

/** GitHub release (subset used by the UI). */
export const githubReleaseSchema = z.looseObject({
  tag_name: z.string(),
  name: z.string().optional(),
  html_url: z.string().optional(),
  body: z.string().optional(),
  draft: z.boolean().optional(),
  prerelease: z.boolean().optional(),
})

/** GitHub release type. */
export type GithubRelease = z.infer<typeof githubReleaseSchema>

/** Response of `GET /api/browse`. */
export const browseResponseSchema = z.object({
  path: z.string().optional(),
  parent: z.string().optional(),
  entries: z
    .array(
      z.object({
        name: z.string(),
        path: z.string(),
        type: z.string(),
      }),
    )
    .default([]),
  error: z.string().optional(),
})

/** Response of `POST /api/covers/upload`. */
export const coverUploadResponseSchema = z.looseObject({
  path: z.string(),
})

/** Generic `{status: boolean}` API responses. */
export const statusResponseSchema = z.object({
  status: z.boolean(),
  error: z.string().optional(),
})
