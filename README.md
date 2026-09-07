# Supernova

Windows and Linux game-streaming monorepo containing **Sol**, the streaming
host and administration UI, and **Terra**, the streaming client.

Sol replaces Sunshine/Progenitor product branding; Terra replaces Moonlight/Eclipse.
Upstream dependencies and attribution retain their original names.

First-party code uses **C++23** and **TypeScript 7**. Shared tooling uses **Node.js
26.8.1**, **npm 12** (pinned to 12.0.2), and locally installed **Nx 23.2.0**.
Both main UIs consume the shared Terra CSS design system, including all 21
themes; `dark` is the Terra default.

## Layout

| Path | Responsibility |
| --- | --- |
| `apps/sol` | Native host, administration frontend, packaging, and host tests |
| `apps/terra` | Neutralino client shell, React frontend, and native streaming extension |
| `packages/design-system` | Shared Terra tokens, palettes, CSS Modules, primitives, and theme state |
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
The root install hook restores Terra's pinned Neutralino desktop runtime as well
as npm dependencies. This requires access to GitHub releases. If install scripts
were disabled, run `npm run setup:terra` explicitly. Close Terra before reinstalling.
Native compilers, SDKs, and OS development packages are still separate prerequisites;
see [Development](docs/development.md) before native builds.

## Commands

Nx is the intended orchestrator, installed in this workspace rather than globally.
Most root npm scripts invoke that local installation. The full development session
uses a foreground supervisor for ordered native builds and process cleanup:

| Command | Purpose |
| --- | --- |
| `npm run dev` | Start Sol at `https://localhost:47990` and Terra desktop; watch frontend changes |
| `npm run check` | Nx lint, typecheck, and test targets |
| `npm run build` | Both frontend builds, not native packages |
| `npm run dev:sol` | Build/start Sol backend and HTTPS web UI on 47990 |
| `npm run dev:terra` | Build/start Terra native desktop app, including its core |
| `npm run graph` | Nx project graph |
| `npm run check:workspace` | Direct checks plus tooling and Nx exit regression tests |

`npm run dev` requires native toolchains and the pinned Neutralino shell to be
installed first. It fails rather than downloading missing tools or bypassing
native build errors. See [Full Local Session](docs/development.md#full-local-session).

Nx's optional daemon is disabled in `nx.json`: the automation runner waits on
surviving detached processes even after the Nx client exits. Graph generation
and task caching remain available without the daemon. See
[Development](docs/development.md) for the diagnosis and direct workspace commands.

Windows extraction failures were traced to directory watchers locking native build
trees. Shared Vite watcher exclusions now prevent those locks. Before the naming
migration, Terra's UCRT64 debug build and six native tests passed. Sol and Terra
now build and start through `npm run dev`; version-checked WiX reuse fixes the
repeat-configure failure. Windows live startup, Terra's Core ready UI, and
desktop-close shutdown have been verified. Full streaming remains unverified. See
[Migration Verification](docs/migration-status.md). Windows CMake 4.4.2, Ninja,
and GCC 16.2 are present in UCRT64. Local NixOS WSL lacks Node.js, CMake, Ninja,
and a compiler; Linux builds are not locally verified.

## Project Boundaries

Sol and Terra retain independent application versions and release
lifecycles. Their admin API and client IPC are different contracts; a shared
contract package has not yet been extracted. See
[Architecture](docs/architecture.md) for the design system and runtime boundaries.

Original histories are preserved through unsquashed subtree imports at
Progenitor (now Sol) `74273db` and Terra `3333223`; the original source repositories remain
preserved. See [Upstream Maintenance](docs/upstream.md) for selective updates and
provenance. Repository: [nikolas-sturm/supernova](https://github.com/nikolas-sturm/supernova).

## Licenses

The root package declares `GPL-3.0-only`; that declaration does not relicense
imported or third-party code. Consult [Sol's license](apps/sol/LICENSE)
and [Terra's license](apps/terra/LICENSE), plus each dependency's own license and
notices. Preserve those boundaries and include required notices when distributing
bundles. Imported app READMEs retain product and upstream context; this root README
and development guide define current monorepo setup where older guidance differs.
