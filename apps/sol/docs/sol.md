# Sol Naming and Compatibility

Sol is the official host application in `apps/sol`, replacing the Sunshine/Progenitor
product names. The directory, npm package, Nx project, and frontend identity use Sol.
Terra is the official client name, replacing Moonlight/Eclipse product branding.
Upstream Moonlight dependencies retain their names. Host symbols use `sol`, `Sol`, or `SOL`;
private client-integration symbols, files, namespaces, and tests use `terra` or
`Terra`. Shared branding uses `TerraBrand`, renders `TERRA`, and uses
`data-terra-button` without old aliases. All 21 themes, the `dark` default, and
the existing `/eclipse/v1` API route namespace remain unchanged. Windows and Linux are the
only supported platforms.

## Build and Package Names

- Native application: `sol` or `sol.exe`.
- Native tests: `tests/test_sol` or `tests/test_sol.exe`.
- Windows service: `SolService`, launched by `tools/solsvc.exe`.
- Windows installer orchestration: `scripts/sol-setup.ps1`.
- Linux application ID: `dev.lizardbyte.app.Sol`.
- Linux systemd service: `app-dev.lizardbyte.app.Sol.service`, alias `sol.service`.
- Linux assets: `share/sol`; input/module rules: `60-sol.rules` and `60-sol.conf`.
- CMake and frontend asset environment options: `SOL_*` instead of `SUNSHINE_*`.
- npm workspace package and Nx project: `sol`; workspace selection: `apps/sol`.
- Root development command: `npm run dev:sol`; native wrapper app argument: `sol`.
  No old project-name aliases are provided.

## Deliberate Compatibility Exceptions

These are existing persisted or externally consumed contracts, not new aliases:

- `sunshine.conf` and `sunshine_state.json`: existing configuration, paired-client
  identity, credentials, and state. The default log is now `sol.log`.
- Linux configuration directory `sunshine` under `XDG_CONFIG_HOME`,
  `CONFIGURATION_DIRECTORY`, or `~/.config`: preserves existing configuration lookup
  and the container `/config` volume. Existing Flatpak migration uses that same name.
- `sunshine_name`: saved configuration and web API key. The C++ member is `sol_name`;
  frontend payloads retain the wire key.
- `SUNSHINE_SERVER_BUSY` and `SUNSHINE_SERVER_FREE`: GameStream `/serverinfo` state
  values consumed by Moonlight clients.
- `sunshineCpuPercent` and `sunshineMemoryBytes`: existing Terra API telemetry keys.
- `/eclipse/v1`, `X-Eclipse-*`, `Eclipse*` serverinfo/launch response fields,
  `eclipse*` pairing/launch query fields, and `eclipse-peripheral-json`: existing
  wire contracts. The `eclipse` query-prefix length remains part of metadata lookup.
- `x-eclipse` application metadata and `eclipse_permissions.*` paired-client state
  keys: preserve catalog UUIDs and authorization policy.
- `eclipse_operations.json`, `eclipse_sandboxes.json`, `eclipse_profiles.json`,
  `eclipse_workspaces.json`, and `eclipse_virtual_displays.json`: existing durable
  resource stores. Renaming private implementation files does not rename user data.
- Workspace catalog `source: eclipse` and the `eclipse-display-topology` UUID seed:
  retain wire values and stable resource identity, not private implementation names.
- `SUNSHINE_APP_ID`, `SUNSHINE_APP_NAME`, and `SUNSHINE_CLIENT_NAME`, `WIDTH`,
  `HEIGHT`, `FPS`, `HDR`, `GCMAP`, `HOST_AUDIO`, `ENABLE_SOPS`,
  `AUDIO_CONFIGURATION`, `AUDIO_SURROUND_PARAMS`: environment variables consumed by
  users' saved application and preparation commands. Each client suffix has the
  `SUNSHINE_CLIENT_` prefix; no duplicate `SOL_*` launch variables are introduced.
- `sunshine-gamepad-{index}`, `sunshine-keyboard`, `sunshine-mouse`,
  `sunshine-touchscreen`, and `sunshine-pen-tablet`: virtual HID stable IDs preserve
  device bindings. Human-readable device names use Sol.
- `sink-sunshine-stereo`, `sink-sunshine-surround51`, and
  `sink-sunshine-surround71`: PulseAudio sink names may occur in saved audio settings.
- `%ProgramData%/Sunshine`: NVIDIA global-setting undo data must remain discoverable
  after an interrupted session. Internal variable and executable profile names use Sol.
- `%LOCALAPPDATA%/LizardByte/Sunshine/service_start_type.txt`: retains saved Windows
  service start policy during upgrades.
- `Software\LizardByte\Sunshine` WiX registry key paths and the existing WiX upgrade
  GUID: retain installer component/upgrade identity. Executables and shortcuts use Sol.
- Windows default installation directory `Sunshine` under Program Files: retained
  for upgrades to find existing configuration, state, credentials, and pairing identity.
  The package is named Sol, but `migrate-config.bat` only checks the current install
  root. No cross-root migration is added or required for the retained default path.
- `sunshinesvc` and `SunshineService` in legacy installation cleanup refer to services
  created by older installers. New service operations use `SolService`.
- `sunshine_name`, `sunshine_name_desc`, and `restart_sunshine*` localization keys:
  preserve existing translation catalogs. Only `en.json` display values change;
  other locales, including `en_US` and `en_GB`, remain untouched per local rules.
- `theme_sunshine` and its `Sunshine` label: the existing shared Terra theme is
  not the host application and remains one of the same 21 themes.

Choosing a different installation directory or Flatpak application ID does not move user data.
For a custom Windows installation directory, explicitly migrate the old `config`
directory, including credentials and state, before starting Sol. For Flatpak, migrate
the old sandbox's configuration and grant permissions to the new application ID.
Back up the original data, retain credential access restrictions, and do not run
the old and new hosts concurrently. No automatic cross-installation copy or runtime
migration is performed by this source rename.

## Upstream and Imported Material

Sunshine repository URLs, source references, issue references, external images,
release feeds, app-directory feeds, badges, and remote service identities keep their
original names. The frontend still reads upstream release and app-directory feeds;
these are not Sol-specific publishing endpoints. Upstream distribution examples in
`getting_started.md` and `DOCKER_README.md` remain explicitly labeled as such.
`awesome_sunshine.md`, the upstream changelog and maintainer guides, Sonar project
identity, Crowdin destination, `locale/sunshine.po` references, copyright notices,
licenses, vendored files, dependency pins, hashes, and submodules are preserved.

Imported macOS/FreeBSD source, platform-specific CMake files, assets, scripts,
Homebrew formula, and CI files retain their original names and contents. They are
not supported build targets and no compatibility shim is added to make them build
against renamed shared Sol symbols.

## Integration Status

Coordinated integration is complete: root native wrappers and their tests use
`SOL_BUILD_WEB_UI`, `SOL_WEB_ASSETS_DIR`, and `test_sol`; workspace documentation
and the root lockfile reflect the Sol package identity. Sol's private Terra
implementation files, includes, namespaces, CMake source lists, and tests are aligned.

Terra's first-party pairing prompts now name Sol, including the native progress
message. Upstream Sunshine compatibility and Moonlight transport references remain.

Native verification remains pending on Windows and Linux, including compilation,
native tests, and a Windows MSI upgrade preserving configuration and pairing identity.
Frontend checks and static reference audits do not substitute for that verification.
