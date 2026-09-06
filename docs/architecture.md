# Architecture

## Applications And Contracts

Progenitor is the native streaming host with a React administration frontend.
Terra is the Eclipse client: Neutralino hosts its React UI, while its native
extension owns host control, Moonlight transport, decoding, rendering, audio, and
high-rate input. Media and high-rate input stay native; IPC carries commands,
state, and telemetry.

Apps retain independent versions and release lifecycles. The root npm package
version is workspace metadata, not a synchronized application release number.
Shared tooling does not require either app to ship when the other changes.

Shared contracts have **not** yet been extracted. Progenitor's administration API
and Terra's versioned, validated Neutralino IPC serve different boundaries. Do
not pretend they are one protocol or create aliases merely to make types look
shared. Extract a contract only when both consumers have a concrete shared need,
with explicit ownership, validation, and compatibility rules.

## Eclipse Design System

[`@supernova/design-system`](../packages/design-system/README.md) is shared source,
compiled by each app's existing Vite pipeline. It supplies CSS semantic
`--color-*` tokens, spacing, typography, angular radii, palettes, CSS Modules,
primitives, branding, and theme state. No separate package build or runtime
stylesheet injection is needed. App-specific routing, translations, native
behavior, and administration adapters stay in their apps.

Both main UIs use all **21 concrete themes**:

| Group | Theme identifiers |
| --- | --- |
| Dark (9) | `dark`, `dracula`, `mocha`, `ember`, `rose-pine`, `moonlight`, `slate`, `midnight`, `nord` |
| Light (12) | `light`, `alucard`, `latte`, `ember-light`, `rose-pine-dawn`, `sunshine`, `indigo`, `ocean`, `forest`, `rose`, `lavender`, `monochrome` |

`dark` is the acid/violet Eclipse default. `auto` is an additional preference that
follows the OS, not a 22nd palette. Existing `data-theme` identifiers and the
`localStorage.theme` preference remain compatible. Missing or invalid preferences
resolve to `dark`; storage denial still permits in-memory selection. Same-origin
windows track storage events, but the two apps' different origins do not share
preferences automatically.

Terra's stream overlay deliberately remains a fixed dark, translucent capture
surface. It does not initialize the application theme store. Overlay transparency,
opacity, charts, and native session behavior stay app-local.

## Platform And Dependency Boundaries

First-party C++23 and TypeScript 7 are the monorepo standards. Vendored code keeps
its upstream language standards, license notices, hashes, and revision pins.
Recursive submodules remain dependencies, not editable first-party packages.

Windows UCRT64 and Linux are the supported targets. Retained macOS, FreeBSD, or
other upstream platform source is historical/vendor context, not a promise of
builds, releases, or CI support. Do not remove it merely to narrow the support
matrix, and do not extend CI to unsupported platforms.

Native source roots, configuration-specific build trees, and Terra's shared
staging destination are documented in [Development](development.md). Native
build outputs must not be treated as portable Nx cache artifacts across operating
systems or toolchains.

## License Boundaries

Consult [Progenitor's license](../apps/progenitor/LICENSE) and
[Terra's license](../apps/terra/LICENSE) for application terms. The root package's
`GPL-3.0-only` metadata does not overwrite dependency licenses. Moonlight Qt,
moonlight-common-c, and all other vendored/submodule dependencies retain their own
license files and attribution requirements. Review the complete binary dependency
closure and required notices before packaging or distribution.
