# Migration Verification

## Official Product Names

The current host is Sol (`apps/sol`, Nx project `sol`); the current client is
Terra. Sunshine/Progenitor and Moonlight/Eclipse are historical product names,
not additional first-party projects or CLI aliases. Historical verification and
source-checkout descriptors below retain the names used at the time; they do not
establish native build success after the rename. See
[Sol directory move](development.md#sol-directory-move) for preserved caches and
the completed local submodule relocation repair.

- `npm run check:workspace` passed after the move: lint, TypeScript checks, and
  152 tests (86 Sol, 43 Terra, 6 design-system contracts, 17 tooling tests), with
  no skipped tests. The staging regression uses temporary fixture files only;
  no native extension was built or staged into the app.
- `npm run build` passed both Nx `build:web` targets (`sol`, `terra`), with zero
  cache hits. No native configure, compilation, packaging, installation, or
  background server was run for this rename.
- The whole-directory move was verified byte-for-byte against 737 pre-existing
  tracked/untracked files before follow-up edits. Binary assets and non-English
  locales were preserved. Both histories and top-level dependency pins pass the
  provenance tests. The authorized Git relocation repaired all 47 initialized
  submodule worktree pointers; normal recursive Git operations now work with
  unchanged pins, histories, and existing submodule edits.
- A final preservation audit found 691 files byte-identical to the starting dirty
  checkout and 46 intended follow-up text edits, with no missing baseline files.
  All 36 initialized nested dependency revisions match their parent pins.
  `apps/sol/third-party/doxyconfig/doxygen-awesome-css` is uninitialized and was
  left untouched. Narrow Git ignore exceptions retain the four imported macOS
  source assets under `src_assets/macos/build`; this adds no platform support.

## Verified Locally (Windows)

- Root `npm ci` succeeds with Node.js 26.8.1 and npm 12.0.2. Audit reported zero
  vulnerabilities. npm's default lifecycle-script restrictions remain in effect.
- At bootstrap, `npm run check:workspace` passed lint and TypeScript checks for both apps and
  the design system, plus 133 tests: 78 Progenitor, 38 Terra, 5 design contracts,
  and 12 native-wrapper/workspace invariants. No tests skipped in this local run.
- Both frontend production builds succeed through direct workspace scripts.
- Provenance tests confirm both original histories are ancestors of Supernova,
  every original top-level submodule pin is retained, and remapped URLs match.
- Original source checkouts are preserved. Progenitor's project changes were
  committed as `74273db` and pushed to `webui-react-rewrite`. Its four pre-existing
  `write_file_test_*.txt` scratch files were deliberately excluded.

## Nx Follow-Up Verification

- The client exited successfully while its detached daemon remained alive. The
  automation runner continued waiting on that background process. `nx.json` now
  disables the optional daemon without disabling graph generation or task caching.
- `npx nx show projects` returned all four projects and terminated normally.
- `node --test tooling/workspace.test.mjs` passed all four tests, including project
  listing with a ten-second process timeout. Listing took 0.62 seconds locally.
- `npm run check -- --outputStyle=static` passed all ten Nx tasks in 5.5 seconds
  (130 tests plus lint/typechecks). No daemon remained afterward.
- Repeating the design-system typecheck reused its Nx cache result (1/1 hit).

## Not Verified

- Build-artifact restoration from cache and remote Nx execution.
- Progenitor native compilation/tests, release builds, hardware streaming, and
  packaging; Terra's Windows debug build/test results are recorded below.
- Local Linux compilation: NixOS WSL lacks the required compiler, CMake, Ninja,
  and Node.js. No substitute toolchain was used.
- Desktop/mobile visual browser checks: detached Vite server processes also kept
  automation calls open, so they were stopped. DOM tests cover interactions but
  do not establish visual correctness.

The root Windows/Linux CI workflow runs frontend checks and builds, not native
streaming validation. See GitHub Actions for its actual status after publication.

## Windows Extraction Follow-Up

- Directory ACLs allowed modification; the old extracted JSON directory could be
  renamed without changing permissions. The exact failed CMake extraction script
  then succeeded with background watchers stopped.
- A finite in-process Vite reproduction watched five native directories. Rename
  returned `EPERM` while watching, then succeeded immediately after watcher close.
  No HTTP server or detached process was launched for this reproduction.
- Both apps now consume shared native-directory watcher exclusions. Regression
  coverage checks both config integrations, path matching, and an actual rename
  with Vite watching frontend source but ignoring native extraction directories.
- `npm run check:workspace` now passes all 137 tests plus lint and typechecks,
  including the three new watcher regressions. The shared frontend tooling is
  represented by its own Nx project so both apps depend on its changes.
- Terra's UCRT64 debug configure succeeded. Its build progressed through dependency
  compilation and into first-party code, then failed on unrelated Windows issues:
  `LoadCursorW`/`IDC_ARROW` string-type mismatch in `input_forwarder.cpp:509`,
  duplicate IID arguments to `IMMDevice::Activate` in `audio_renderer.cpp:337`,
  and missing AV1 decoder-profile / `GetDpiForWindow` declarations in
  `video_renderer.cpp:189,572`. These initial compilation failures are resolved
  by the follow-up below. No codecs or platform features were disabled.
- Progenitor's pinned Boost, JSON, and FFmpeg dependencies downloaded/extracted
  successfully. Missing libvirtualhid nested submodules were initialized at their
  recorded revisions. The next configure hit a separate WiX 4.0.4 NuGet restore
  failure. Upstream CMake had restored WiX into the build-local `.wix` directory
  on the first attempt; system-wide toolchains and package versions were unchanged.

## Terra Compilation Follow-Up

- Corrected the wide-character arrow cursor resource and the four-argument
  `IMMDevice::Activate` call. AV1 uses the canonical DXVA Profile0 GUID locally
  because the installed UCRT64 headers/import libraries lack the D3D11 symbol.
- Updated `WINVER` and `_WIN32_WINNT` to Windows 10, matching the existing static
  `GetDpiForWindow` call (requires Windows 10 version 1607+). Added a declaration
  baseline regression test; AV1, HDR, and other media features remain enabled.
- `node tooling/native/build.mjs terra build debug` completed compilation,
  linking, DLL collection, and extension staging successfully.
- `node tooling/native/build.mjs terra test debug` passed all six CTest tests:
  stream session lifecycle, network route, display mode, Wake-on-LAN, mDNS
  discovery, and stream statistics.
- Existing compiler warnings and CMake CMP0207 runtime-path warnings remain.
  These checks do not establish hardware streaming or release-package correctness.
