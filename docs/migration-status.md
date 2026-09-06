# Migration Verification

## Verified Locally (Windows)

- Root `npm ci` succeeds with Node.js 26.8.1 and npm 12.0.2. Audit reported zero
  vulnerabilities. npm's default lifecycle-script restrictions remain in effect.
- `npm run check:workspace` passes lint and TypeScript checks for both apps and
  the design system, plus 133 tests: 78 Progenitor, 38 Terra, 5 design contracts,
  and 12 native-wrapper/workspace invariants. No tests skipped in this local run.
- Both frontend production builds succeed through direct workspace scripts.
- Provenance tests confirm both original histories are ancestors of Supernova,
  every original top-level submodule pin is retained, and remapped URLs match.
- Original source checkouts are preserved. Progenitor's project changes were
  committed as `74273db` and pushed to `webui-react-rewrite`. Its four pre-existing
  `write_file_test_*.txt` scratch files were deliberately excluded.

## Not Verified

- Nx orchestration and cache restoration: graph output appeared, but automation
  waited on process lifetime. Further Nx retries are suspended at the user's
  direction. This is unresolved, not a successful orchestration check.
- Native compilation, native application tests, streaming, and packaging:
  Windows CMake dependency extraction failed with access denied for Boost and
  nlohmann/json. Dependency pins and required media features were not relaxed.
- Local Linux compilation: NixOS WSL lacks the required compiler, CMake, Ninja,
  and Node.js. No substitute toolchain was used.
- Desktop/mobile visual browser checks: detached Vite server processes also kept
  automation calls open, so they were stopped. DOM tests cover interactions but
  do not establish visual correctness.

The root Windows/Linux CI workflow runs frontend checks and builds, not native
streaming validation. See GitHub Actions for its actual status after publication.
