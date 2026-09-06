# Repository Constraints

- Support Windows and Linux only. Preserve imported unsupported-platform source;
  do not add CI support for it.
- First-party code uses C++23 and TypeScript 7. Use Node.js 26.8.1, npm 12, the root
  lockfile/workspaces, and locally installed Nx 23.2. Preserve vendored standards,
  licenses, checksums, and submodule pins.
- Read app-local instructions before editing. Reuse the shared Eclipse CSS design
  system; retain all 21 themes and the `dark` Eclipse default.
- Native roots: `apps/progenitor` and `apps/terra/native`. Use
  `tooling/native/build.mjs`; Windows requires MSYS2 UCRT64, Linux its own toolchain.
  Keep app/configuration/platform build trees separate with `cmake-build-` names.
- Terra debug/release builds share `apps/terra/extensions/eclipse-core/bin` when
  staged. Stop the app and serialize staging, launch, and packaging.
- Add or update relevant tests for behavior changes. Run only authorized checks;
  `check`, Terra `build`, and Terra `bundle` include tests. Report unrun checks and
  blockers, never inferred passes. Follow explicit no-tests/no-Nx task restrictions.
- Nx is the intended orchestrator, but automation process lifetime remains
  unresolved: graph output appeared and the tool hung. Direct workspace validation
  commands are available, not a permanent replacement or proof of an Nx fix.
- Do not install missing tools, switch toolchains, relax pins, or disable required
  features to bypass blockers without authorization. See `docs/development.md`.
- Keep app versions independent. Shared admin API/IPC contracts are not yet
  extracted; do not invent compatibility layers without a concrete consumer need.
- Preserve unsquashed source histories and upstream provenance. Never revert
  others' changes. Commit, push, or create remote resources only when requested;
  no force pushes or history rewrites.
- Never commit secrets, credentials, pairing identities, private keys, or local
  runtime state. Do not weaken TLS verification or certificate pinning.
