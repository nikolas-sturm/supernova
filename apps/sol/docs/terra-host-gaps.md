# Terra Host API Remaining Work

This document records work still required for Sol to satisfy the Terra host contract. The retained
`/eclipse/v1`, `Eclipse*`, `X-Eclipse-*`, and `eclipse_*` wire and persistence names are intentional
compatibility contracts and do not need renaming.

## Displays And Recovery

- Verify startup reconciliation against real Windows sandbox, display, virtual-display, peripheral,
  and process providers.

## Acceptance Verification

- Add direct route tests for every success, malformed-input, unauthenticated, unauthorized,
  unavailable-provider, stale-revision, replay, and lifecycle-conflict path.
- Add scope, allowlist, ownership, expiry, disable, unpair, certificate-rotation, and SSE replay
  coverage for every domain.
- Add active-stream telemetry snapshot and one-second SSE cadence coverage.
- Add Windows restart and provider integration coverage for valid records, malformed records,
  interrupted operations, external resource changes, revocation, orphan cleanup, and adoption.
- Run native `check` and `test_sol` on supported Windows and Linux toolchains.
- Verify stock Moonlight pairing, discovery, catalog, launch, resume, cancel, streaming, and input
  remain compatible.
- Measure changed-code coverage and close gaps toward repository's 100 percent target.
