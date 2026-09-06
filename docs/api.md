# API

Sunshine has a RESTful API which can be used to interact with the service.

Unless otherwise specified, authentication is required for all API calls. You can authenticate using
basic authentication with the admin username and password.

## Eclipse API v1

The Eclipse extension API is served from the GameStream HTTPS port, normally `47984`, under
`/eclipse/v1`. It authenticates with an enabled paired-client certificate. Sunshine Web UI
credentials are not accepted. Existing GameStream routes and RTSP behavior remain available for
stock Moonlight clients.

Paired HTTPS `/serverinfo` responses advertise `EclipseApiVersion`, `EclipseCapabilities`, and
`EclipseApiPort`. These fields are intentionally omitted from unauthenticated HTTP discovery.
`EclipseCapabilities` contains only operational features. A usable host MAC address is included when
Sunshine can resolve one for the serving interface; unavailable Wake-on-LAN data is omitted rather
than represented by a zero address.

Sunshine advertises only capabilities whose complete routes and runtime dependencies are currently
operational: `client-permissions`, `session-ids`, `structured-errors`, `catalog-v2`, `events-v1`,
`profiles-v1`, `workspaces-v1`, `telemetry-v1`, and `peripherals-v1` on every supported platform,
plus `sandboxes-v1` and `displays-v1` on Windows while their providers stay healthy. Incomplete and
unhealthy provider-backed domains remain listed as unavailable under `features`.

Deleting a profile that is still referenced by a workspace definition, a launch profile, or
application metadata returns `resource_busy`; references must be removed first. Host name changes
emit `host.changed`, and capability advertisement changes emit `capabilities.changed` carrying the
current capability list.

Eclipse API errors use HTTP status codes and this stable envelope:

```json
{
  "schemaVersion": 1,
  "error": {
    "code": "permission_denied",
    "message": "Client certificate lacks catalog.read permission"
  }
}
```

Supported certificate-bound scopes are `catalog.read`, `stream.launch`, `session.control`,
`telemetry.read`, `display.read`, `display.manage`, `virtual-display.manage`,
`peripheral.forward`, `sandbox.manage`, and `host.control`. Paired clients created before this
schema receive all scopes and input classes to preserve existing behavior. Explicit Eclipse
pairing requests may request fewer scopes with `eclipseScopes` and input classes with
`eclipseInput`. Administrators can review requested platform and scopes before entering the PIN.

Application configuration specific to Eclipse is stored under a versioned `x-eclipse` object.
Missing stable identities are generated once and persisted without changing legacy Moonlight IDs:

```json
{
  "name": "Example",
  "x-eclipse": {
    "schemaVersion": 1,
    "uuid": "0d818e2c-4232-41a4-949b-2fdd100c0b26",
    "legacyId": 123,
    "kind": "game",
    "tags": ["rpg", "controller"],
    "source": "steam",
    "publisher": "Example Studio"
  }
}
```

Pairing requests must provide both `eclipseScopes` and `eclipseInput` to activate explicit policy.
Supplying only one field or any unknown value rejects pairing. Omitting both fields preserves legacy
Moonlight permissions; explicitly empty fields grant no Eclipse scopes or input classes.

### GET /eclipse/v1/capabilities

Returns API capabilities, paired-client UUID, granted scopes, application allowlist, expiry, host
identity, platform and version, Wake-on-LAN availability, status for every known capability, and
published resource limits. Unavailable capabilities include a stable `reasonCode` and are omitted
from `capabilities`.

### GET /eclipse/v1/apps

Requires `catalog.read`. Returns Catalog V2 metadata filtered by client application allowlist.
Pass `since=<revision>` for a conditional incremental check. An unchanged revision returns an
empty application array with `changed: false`; stale revisions receive a full snapshot.
Workspaces visible to the caller are additionally projected as first-class catalog entries with
`kind: workspace`, `uuid` equal to the workspace UUID, and profile references from the workspace
definition.

Catalog image metadata is generated from inspected local PNG or JPEG bytes. Assets larger than
8 MiB or images without readable dimensions are omitted without invalidating their application.
Asset URLs require the same paired-client certificate, `catalog.read` scope, and application
allowlist access as the catalog.

Profile references are `null` until they resolve to readable profile resources. Empty
`launchProfiles` indicates that only the application's default launch configuration is available.
When application artwork exists, authenticated `poster` and `icon` roles refer to its validated
bytes.

### GET /eclipse/v1/apps/{appUuid}/assets/{assetId}

Returns validated catalog image bytes with `Content-Type`, `Content-Length`, and a strong ETag.
Unknown, missing, oversized, disallowed, or malformed assets return the same `asset_not_found`
response so application visibility is not disclosed.

### GET /eclipse/v1/sessions

Requires `session.control`. Returns caller-owned active, preparing, disconnected, and retained
terminal sessions. Clients with `host.control` can see every session. The response contains a
monotonic collection `revision` and supports `since=<revision>`. Session resources carry
`stateReason`, `updatedAt`, `revision`, resolved `displayProfileId`, `streamProfileId`,
`launchProfileId`, and `sandboxProfileId` references, plus `workspaceId`, `sandboxId`, and
`peripheralClaimIds` associations. Terminal sessions remain queryable for at least 60 seconds and
are then removed with a `session.removed` event. Transitions emit `session.created`,
`session.updated`, and `session.removed`.

### GET /eclipse/v1/sessions/{id}

Requires `session.control`. Returns one visible session.

### GET /eclipse/v1/telemetry

Requires `telemetry.read`. Returns the host telemetry snapshot plus one entry per visible session.
Entries include only available host-observed values; unavailable metrics are `null`. Clients
without `host.control` observe session entries owned by their own identity only. The same document
is published as `telemetry.sample` events at a one-second cadence to `telemetry.read` subscribers.

### GET /eclipse/v1/sessions/{id}/telemetry

Requires `telemetry.read` and normal session visibility. Returns the telemetry entry for one
session; hidden or absent sessions return `session_not_found`.

### POST /eclipse/v1/sessions/{id}/disconnect

Requires `session.control`. Disconnects transport streams while leaving host application available
for GameStream resume.

### POST /eclipse/v1/sessions/{id}/stop

Requires `session.control`. Stops transport streams, owning host application, session-scoped
sandboxes, and bound peripheral claims. Cross-client control additionally requires `host.control`.

### GET /eclipse/v1/displays

Requires `display.read`. Windows only. Returns the unified physical and virtual display inventory
with a monotonic collection revision and `since=<revision>` change detection. Display resources
expose modes as exact rational refresh rates, `hdr` support and state, position, scale, rotation,
and capture eligibility.

### GET /eclipse/v1/displays/{id}

Requires `display.read`. Returns one unified display resource or `display_not_found`.

### PATCH /eclipse/v1/displays/{id}

Requires `display.manage`. Accepts any subset of `enabled`, `primary`, `modeId`, and `hdr` for
physical displays; `position`, `scale`, and `rotation` are immutable and return `invalid_argument`.
Virtual display identifiers are forwarded to the virtual-display patch semantics. The response
contains the resulting `display`; unsupported or unknown modes fail without partial application.

### GET /eclipse/v1/display-topology

Requires `display.read`. Returns `{"topology": {"id", "revision", "displays"}}` where entries carry
`id`, `enabled`, `primary`, `x`, `y`, `scale`, `rotation`, `modeId`, and `hdr`.

### PUT /eclipse/v1/display-topology

Requires `display.manage` and strong numeric `If-Match` against the topology revision. The body is
a complete desired topology for listed physical displays. Validation is atomic: unknown displays,
non-physical entries, disabled outputs, non-zero rotation, multiple primary requests, and
unsupported modes fail before any host state changes. The response returns the actual applied
`topology` and affected `displays`. Failed application reverts display configuration.

### GET /eclipse/v1/peripherals

Requires `peripheral.forward`. Returns registered peripheral device descriptors visible to the
caller with a monotonic collection revision and `since=<revision>` support.

### POST /eclipse/v1/peripherals

Requires `peripheral.forward`. Registers an Eclipse-side device descriptor from `class`,
`platformId`, `name`, `vendorId`, `productId`, optional `serial`, `capabilities`, and optional
`reportDescriptorBase64`. This host accepts keyboard and mouse classes with `keyboard.hid`,
`keyboard.text`, `mouse.hid`, and `mouse.relative` capabilities; other classes return
`unsupported_configuration`. Returns HTTP 201 with `peripheral`.

### GET /eclipse/v1/peripherals/{id}

Requires `peripheral.forward`. Returns one visible device or `peripheral_not_found`.

### DELETE /eclipse/v1/peripherals/{id}

Requires `peripheral.forward`. Unregisters the device and releases its active claims.

### GET /eclipse/v1/peripherals/claims

Requires `peripheral.forward`. Returns visible claims with `since=<revision>` support.

### POST /eclipse/v1/peripherals/claims

Requires `peripheral.forward`. Creates a claim from `deviceId`, `target` (`session`, `workspace`,
or `sandbox`), `requestedCapabilities`, `exclusive`, and `disconnectPolicy`. Targets must be
visible to the caller. Unsupported capabilities return `unsupported_configuration`; conflicting
exclusive claims return `resource_busy`. Returns HTTP 201 with `claim`.

Claims start `pending` and become `active` when the forwarding channel opens. The channel is a
WebSocket at `wss://host:port/eclipse/v1/peripherals/claims/{claimId}/channel` over the paired
client TLS connection, authenticated with the `X-Eclipse-Claim-Token` header carrying the claim
credential. Failed authentication returns a standard HTTP 401 error without establishing a
WebSocket. Messages follow the `eclipse-peripheral-json` version 1 protocol with `schemaVersion`,
monotonic `sequence`, `type`, `deviceId`, and `payload`: the first client message is `claim.open`,
the host replies `claim.ready` then `device.attached`, client `hid.input` reports carry base64 HID
boot reports injected for keyboard and mouse classes, and `claim.closed` ends the channel. Malformed
messages close with 1002, oversized messages with 1009, unauthorized claims with 1008, and normal
release with 1000. Closing applies the claim disconnect policy.

### GET /eclipse/v1/peripherals/claims/{id}

Requires `peripheral.forward`. Returns one visible claim or `claim_not_found`.

### DELETE /eclipse/v1/peripherals/claims/{id}

Requires `peripheral.forward`. Releases one claim idempotently and returns
`{"released": true, "id"}`.

Claim lifetime follows its target: session stop, workspace stop, or sandbox deletion release or
suspend matching claims according to policy, and client revocation releases owned claims. Live
channels close immediately in every case.

### GET /eclipse/v1/sandboxes

Requires `sandbox.manage`. Returns caller-owned sandbox resources and resources visible through
`host.control`. Application allowlists are applied before resources are returned. Pass
`since=<revision>` for collection change detection.

### GET /eclipse/v1/sandboxes/{id}

Requires `sandbox.manage`. Returns one owner-visible sandbox. Unknown, orphaned, disallowed, and
cross-client resources without `host.control` return `sandbox_not_found`.

### POST /eclipse/v1/sandboxes

Requires `sandbox.manage` and `Idempotency-Key`. Creates a sandbox definition asynchronously from
`profileId`, nullable `workspaceId`, nullable `appUuid`, `persistent`, and `name`. Current native
provider rejects caller-supplied workspace association with `unsupported_configuration`; workspace
lifecycle operations create their own bound sandbox resources. Profile and application references
are rechecked when queued work runs.

### POST /eclipse/v1/sandboxes/{id}/start

Requires `sandbox.manage`, `Idempotency-Key`, and strong numeric `If-Match`. Body may select
`appUuid` and `launchProfileId`. Provider resolves configured application immediately before launch,
creates non-elevated restricted primary token, assigns suspended process atomically to kill-on-close
Job Object, installs requested CPU, memory, process-count and wall-clock limits, then resumes it.

Launch profiles used by native sandbox must select same application and sandbox profile, must not
request display or stream profiles, elevation, hooks, resume, retained cleanup, or parallel/reused
launch. Arguments, administrator-configured working directories, and allowed environment values are
applied without logging secret contents.

### POST /eclipse/v1/sandboxes/{id}/stop

Requires `sandbox.manage`, `Idempotency-Key`, strong numeric `If-Match`, and `{ "force": boolean }`.
Terminates all Job Object processes, confirms zero active processes, applies cleanup, and persists
resulting sandbox state.

### POST /eclipse/v1/sandboxes/{id}/restart

Uses same authorization, headers, and body as stop. Restart begins only after confirmed termination
and cleanup. Failed resources retaining runtime associations cannot launch another process.

### DELETE /eclipse/v1/sandboxes/{id}

Requires `sandbox.manage`, `Idempotency-Key`, and strong numeric `If-Match`. Process termination and
cleanup complete before durable definition deletion.

### POST /eclipse/v1/sandboxes/{id}/adopt

Requires `sandbox.manage`, `host.control`, `Idempotency-Key`, strong numeric `If-Match`, and empty
schema-versioned object. Only orphaned persistent resources can be adopted.

Sandbox mutations return HTTP `202` operations. Every durable lifecycle transition emits
`sandbox.created`, `sandbox.updated`, or `sandbox.removed`. Runtime exits are reconciled every 250 ms.
Unpair, disable, expiry, or permission replacement terminates active runtime; persistent definitions
become orphaned and ephemeral definitions are deleted. Any malformed persisted sandbox record makes
the complete sandbox manager unavailable rather than discarding an unverified runtime identity.

Native Windows provider supports only explicitly relaxed policies it can enforce. Filesystem
isolation, WFP network filtering, storage quotas, restricted GPU/display/input/peripheral access,
clipboard restriction, elevation, and persistent sandbox data are rejected. Supported policy may
use configured-only executable, unrestricted filesystem/network/GPU/display/clipboard access,
empty input and peripheral grants, explicit environment allowlist, CPU/memory/process/time limits,
no elevation, no persistent data, and delete cleanup. Workspace preparation starts a bound sandbox
before GameStream launch, and session stop or configured disconnect cleanup terminates and removes
it. Direct sandbox lifecycle mutations reject workspace-bound resources with `resource_busy` to
preserve workspace transaction ownership. `sandboxes-v1` is advertised only when the live provider
health probe succeeds; an unhealthy provider reports its stable failure code under
`features.sandboxes-v1` instead.

## CSRF Protection

State-changing API endpoints (POST, DELETE) are protected against Cross-Site Request Forgery (CSRF) attacks.

**For Web Browsers:**
- Requests from same-origin (configured via `csrf_allowed_origins`) are automatically allowed
- Cross-origin requests require a CSRF token

**For Non-Browser Applications:**
- Non-browser clients (e.g. `curl`, scripts, custom apps) are **exempt** from CSRF protection
- CSRF attacks require a browser to silently attach credentials to a cross-origin request — this threat
  does not apply to non-browser clients that explicitly provide credentials with every request
- Requests with no `Origin` or `Referer` header (as is typical for non-browser clients) are automatically
  allowed without a CSRF token

**Example (browser-equivalent cross-origin request):**
```bash
# Get CSRF token
curl -u user:pass https://localhost:47990/api/csrf-token

# Use token in request
curl -u user:pass -H "X-CSRF-Token: your_token_here" \
  -X POST https://localhost:47990/api/restart
```

@htmlonly
<script src="api.js"></script>
@endhtmlonly

## GET /api/csrf-token
@copydoc confighttp::getCSRFToken()

## GET /api/apps
@copydoc confighttp::getApps()

## POST /api/apps
@copydoc confighttp::saveApp()

## POST /api/apps/close
@copydoc confighttp::closeApp()

## DELETE /api/apps/{index}
@copydoc confighttp::deleteApp()

## GET /api/browse
@copydoc confighttp::browseDirectory()

## GET /api/clients/list
@copydoc confighttp::getClients()

## POST /api/clients/unpair
@copydoc confighttp::unpair()

## POST /api/clients/unpair-all
@copydoc confighttp::unpairAll()

## POST /api/clients/update
@copydoc confighttp::updateClient()

## GET /api/config
@copydoc confighttp::getConfig()

## GET /api/configLocale
@copydoc confighttp::getLocale()

## POST /api/config
@copydoc confighttp::saveConfig()

## GET /api/covers/{index}
@copydoc confighttp::getCover()

## POST /api/covers/upload
@copydoc confighttp::uploadCover()

## GET /api/logs
@copydoc confighttp::getLogs()

## POST /api/password
@copydoc confighttp::savePassword()

## POST /api/pin
@copydoc confighttp::savePin()

## POST /api/reset-display-device-persistence
@copydoc confighttp::resetDisplayDevicePersistence()

## POST /api/restart
@copydoc confighttp::restart()

## GET /api/virtual-input/status
@copydoc confighttp::getVirtualInputStatus()

<div class="section_buttons">

| Previous                                    |                                  Next |
|:--------------------------------------------|--------------------------------------:|
| [Performance Tuning](performance_tuning.md) | [Troubleshooting](troubleshooting.md) |

</div>

<details style="display: none;">
  <summary></summary>
  [TOC]
</details>
