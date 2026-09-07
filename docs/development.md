# Development

Run the commands below from the repository root. These are available interfaces,
not claims that every build or check has passed.

## Prerequisites

- Windows and Linux only. Do not add macOS or other platform CI support based on
  retained upstream source.
- Node.js 26.8.1 and npm 12. The root engines accept Node `>=26.8.1 <27` and npm
  `>=12 <13`; `packageManager` pins npm 12.0.2.
- TypeScript 7.0.2 and Nx 23.2.0 come from the root npm installation. Do not
  require a global Nx installation.
- First-party native C++ uses C++23. Preserve dependency-specific C/C++ standards.
- Git and initialized recursive submodules at the recorded revisions.

```sh
git submodule update --init --recursive
npm ci
```

Do not use `git submodule update --remote` as setup: setup restores recorded pins,
not the latest upstream revisions. Install npm dependencies once at the root.
The root `postinstall` hook also downloads the Neutralino version pinned in
`apps/terra/neutralino.config.json`; the npm CLI/client packages alone do not
contain its desktop executable. Both `npm install` and `npm ci` perform this step.
Close Terra before reinstalling. When using `--ignore-scripts`, or repairing an
older checkout's missing runtime, run `npm run setup:terra`. Restore failures
fail installation rather than leaving an apparently complete desktop setup.

## Frontend And Checks

### Full Local Session

```sh
npm run dev
```

Run either application independently with `npm run dev:sol` or `npm run dev:terra`.
Each command builds and launches that application's native runtime, not a
browser-only preview. Sol-only startup does not require Neutralino; Terra-only
startup does not build Sol or reserve any host streaming ports.

This single foreground command configures/builds Sol and Terra serially, stages
runtime assets, starts Sol with its built-in HTTPS web UI and a frontend build
watcher, then opens Terra desktop with its built frontend served by Neutralino.
Sol UI is at `https://localhost:47990`. Neither command starts a Vite HTTP
server on 5173 or 5174. Sol assets rebuild on edits; refresh the browser to load
changes. Restart `dev:terra` after Terra frontend or native changes.
Closing Terra or pressing Ctrl+C stops
the session's processes. A failed build/service or occupied port aborts startup;
existing hosts and dev servers are never killed.

Native prerequisites must already be installed, including the Neutralino 6.9.0
shell under `apps/terra/bin`. The launcher does not install tools, download the
shell, change dependency pins, or bypass native build failures. WiX 4.0.4 is
installed once per build tree and its exact version is checked on later runs. Close
other Terra instances before running it: the core build shares the staging path.
Run this long-lived command in your own terminal, not an automation runner.

Sol uses `apps/sol/cmake-build-<platform>-dev-debug`, with generated runtime assets
and a separate development configuration/paired-client identity there. It does
not import your installed host's configuration. Terra retains its existing app
settings and pairing storage. Pair Terra with this development host separately.
Development Sol uses the standard HTTP port `47989` and admin HTTPS port `47990`.
Stop any installed Sol/Sunshine host before starting it; the launcher refuses
occupied ports rather than silently moving the service or killing another host.
Add `http://127.0.0.1:47989` in Terra to test this local host.
At `https://localhost:47990`, Sol handles first-run setup, Basic authentication,
CSRF checks, pages, and API requests on the same native origin.
Credentials for an installed Sunshine host do not automatically apply here.

Sol retains Sunshine's default port mapping (base port 47989):

| Protocol | Port | Purpose |
| --- | --- | --- |
| TCP | 47984 | Client HTTPS/pairing |
| TCP | 47989 | Client HTTP/server discovery information |
| TCP | 47990 | Admin HTTPS web UI and API |
| TCP | 48010 | RTSP session setup |
| UDP | 47998 | Video |
| UDP | 47999 | Control |
| UDP | 48000 | Audio |

The launcher checks both TCP and UDP host ports for conflicts. Discovery keeps
the upstream `_nvstream._tcp` advertisement and mapped HTTP port; shared mDNS
uses its normal infrastructure, not a replacement application port.

Browser-only previews remain explicitly available through `npx nx run sol:dev:web`
and `npx nx run terra:dev:web`; these are not the normal app launch commands. After
native edits, stop and rerun `npm run dev`; native processes do not hot-reload.

The development-only native wrapper suffix `--dev` is accepted for Sol debug
configure/build/test operations. It selects the isolated tree and points compiled
asset lookup at that tree, without changing release packaging paths.

The expected orchestration interface is the root npm scripts backed by local Nx:

```sh
npm run check
npm run build
npm run dev:sol
npm run dev:terra
npm run graph
```

`check` includes tests. Root `build` runs `build:web` targets only. The optional
Terra browser-only Nx preview intentionally reports the native core unavailable.

### Nx Process Lifetime

`nx.json` sets `useDaemonProcess: false`. A controlled Windows reproduction showed
the daemon-enabled Nx client exiting successfully and closing its output streams
in 4.9 seconds while the detached daemon remained alive. The same project listing
without the daemon exited in 0.55 seconds with no surviving daemon. The automation
runner was waiting on the background process, not on graph computation.

Disabling the optional watcher keeps Nx graph generation and disk/task caching;
it does not replace Nx or change toolchains. This workspace defaults to the
non-daemon mode so ordinary `npx nx show projects` also terminates through the
runner. `NX_DAEMON=true` overrides this setting and can reintroduce the problem.
The runner's general handling of persistent background processes is not repaired.

Direct workspace scripts are also available for validation:

```sh
npm run check:workspace
npm run check --workspace apps/sol
npm run build --workspace apps/sol
npm run check --workspace apps/terra
npm run build:web --workspace apps/terra
```

These invoke app tooling directly and do not replace Nx as the intended
orchestrator. Each `check` runs lint, typecheck, and tests. When tests are not
authorized, use the individual `lint` and `typecheck` workspace scripts instead;
do not run `check`. Direct app checks do not cover every shared-project target.
Design-system contract tests and native-wrapper tests have separate commands:

```sh
npm run test --workspace packages/design-system
npm run test:tooling
```

`test:tooling` and `check:workspace` include a bounded Nx project-listing regression
test. Do not run them under an explicit no-Nx restriction.

Sol frontend output defaults to `apps/sol/build/assets/web`; Terra
frontend output is `apps/terra/build/web`. Terra's workspace `build` and `bundle`
scripts additionally configure/build native code, run checks including tests, and
invoke Neutralino packaging. They are not frontend-only validation commands.

## Native Builds

### Sol Directory Move

The host now lives at `apps/sol`; npm, Nx, and the native wrapper accept `sol`
only. The whole directory was moved, including untracked assets, app-local npm
dependencies, and third-party submodule worktrees. Existing native caches are
preserved at `apps/sol/cmake-build-legacy-debug` and
`apps/sol/cmake-build-legacy-win32-debug`. They embed absolute paths to the former
`apps/progenitor` location and must not be reused or treated as verified builds.
Run a fresh authorized configure into `apps/sol/cmake-build-<platform>-<config>`
before native compilation; do not copy old `CMakeCache.txt` files into it.

The `.gitmodules` paths use `apps/sol`; section names retain their original
`progenitor/` identity and upstream URLs. The authorized Git relocation updated
gitlink paths and repaired all 47 initialized submodule worktree pointers through
`git mv`. Normal recursive submodule status works; dependency pins, histories,
and existing submodule edits remain unchanged. History tests verify the current
indexed paths and use normal Git repository discovery in moved worktrees.

Generated root npm junctions now map `node_modules/sol` to `apps/sol` and
`node_modules/terra-client` to `apps/terra`. Their old generated names were removed
without running npm installation or lifecycle scripts. No dependency versions changed.

The [native wrapper reference](../tooling/native/README.md) documents the complete
interface. It installs nothing, does not invoke Nx, and keeps configure, build,
and test separate:

```sh
node tooling/native/build.mjs sol configure debug
node tooling/native/build.mjs sol build debug
node tooling/native/build.mjs sol test debug
node tooling/native/build.mjs terra configure debug
node tooling/native/build.mjs terra build debug
node tooling/native/build.mjs terra test debug
```

Use `release` instead of `debug` for the release configuration. Run each step only
after the preceding step succeeds, and run tests only when authorized. Nx exposes
`native:configure`, `native:build`, and `native:test` on both apps with debug as the
default and a release configuration. Native targets disable caching and arrange
configure/build/test dependencies. They use the same non-daemon workspace mode.

| App | CMake source root | Build trees |
| --- | --- | --- |
| Sol | `apps/sol` | `apps/sol/cmake-build-<platform>-<config>` |
| Terra | `apps/terra/native` | `apps/terra/cmake-build-<platform>-<config>` |

`platform` is `win32` or `linux`; `config` is `debug` or `release`. Never share a
build tree across apps, configurations, platforms, or toolchains. A separate
Linux checkout is still recommended to avoid mixing Node native packages. Do not run competing
operations in the same tree. Sol uses GoogleTest's `tests/test_sol`
with the build tree's `tests` directory as the working directory. Terra uses
CTest; no registered tests is an error.

### Windows

Use MSYS2 UCRT64, not MSVC or another MinGW environment. The wrapper launches
`C:\msys64\msys2_shell.cmd` with `-ucrt64` and checks tools under `/ucrt64/bin`.
`SUPERNOVA_MSYS2_SHELL` may specify an absolute path to another MSYS2 installation.
Required tools include CMake, Ninja, GCC/G++, and CTest, plus each app's native
dependencies such as OpenSSL.

The local UCRT64 environment has CMake 4.4.2, Ninja, and GCC 16.2. Sol keeps
its **Boost 1.89.0** requirement despite system Boost 1.92; the existing pinned
FetchContent path downloads the required version. No dependency pin was relaxed.

The initial `Access is denied` extraction failures were directory-watcher locks,
not ACL errors. A controlled Vite reproduction watched native dependency
directories and made their rename fail with `EPERM`; closing the watcher made
the identical rename succeed. Shared `nativeWatchIgnored` in
`tooling/frontend/config.ts` excludes app-root `cmake-build-*`, `third-party`,
`refs`, and `extensions` trees from both Vite configurations. Frontend source and
shared design-system files remain watched. Restart existing dev servers after
changing watcher configuration so previously opened handles are released.

Terra's Windows debug build, runtime staging, and six native tests now pass after
correcting the Win32 call sites and renderer declarations. The native renderer
requires Windows 10 version 1607 or newer because it directly imports
`GetDpiForWindow`; its `WINVER` and `_WIN32_WINNT` definitions target Windows 10.
Hardware streaming, release builds, and Linux execution remain unverified. Sol's
development debug build now compiles and links, including `test_sol.exe`.
The full launcher was verified with live backend/frontend responses, Terra's
rendered UI reporting Core ready, and desktop-close shutdown releasing every
development port while leaving the installed Sunshine process running.

Sol's imported WiX CMake code restores tools into the build-local `.wix`
directory during configure. Reinstalling an already installed tool produced a
misleading NuGet "version not found" error. Configure now installs only when the
local executable is absent and verifies the exact pinned release before adding
extensions. A mismatched or broken executable fails explicitly. Repeated real
configure/build runs passed; no alternate WiX version or feed was introduced.

Do not relax the Boost pin, substitute toolchains, disable required features, or
invent missing-tool workarounds. Report the actual blocker; dependency installation
or environment repair requires authorization. Existing pinned FetchContent
behavior remains intact and may require network access.

### Linux

Provide Node.js/npm at the versions above, CMake 3.24+, Ninja, CTest, a C++23
compiler, and app development dependencies. `CC` and `CXX` may select explicit
compiler executables. The wrapper invokes tools directly without MSYS2.

Terra requires OpenSSL and, for its default Linux media configuration, pkg-config,
SDL2 2.0.16+, FFmpeg libavcodec 59+, libavutil 57+, libswresample 4+, GTK 3.22+,
WebKitGTK 4.1 API 2.40+, Wayland client/protocols, and wayland-scanner. Optional
libva/libva-x11/X11 enable direct VAAPI presentation; libplacebo 7+, Vulkan, and
libavformat 59+ enable the corresponding HDR/4:4:4 renderer. Provide Sol's
host-specific dependencies according to its CMake checks and
[app documentation](../apps/sol/docs). Runtime capture, audio, GPU drivers,
and input permissions are additional requirements, not supplied by npm.

The local NixOS WSL environment lacks Node.js, CMake, Ninja, and a compiler.
These dependencies need installation before local validation; no Linux configure,
build, or streaming success is claimed. Do not silently install tools, use Windows
binaries as substitutes, or disable features to bypass missing dependencies.

## Assets And Staging

Sol wrapper configure disables `SOL_BUILD_WEB_UI` and `BUILD_DOCS`.
Native-only compilation does not produce deployable web assets. Before packaging,
stage the complete frontend output in the selected native tree's `assets/web`, or
set `SOL_WEB_ASSETS_DIR` to the absolute staged web directory. Installation
and CPack require its `index.html`. If enabling the CMake `web-ui` target, root
npm installation is a prerequisite; it calls the direct Sol workspace
build, never Nx. Under UCRT64, set `NPM` explicitly to native Windows `npm.cmd`.

Terra compiles runtime files into
`apps/terra/cmake-build-<platform>-<config>/extension/<Debug|Release>`. Wrapper `build` then
explicitly invokes `stage-extension`, publishing binaries, Windows DLLs, and Linux
runtime assets to `apps/terra/extensions/terra-core/bin`. Direct CMake builds
do not publish unless `stage-extension` is requested.

Stop Terra before staging, especially on Windows where executables may be locked.
Staging removes old generated runtime files. Debug and release compilation have
separate trees, but share one staging destination: never run their wrapper builds
concurrently. A CMake lock rejects overlapping stages; the last completed stage
selects the active runtime. Serialize staging with app launch and packaging, and
retain the required DLLs/assets in distributed bundles.

## Verification Scope

The root `Workspace` workflow checks frontend code, shared design contracts, and
workspace invariants on Windows and Linux. Native compilation, signing,
packaging, and hardware streaming need separate validation after native
prerequisites are repaired. Original product workflows remain under the imported
app directories and do not run as Supernova workflows.

Sol's web build remains uncached because upstream supports environment-
selected output directories and bundle-analysis side effects. Terra's pure web
build and deterministic frontend checks declare cache inputs. Native targets
remain uncached. Changes to ignored vendored dependency contents must be reviewed
with their Git submodule pins and validated explicitly, not inferred solely from
Nx affected-project selection.

Do not launch background development servers through an automation runner that
waits for descendant processes. During migration both Nx daemon commands and
detached Vite servers left tool invocations open. Browser visual verification was
therefore not completed; automated DOM tests are not a substitute for it.
