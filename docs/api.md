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

### GET /eclipse/v1/capabilities

Returns API capabilities, paired-client UUID, granted scopes, application allowlist, and expiry.

### GET /eclipse/v1/apps

Requires `catalog.read`. Returns Catalog V2 metadata filtered by client application allowlist.
Pass `since=<revision>` for a conditional incremental check. An unchanged revision returns an
empty application array with `changed: false`; stale revisions receive a full snapshot.

### GET /eclipse/v1/sessions

Requires `session.control`. Returns caller-owned active or disconnected resumable sessions.
Clients with `host.control` can see every session.

### GET /eclipse/v1/sessions/{id}

Requires `session.control`. Returns one visible session.

### POST /eclipse/v1/sessions/{id}/disconnect

Requires `session.control`. Disconnects transport streams while leaving host application available
for GameStream resume.

### POST /eclipse/v1/sessions/{id}/stop

Requires `session.control`. Stops transport streams and owning host application. Cross-client
control additionally requires `host.control`.

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
