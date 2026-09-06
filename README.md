# Supernova

Windows and Linux game-streaming monorepo containing **Progenitor**, the streaming
host and administration UI, and **Terra**, the Eclipse streaming client.

First-party code uses **C++23** and **TypeScript 7**. Shared tooling uses **Node.js
26.8.1**, **npm 12** (pinned to 12.0.2), and locally installed **Nx 23.2.0**.
Both main UIs consume the shared Eclipse CSS design system, including all 21
themes; `dark` is the Eclipse default.

## Layout

| Path | Responsibility |
| --- | --- |
| `apps/progenitor` | Native host, administration frontend, packaging, and host tests |
| `apps/terra` | Neutralino client shell, React frontend, and native streaming extension |
| `packages/design-system` | Shared Eclipse tokens, palettes, CSS Modules, primitives, and theme state |
| `tooling/native` | Windows UCRT64/Linux native build wrapper |
| `docs` | Monorepo development, architecture, and upstream maintenance |

Only Windows and Linux are supported build, release, and CI targets. Imported
source for other platforms remains preserved, but does not imply support or CI
coverage. First-party language standards do not override vendored standards or
dependency pins.

## Setup

Install the required Node.js/npm versions, then run from the repository root:

```sh
git submodule update --init --recursive
npm ci
```

Use the root lockfile and workspace installation, not separate app installs.
Native toolchains and app dependencies are separate prerequisites; `npm ci` does
not install them. See [Development](docs/development.md) before native builds.

## Commands

Nx is the intended orchestrator, installed in this workspace rather than globally.
Root npm scripts invoke that local installation:

| Command | Purpose |
| --- | --- |
| `npm run check` | Nx lint, typecheck, and test targets |
| `npm run build` | Both frontend builds, not native packages |
| `npm run dev:progenitor` | Progenitor web development server |
| `npm run dev:terra` | Terra browser preview, without the native core |
| `npm run graph` | Nx project graph |
| `npm run check:workspace` | Direct finite workspace validation, without Nx |

**Known limitation:** Nx execution through automation has an unresolved process
lifetime problem. Graph output appeared, but the tool invocation hung. This is
not fixed or a verified successful Nx run. Direct npm workspace check/build
commands are available for validation, as documented in
[Development](docs/development.md); they are not a permanent replacement for Nx.

Windows native configuration is currently blocked by Progenitor's exact Boost
1.89.0 requirement versus installed Boost 1.92, followed by an access-denied
failure in the existing FetchContent path. Terra hit the same extraction error
with nlohmann/json. Windows CMake 4.4.2, Ninja, and GCC
16.2 are already present in UCRT64. The local NixOS WSL environment lacks Node.js,
CMake, Ninja, and a compiler; Linux builds are not locally verified.

## Project Boundaries

Progenitor and Terra retain independent application versions and release
lifecycles. Their admin API and client IPC are different contracts; a shared
contract package has not yet been extracted. See
[Architecture](docs/architecture.md) for the design system and runtime boundaries.

Original histories are preserved through unsquashed subtree imports at
Progenitor `74273db` and Terra `3333223`; the original source repositories remain
preserved. See [Upstream Maintenance](docs/upstream.md) for selective updates and
provenance. Repository: [nikolas-sturm/supernova](https://github.com/nikolas-sturm/supernova).

## Licenses

The root package declares `GPL-3.0-only`; that declaration does not relicense
imported or third-party code. Consult [Progenitor's license](apps/progenitor/LICENSE)
and [Terra's license](apps/terra/LICENSE), plus each dependency's own license and
notices. Preserve those boundaries and include required notices when distributing
bundles. Imported app READMEs retain product and upstream context; this root README
and development guide define current monorepo setup where older guidance differs.
