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

## Frontend And Checks

The expected orchestration interface is the root npm scripts backed by local Nx:

```sh
npm run check
npm run build
npm run dev:progenitor
npm run dev:terra
npm run graph
```

`check` includes tests. Root `build` runs `build:web` targets only. Terra's browser
preview intentionally reports the native core unavailable.

Nx automation currently has an unresolved process lifetime problem: graph output
appeared, but the tool invocation hung instead of completing. Do not treat that
output as a passed invocation or claim the problem is fixed.

Direct workspace scripts are also available for validation:

```sh
npm run check:workspace
npm run check --workspace apps/progenitor
npm run build --workspace apps/progenitor
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

Progenitor frontend output defaults to `apps/progenitor/build/assets/web`; Terra
frontend output is `apps/terra/build/web`. Terra's workspace `build` and `bundle`
scripts additionally configure/build native code, run checks including tests, and
invoke Neutralino packaging. They are not frontend-only validation commands.

## Native Builds

The [native wrapper reference](../tooling/native/README.md) documents the complete
interface. It installs nothing, does not invoke Nx, and keeps configure, build,
and test separate:

```sh
node tooling/native/build.mjs progenitor configure debug
node tooling/native/build.mjs progenitor build debug
node tooling/native/build.mjs progenitor test debug
node tooling/native/build.mjs terra configure debug
node tooling/native/build.mjs terra build debug
node tooling/native/build.mjs terra test debug
```

Use `release` instead of `debug` for the release configuration. Run each step only
after the preceding step succeeds, and run tests only when authorized. Nx exposes
`native:configure`, `native:build`, and `native:test` on both apps with debug as the
default and a release configuration. Native targets disable caching and arrange
configure/build/test dependencies; their presence does not resolve the automation
lifetime issue.

| App | CMake source root | Build trees |
| --- | --- | --- |
| Progenitor | `apps/progenitor` | `apps/progenitor/cmake-build-<platform>-<config>` |
| Terra | `apps/terra/native` | `apps/terra/cmake-build-<platform>-<config>` |

`platform` is `win32` or `linux`; `config` is `debug` or `release`. Never share a
build tree across apps, configurations, platforms, or toolchains. A separate
Linux checkout is still recommended to avoid mixing Node native packages. Do not run competing
operations in the same tree. Progenitor uses GoogleTest's `tests/test_sunshine`
with the build tree's `tests` directory as the working directory. Terra uses
CTest; no registered tests is an error.

### Windows

Use MSYS2 UCRT64, not MSVC or another MinGW environment. The wrapper launches
`C:\msys64\msys2_shell.cmd` with `-ucrt64` and checks tools under `/ucrt64/bin`.
`SUPERNOVA_MSYS2_SHELL` may specify an absolute path to another MSYS2 installation.
Required tools include CMake, Ninja, GCC/G++, and CTest, plus each app's native
dependencies such as OpenSSL.

The local UCRT64 environment already has CMake 4.4.2, Ninja, and GCC 16.2. The
reported Progenitor configure attempt requested **Boost 1.89.0 exactly**, while
the system provides **Boost 1.92**. CMake therefore entered its existing
FetchContent path, which failed with **access denied**. This is an unresolved
dependency/fetch blocker, not evidence that CMake or the compiler is missing, and
not a successful native build.

Terra configuration also failed during CMake extraction of the pinned
nlohmann/json 3.12.0 archive: renaming the extracted directory returned access
denied. This indicates an unresolved local extraction/filesystem problem, not a
missing compiler. Neither app's native compile or streaming behavior has been
validated in the new checkout.

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
libavformat 59+ enable the corresponding HDR/4:4:4 renderer. Provide Progenitor's
host-specific dependencies according to its CMake checks and
[app documentation](../apps/progenitor/docs). Runtime capture, audio, GPU drivers,
and input permissions are additional requirements, not supplied by npm.

The local NixOS WSL environment lacks Node.js, CMake, Ninja, and a compiler.
These dependencies need installation before local validation; no Linux configure,
build, or streaming success is claimed. Do not silently install tools, use Windows
binaries as substitutes, or disable features to bypass missing dependencies.

## Assets And Staging

Progenitor wrapper configure disables `SUNSHINE_BUILD_WEB_UI` and `BUILD_DOCS`.
Native-only compilation does not produce deployable web assets. Before packaging,
stage the complete frontend output in the selected native tree's `assets/web`, or
set `SUNSHINE_WEB_ASSETS_DIR` to the absolute staged web directory. Installation
and CPack require its `index.html`. If enabling the CMake `web-ui` target, root
npm installation is a prerequisite; it calls the direct Progenitor workspace
build, never Nx. Under UCRT64, set `NPM` explicitly to native Windows `npm.cmd`.

Terra compiles runtime files into
`apps/terra/cmake-build-<platform>-<config>/extension/<Debug|Release>`. Wrapper `build` then
explicitly invokes `stage-extension`, publishing binaries, Windows DLLs, and Linux
runtime assets to `apps/terra/extensions/eclipse-core/bin`. Direct CMake builds
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

Progenitor's web build remains uncached because upstream supports environment-
selected output directories and bundle-analysis side effects. Terra's pure web
build and deterministic frontend checks declare cache inputs. Native targets
remain uncached. Changes to ignored vendored dependency contents must be reviewed
with their Git submodule pins and validated explicitly, not inferred solely from
Nx affected-project selection.

Do not launch background development servers through an automation runner that
waits for descendant processes. During migration both Nx daemon commands and
detached Vite servers left tool invocations open. Browser visual verification was
therefore not completed; automated DOM tests are not a substitute for it.
