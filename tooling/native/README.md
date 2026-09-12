# Native builds

Run from any working directory using Node.js; no npm dependencies are needed by
the wrapper. Supported hosts: Windows MSYS2 UCRT64 and Linux only.

```text
node tooling/native/build.mjs <sol|terra|vdd> <configure|build|test> <debug|release> [--dev]
node --test tooling/native/build.test.mjs
```

Operations are separate: configure first, build second, test last. Build and test
do not silently configure or install dependencies. Sol tests run
`tests/test_sol` with the build tree's `tests` directory as its working
directory; Terra tests use CTest and fail if no tests are registered.

| App | CMake source root | Build directory |
| --- | --- | --- |
| Sol | `apps/sol` | `apps/sol/cmake-build-<win32\|linux>-<debug\|release>` |
| Terra | `apps/terra/native` | `apps/terra/cmake-build-<win32\|linux>-<debug\|release>` |
| VDD | `apps/sol/third-party/vdd` | `apps/sol/cmake-build-win32-vdd-<debug\|release>` |

## Prerequisites

- Windows: `C:\msys64\msys2_shell.cmd`, UCRT64 CMake, Ninja, GCC/G++, and CTest.
  Set `SUPERNOVA_MSYS2_SHELL` to an explicit absolute `msys2_shell.cmd` path for
  another installation. The wrapper always selects `-ucrt64` and checks tools
  under `/ucrt64/bin`; it never falls back to MSVC or another MinGW environment.
- VDD additionally requires Visual Studio 2022 17.13+ C++ Build Tools with Windows
  Driver Kit Build Tools, x64/x86 Spectre-mitigated libraries, ATL with Spectre
  mitigations, and a matching Windows SDK/WDK pair. UCRT64 CMake/Ninja orchestrate
  its existing WDK/MSBuild project; GCC never compiles driver sources. VDD is
  Windows-only and remains separate from normal Sol builds.
- Linux: CMake 3.24+, Ninja, CTest, and a C++23 compiler on PATH. `CC` and `CXX`
  can select explicit compiler executables. No MSYS2 or command shell is used.
- Initialize upstream recursive submodules and provide each app's existing
  native dependencies. CMake reports missing packages; its existing dependency
  fetching remains unchanged. The wrapper installs nothing.
- Existing CMake options can be adjusted in the same build tree using CMake
  directly, with the same platform toolchain. Wrapper configure reasserts its
  generator, configuration, tests, and native-only settings.

Both Vite configurations exclude native build, vendor, and staging directories
through `tooling/frontend/config.ts`. On Windows, watching directories inside an
extraction tree can make its final rename fail with access denied. Restart old dev
servers after watcher-config changes; do not change ACLs or dependency versions
to compensate for an open watcher handle.

## Nx integration

Use the interface above from Nx run-commands targets. Set `cache: false` on every
native target. Configure/build/test dependencies belong in Nx; this wrapper and
CMake never invoke Nx. Do not schedule competing operations against the same
app/configuration build directory.

## Web assets

Sol wrapper configure sets `SOL_BUILD_WEB_UI=OFF` and
`BUILD_DOCS=OFF`. Native compilation does not need Node/npm dependencies.
With `SOL_BUILD_WEB_UI=ON`, the existing `web-ui` target calls
`npm run build --workspace apps/sol` from the repository root. Root npm
installation is a precondition, never a recurring CMake build step. This npm
script must remain a direct web build, not an Nx alias. On Windows, explicitly
set `NPM` to native Windows `npm.cmd` when building web assets from UCRT64.

Frontend can instead build independently through Nx. Before packaging, stage
its complete output in the selected build tree's `assets/web`, or configure
`SOL_WEB_ASSETS_DIR` to the absolute staged web directory (for example,
`apps/sol/build/assets/web`). Installation/CPack fails if that directory
has no `index.html`. Native-only compilation does not imply deployable assets.

## Terra staging

Compiled runtime output is isolated under
`apps/terra/cmake-build-<platform>-<config>/extension/<Debug|Release>`, including Windows DLLs
and Linux overlay/gamepad assets. Wrapper `build` first builds everything, then
explicitly builds `stage-extension` to populate
`apps/terra/extensions/terra-core/bin`. Direct CMake builds do not stage unless
that target is requested. Staging removes old generated runtime files so DLLs
from another configuration cannot linger.

Stop Terra before staging, especially on Windows where running executables are
locked. Do not run debug and release wrapper builds concurrently: isolated
compilation is safe, but both publish to the same runtime directory. A CMake
file lock rejects overlapping staging; the last completed stage is the active
configuration. Serialize build/stage and launch/package operations in Nx or CI.
